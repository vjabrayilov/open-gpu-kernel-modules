// SPDX-License-Identifier: GPL-2.0
/*
 * Tempo GPU: MMIO Emulation and Register Virtualization
 *
 * This file handles all MMIO (Memory-Mapped I/O) accesses from VMs
 * to their virtual GPU device. When QEMU opens the mdev device and
 * the VM's guest NVIDIA driver reads/writes GPU "registers", those
 * accesses come through our VFIO read/write handlers and land here.
 *
 * We have three strategies for handling each register access:
 *
 *   1. PASSTHROUGH: Forward the read/write to the real GPU hardware
 *      via BAR0. Used for registers that are safe and don't need
 *      virtualization (e.g., read-only status registers).
 *
 *   2. EMULATE: Return a software-computed value for reads, or
 *      intercept and process writes without forwarding to hardware.
 *      Used for registers that need per-VM virtualization (e.g.,
 *      the VM sees its own channel IDs, not the global ones).
 *
 *   3. TRAP + FORWARD: Intercept the access, do scheduler-relevant
 *      processing, then forward to hardware. The critical case is
 *      USERD doorbell writes (GP_PUT), which are our primary signal
 *      that a VM has submitted new GPU work.
 *
 * BAR0 Layout (simplified, NVIDIA Volta/Ampere/Hopper):
 *
 *   0x000000–0x000FFF  PMC     (Master Control: boot ID, interrupts)
 *   0x001000–0x001FFF  PBUS    (Bus interface)
 *   0x002000–0x003FFF  PFIFO   (Host engine / command processor)
 *   0x009000–0x009FFF  PTIMER  (GPU timer)
 *   0x00B000–0x00BFFF  PMCR    (Clock management)
 *   0x020000–0x020FFF  PGRAPH  (Graphics/compute engine control)
 *   0x100000–0x1FFFFF  FB      (Framebuffer / memory controller)
 *   0x800000–0x9FFFFF  PCCSR   (Per-channel control/status)
 *   0xA00000–...       USERD   (User submit data, per-channel doorbells)
 *
 * The USERD region is the most performance-critical interception point.
 * When the VM writes to its channel's USERD GP_PUT offset, it means:
 *   "I just pushed new GPU commands into my pushbuffer, please execute them."
 *
 * Our interception:
 *   1. Detect the write is to USERD GP_PUT
 *   2. Identify which channel (from the offset within USERD region)
 *   3. Identify which VM owns that channel
 *   4. Forward the write to real hardware (so the GPU actually sees it)
 *   5. Notify the scheduler: this VM has pending work
 *
 * This is the "command buffer inspection" mechanism from the proposal,
 * at its simplest level: detecting that work was submitted. Phase 2
 * (Month 2) will additionally parse the pushbuffer contents to identify
 * kernel launches, memory copies, and synchronization operations.
 *
 * Security considerations:
 *   - A malicious VM could try to write to another VM's USERD or
 *     to PFIFO control registers to hijack scheduling. We must
 *     validate all MMIO write offsets against the VM's allowed
 *     register set.
 *   - PFIFO registers (runlist, preempt) are host-only and must
 *     not be accessible to any VM.
 *   - A VM should only be able to access its own channels' USERD
 *     and Instance Blocks.
 */

#include "tempo_gpu.h"

/* ──────────────────────────────────────────────────────────────────
 * BAR0 Region Boundaries
 *
 * These define the major register blocks within BAR0.
 * Used for dispatching MMIO accesses to the appropriate handler.
 * ────────────────────────────────────────────────────────────────── */

#define BAR0_PMC_START          0x000000
#define BAR0_PMC_END            0x001000
#define BAR0_PBUS_START         0x001000
#define BAR0_PBUS_END           0x002000
#define BAR0_PFIFO_START        0x002000
#define BAR0_PFIFO_END          0x004000
#define BAR0_PTIMER_START       0x009000
#define BAR0_PTIMER_END         0x00A000
#define BAR0_PGRAPH_START       0x020000
#define BAR0_PGRAPH_END         0x021000
#define BAR0_FB_START           0x100000
#define BAR0_FB_END             0x200000
#define BAR0_PCCSR_START        0x800000
#define BAR0_PCCSR_END          0xA00000

/*
 * USERD region: On modern NVIDIA GPUs, USERD can be mapped at
 * various locations depending on how the RM allocates it.
 * For our mdev emulation, we define a virtual USERD region that
 * maps to per-VM channel doorbells.
 *
 * In the real hardware, USERD locations are set per-channel via
 * the RM and can be anywhere in VRAM or system memory. For our
 * Phase 1 prototype, we use a fixed virtual region starting at
 * BAR0_USERD_START where each channel gets USERD_SIZE bytes.
 */
#define BAR0_USERD_START        0xA00000
#define BAR0_USERD_END          (BAR0_USERD_START + TEMPO_MAX_TOTAL_CHANNELS * USERD_SIZE)

/* ──────────────────────────────────────────────────────────────────
 * USERD Identification
 *
 * Given a BAR0 offset, determine if it falls in the USERD region,
 * and if so, which channel and which field within the USERD.
 * ────────────────────────────────────────────────────────────────── */

bool tempo_mmio_is_userd(struct tempo_gpu_state *g, u64 offset)
{
    return (offset >= BAR0_USERD_START && offset < BAR0_USERD_END);
}

/*
 * Extract channel ID and USERD field offset from a BAR0 offset.
 * Returns 0 on success, -1 if the offset is not in the USERD region.
 */
static int userd_decode(u64 bar0_offset, u32 *out_chid, u32 *out_field)
{
    u64 userd_off;

    if (bar0_offset < BAR0_USERD_START || bar0_offset >= BAR0_USERD_END)
        return -1;

    userd_off  = bar0_offset - BAR0_USERD_START;
    *out_chid  = (u32)(userd_off / USERD_SIZE);
    *out_field = (u32)(userd_off % USERD_SIZE);

    return 0;
}

/*
 * Find which VM owns a given channel ID.
 * Returns the VM pointer or NULL if the channel is not tracked.
 */
static struct tempo_vm *find_vm_for_channel(struct tempo_gpu_state *g, u32 chid)
{
    int t, c;

    for (t = 0; t < TEMPO_MAX_TSGS; t++) {
        struct tempo_tsg *tsg = &g->tsgs[t];

        if (!tsg->allocated)
            continue;

        for (c = 0; c < TEMPO_MAX_CHANNELS; c++) {
            if (tsg->channels[c].active && tsg->channels[c].chid == chid) {
                int vm_id = tsg->owning_vm;
                if (vm_id >= 0 && vm_id < g->num_vms)
                    return g->vms[vm_id];
                return NULL;
            }
        }
    }

    return NULL;
}

/* ──────────────────────────────────────────────────────────────────
 * Register Access Policy
 *
 * Classify each BAR0 region into access policies for VMs.
 * ────────────────────────────────────────────────────────────────── */

enum mmio_policy {
    MMIO_PASSTHROUGH,   /* Forward to real HW */
    MMIO_EMULATE,       /* Return software value / process locally */
    MMIO_DENY,          /* Block access (security-sensitive registers) */
    MMIO_TRAP_FORWARD,  /* Intercept + forward (scheduling signals) */
};

static enum mmio_policy classify_read(u64 offset)
{
    /* PMC: read-only ID registers — safe to passthrough */
    if (offset >= BAR0_PMC_START && offset < BAR0_PMC_END)
        return MMIO_PASSTHROUGH;

    /* PFIFO: sensitive scheduling registers — deny VM access */
    if (offset >= BAR0_PFIFO_START && offset < BAR0_PFIFO_END)
        return MMIO_EMULATE;  /* Return safe emulated values */

    /* PTIMER: GPU timestamp — safe to passthrough */
    if (offset >= BAR0_PTIMER_START && offset < BAR0_PTIMER_END)
        return MMIO_PASSTHROUGH;

    /* PGRAPH: compute engine — passthrough for now */
    if (offset >= BAR0_PGRAPH_START && offset < BAR0_PGRAPH_END)
        return MMIO_PASSTHROUGH;

    /* FB: memory controller — emulate for VRAM size queries */
    if (offset >= BAR0_FB_START && offset < BAR0_FB_END)
        return MMIO_EMULATE;

    /* PCCSR: per-channel status — passthrough (read-only) */
    if (offset >= BAR0_PCCSR_START && offset < BAR0_PCCSR_END)
        return MMIO_PASSTHROUGH;

    /* USERD: doorbell reads — passthrough */
    if (offset >= BAR0_USERD_START && offset < BAR0_USERD_END)
        return MMIO_PASSTHROUGH;

    /* Unknown region — emulate as zero */
    return MMIO_EMULATE;
}

static enum mmio_policy classify_write(u64 offset)
{
    /* PMC: control registers — deny writes from VMs */
    if (offset >= BAR0_PMC_START && offset < BAR0_PMC_END)
        return MMIO_DENY;

    /* PFIFO: runlist/preempt — absolutely deny from VMs */
    if (offset >= BAR0_PFIFO_START && offset < BAR0_PFIFO_END)
        return MMIO_DENY;

    /* PTIMER: timer registers — deny writes */
    if (offset >= BAR0_PTIMER_START && offset < BAR0_PTIMER_END)
        return MMIO_DENY;

    /* PGRAPH: compute engine — deny for now (Phase 1 safety) */
    if (offset >= BAR0_PGRAPH_START && offset < BAR0_PGRAPH_END)
        return MMIO_DENY;

    /* FB: memory controller — deny writes */
    if (offset >= BAR0_FB_START && offset < BAR0_FB_END)
        return MMIO_DENY;

    /* PCCSR: channel enable/disable — trap and validate */
    if (offset >= BAR0_PCCSR_START && offset < BAR0_PCCSR_END)
        return MMIO_TRAP_FORWARD;

    /* USERD: doorbell writes — TRAP (primary scheduling signal!) */
    if (offset >= BAR0_USERD_START && offset < BAR0_USERD_END)
        return MMIO_TRAP_FORWARD;

    /* Unknown — deny */
    return MMIO_DENY;
}

/* ──────────────────────────────────────────────────────────────────
 * PFIFO Register Emulation (Reads)
 *
 * VMs may read PFIFO registers during driver initialization.
 * We return safe emulated values that don't leak host state
 * or other VMs' scheduling information.
 * ────────────────────────────────────────────────────────────────── */

static u32 emulate_pfifo_read(struct tempo_gpu_state *g,
                              struct tempo_vm *vm, u64 offset)
{
    u32 reg = (u32)(offset - BAR0_PFIFO_START);

    switch (offset) {
    case 0x002100:  /* NV_PFIFO_INTR_0 */
        return 0;   /* No pending FIFO interrupts for this VM */

    case 0x002254:  /* NV_PFIFO_RUNLIST — read returns last submitted */
        return 0;   /* VM doesn't see the host runlist */

    default:
        /*
         * Unknown PFIFO register: return 0.
         * The guest driver may probe various registers during init;
         * returning 0 is safe (means "feature not present" or
         * "no pending status" for most registers).
         */
        pr_debug(TEMPO_DRIVER_NAME ": VM %d read PFIFO reg 0x%03x → 0\n",
                 vm->vm_id, reg);
        return 0;
    }
}

/* ──────────────────────────────────────────────────────────────────
 * FB Register Emulation (Reads)
 *
 * The VM's guest driver queries FB (framebuffer/memory controller)
 * registers to determine VRAM size and configuration. We return
 * the VM's partition size instead of the physical GPU VRAM size.
 * ────────────────────────────────────────────────────────────────── */

/*
 * NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE — used by the NVIDIA driver to
 * determine addressable VRAM. We return a value encoding the VM's
 * VRAM partition size.
 *
 * Format: [31:16] = size in 64KB units (approximate)
 *         [15:0]  = base address in 64KB units
 */
#define NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE  0x00100CE0

static u32 emulate_fb_read(struct tempo_gpu_state *g,
                           struct tempo_vm *vm, u64 offset)
{
    if (offset == NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE) {
        u32 size_64k = (u32)(vm->vram_size >> 16);
        u32 base_64k = (u32)(vm->vram_base >> 16);
        return (size_64k << 16) | (base_64k & 0xFFFF);
    }

    /* Other FB registers: passthrough to real HW is usually safe
     * for reads, but for isolation we return 0 by default. */
    pr_debug(TEMPO_DRIVER_NAME ": VM %d read FB reg 0x%06llx → 0\n",
             vm->vm_id, offset);
    return 0;
}

/* ──────────────────────────────────────────────────────────────────
 * USERD Doorbell Write Handler
 *
 * THIS IS THE CRITICAL SCHEDULING PATH.
 *
 * When a VM writes to its channel's USERD GP_PUT field, it means
 * the VM has pushed new GPU commands into the pushbuffer. This is
 * our signal to:
 *   1. Mark the VM as having pending work (state → READY)
 *   2. If the GPU is idle, schedule this VM immediately
 *   3. If a lower-priority VM is running, trigger preemption
 *
 * Performance note:
 *   This handler is on the critical path for every GPU command
 *   submission. It must be as fast as possible. In production,
 *   we'd want to avoid the spin_lock in tempo_sched_notify_work()
 *   for the common case (VM is already RUNNING) and only take the
 *   lock when a state transition is needed.
 * ────────────────────────────────────────────────────────────────── */

static void handle_userd_write(struct tempo_gpu_state *g,
                               struct tempo_vm *vm,
                               u32 chid, u32 field, u32 value)
{
    if (field == USERD_GP_PUT) {
        /*
         * GP_PUT write: VM is submitting GPU work.
         *
         * Forward the write to the real GPU so the hardware PBDMA
         * can fetch the new commands from the pushbuffer.
         *
         * Then notify the scheduler that this VM has pending work.
         * The scheduler will decide whether to:
         *   - Continue running this VM (if it's already active)
         *   - Switch to this VM (if GPU is idle or running lower-prio)
         *   - Queue this VM for later (if higher-prio VM is running)
         */

        /* Forward to real HW USERD */
        /* In production: write to the mapped USERD region in VRAM/sysmem.
         * For Phase 1: we trust that the guest driver's USERD is properly
         * set up and the write will reach the GPU via normal MMIO path. */

        /* Notify scheduler */
        tempo_sched_notify_work(g, vm);

        pr_debug(TEMPO_DRIVER_NAME
                 ": VM %d ch %u GP_PUT write = 0x%08x\n",
                 vm->vm_id, chid, value);

    } else if (field == USERD_GP_GET) {
        /*
         * GP_GET is normally read-only from the host side (the GPU
         * updates it). If a VM writes to it, ignore or warn.
         */
        pr_debug(TEMPO_DRIVER_NAME
                 ": VM %d ch %u GP_GET write (ignored) = 0x%08x\n",
                 vm->vm_id, chid, value);

    } else {
        /*
         * Other USERD fields (e.g., TOP_LEVEL_GET, semaphore fields).
         * Forward to hardware without special processing.
         */
        pr_debug(TEMPO_DRIVER_NAME
                 ": VM %d ch %u USERD+0x%02x write = 0x%08x\n",
                 vm->vm_id, chid, field, value);
    }
}

/* ──────────────────────────────────────────────────────────────────
 * MMIO Read Handler
 *
 * Called from tempo_vfio_read() when the VM reads a GPU "register".
 * Dispatches based on the BAR0 region.
 * ────────────────────────────────────────────────────────────────── */

ssize_t tempo_mmio_read(struct tempo_gpu_state *g, struct tempo_vm *vm,
                        char __user *buf, size_t count, loff_t *ppos)
{
    u64 offset = *ppos;
    u32 val = 0;
    enum mmio_policy policy;

    /* Only support 4-byte (32-bit) aligned reads */
    if (count != 4 || (offset & 3))
        return -EINVAL;

    /* Bounds check against BAR0 */
    if (offset + count > g->bar0_size && offset < BAR0_USERD_END) {
        /* Allow reads to our virtual USERD region beyond physical BAR0 */
    } else if (offset + count > g->bar0_size) {
        return -EINVAL;
    }

    policy = classify_read(offset);

    switch (policy) {
    case MMIO_PASSTHROUGH:
        /* Read from real GPU hardware */
        if (offset < g->bar0_size)
            val = tempo_rd32(g, (u32)offset);
        else
            val = 0;
        break;

    case MMIO_EMULATE:
        /* Return software-computed value */
        if (offset >= BAR0_PFIFO_START && offset < BAR0_PFIFO_END)
            val = emulate_pfifo_read(g, vm, offset);
        else if (offset >= BAR0_FB_START && offset < BAR0_FB_END)
            val = emulate_fb_read(g, vm, offset);
        else
            val = 0;
        break;

    case MMIO_DENY:
        /* Should not happen for reads (we emulate instead) */
        val = 0;
        break;

    case MMIO_TRAP_FORWARD:
        /* Read with interception — just passthrough for reads */
        if (offset < g->bar0_size)
            val = tempo_rd32(g, (u32)offset);
        else
            val = 0;
        break;
    }

    if (copy_to_user(buf, &val, sizeof(val)))
        return -EFAULT;

    return count;
}

/* ──────────────────────────────────────────────────────────────────
 * MMIO Write Handler
 *
 * Called from tempo_vfio_write() when the VM writes a GPU "register".
 * This is the most security-critical path: we must prevent VMs from
 * writing to registers that could compromise scheduling, access
 * other VMs' memory, or crash the GPU.
 * ────────────────────────────────────────────────────────────────── */

ssize_t tempo_mmio_write(struct tempo_gpu_state *g, struct tempo_vm *vm,
                         const char __user *buf, size_t count, loff_t *ppos)
{
    u64 offset = *ppos;
    u32 val;
    enum mmio_policy policy;

    /* Only support 4-byte (32-bit) aligned writes */
    if (count != 4 || (offset & 3))
        return -EINVAL;

    if (copy_from_user(&val, buf, sizeof(val)))
        return -EFAULT;

    policy = classify_write(offset);

    switch (policy) {
    case MMIO_PASSTHROUGH:
        /* Direct write to GPU hardware */
        if (offset < g->bar0_size)
            tempo_wr32(g, (u32)offset, val);
        break;

    case MMIO_EMULATE:
        /* Process locally without forwarding to HW */
        pr_debug(TEMPO_DRIVER_NAME
                 ": VM %d emulated write to 0x%06llx = 0x%08x\n",
                 vm->vm_id, offset, val);
        break;

    case MMIO_DENY:
        /*
         * Blocked write. This VM is trying to access a privileged
         * register. Log it but don't forward to hardware.
         *
         * PFIFO writes (runlist, preempt) would be catastrophic if
         * a VM could issue them — it could hijack GPU scheduling.
         */
        pr_debug(TEMPO_DRIVER_NAME
                 ": VM %d DENIED write to 0x%06llx = 0x%08x\n",
                 vm->vm_id, offset, val);
        break;

    case MMIO_TRAP_FORWARD: {
        /*
         * Intercepted write — process for scheduling, then forward.
         */
        u32 chid, field;

        if (userd_decode(offset, &chid, &field) == 0) {
            /*
             * USERD write — this is the scheduling hot path.
             *
             * Security check: verify the channel belongs to this VM.
             * A malicious VM could try to ring another VM's doorbell.
             */
            struct tempo_vm *owner = find_vm_for_channel(g, chid);

            if (owner && owner->vm_id == vm->vm_id) {
                /* Channel belongs to this VM — process and forward */
                handle_userd_write(g, vm, chid, field, val);

                /* Forward to real hardware */
                if (offset < g->bar0_size)
                    tempo_wr32(g, (u32)offset, val);
            } else {
                /* Channel does NOT belong to this VM — DENY */
                pr_warn(TEMPO_DRIVER_NAME
                        ": VM %d tried to write channel %u "
                        "USERD (not owned!) — BLOCKED\n",
                        vm->vm_id, chid);
                /* Don't forward to hardware */
            }

        } else if (offset >= BAR0_PCCSR_START &&
                   offset < BAR0_PCCSR_END) {
            /*
             * PCCSR write — channel enable/disable.
             * Validate that the channel belongs to this VM before
             * forwarding.
             */
            u32 pccsr_chid = (u32)((offset - BAR0_PCCSR_START) / 8);
            struct tempo_vm *owner = find_vm_for_channel(g, pccsr_chid);

            if (owner && owner->vm_id == vm->vm_id) {
                if (offset < g->bar0_size)
                    tempo_wr32(g, (u32)offset, val);
            } else {
                pr_warn(TEMPO_DRIVER_NAME
                        ": VM %d tried to control channel %u "
                        "PCCSR (not owned!) — BLOCKED\n",
                        vm->vm_id, pccsr_chid);
            }

        } else {
            /* Other TRAP_FORWARD writes — forward after logging */
            if (offset < g->bar0_size)
                tempo_wr32(g, (u32)offset, val);
        }
        break;
    }
    }

    return count;
}
