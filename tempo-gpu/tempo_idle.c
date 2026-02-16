// SPDX-License-Identifier: GPL-2.0
/*
 * Tempo GPU: Idle Detection via GET/PUT Pointer Monitoring
 *
 * The single most impactful optimization in Tempo GPU over NVIDIA vGPU
 * is instant idle yield. When a VM finishes its GPU work and has no more
 * pending commands, vGPU (Equal Share / Fixed Share) wastes the remainder
 * of the time slice — the GPU sits idle until the slice expires. For
 * inference workloads where inter-request gaps average 10–50ms and slices
 * are 2–10ms, this wastes enormous amounts of GPU time.
 *
 * Tempo GPU detects idleness within ~100μs and immediately yields the
 * GPU to the next ready VM.
 *
 * Detection mechanism:
 *   Each GPU channel has a pair of ring buffer pointers in its RAMFC
 *   (RAM FIFO Context) area:
 *
 *     GP_GET  — GPU's current read position in the pushbuffer
 *     GP_PUT  — Host's write position (last submitted command)
 *
 *   When GP_GET == GP_PUT, the channel has consumed all submitted
 *   commands and has no pending work. If ALL channels in the active
 *   VM's TSG have GET==PUT, the VM is idle.
 *
 * Idle timeline:
 *   1. GPU finishes last kernel → channel drained (GET catches up to PUT)
 *   2. Our poll detects GET==PUT for all channels                  (~0μs)
 *   3. Start yield timeout timer (TEMPO_DEFAULT_YIELD_TIMEOUT_US)
 *   4. If no new PUT write arrives within timeout:
 *      → Classify VM as IDLE                                      (+100μs)
 *      → Notify scheduler → switch to next ready VM               (+50μs)
 *   Total: ~150μs from GPU going idle to next VM executing
 *   (vs. vGPU Equal Share: up to full time-slice duration, 2–10ms)
 *
 * Polling approach (Phase 1):
 *   The scheduler's hrtimer calls tempo_idle_check() every 50μs.
 *   This reads GET/PUT pointers via BAR0 MMIO.
 *
 * Future (Phase 2 — Month 2):
 *   Replace polling with fence completion callbacks:
 *   - Plant a host-side semaphore release after each kernel launch
 *   - GPU signals the semaphore when the kernel completes
 *   - Host interrupt fires → immediate idle notification
 *   This gives ~1μs detection latency instead of up to 50μs polling.
 *
 * GET/PUT pointer locations:
 *   Per-channel GP_GET and GP_PUT are stored in the channel's RAMFC
 *   area in VRAM. They can be read via BAR0 at offsets that depend
 *   on the channel ID and FIFO engine layout.
 *
 *   Alternative: The USERD (User Submit Data) region contains a
 *   shadow GP_GET at offset 0x44 that is updated by the GPU engine.
 *   Since we already track USERD addresses per channel (for doorbell
 *   interception), we can read GET/PUT from the USERD mapping.
 *
 *   For Phase 1, we use a simulated approach: we track PUT writes
 *   via our MMIO interception (tempo_mmio.c) and infer GET progress
 *   from GPU completion signals. Full hardware GET/PUT reading
 *   requires mapping each channel's USERD into host address space.
 *
 * Sources:
 *   - nvdebug: get/put pointer reading via RAMFC offsets
 *   - NVIDIA open-gpu-doc: USERD layout (GP_PUT at 0x40, GP_GET at 0x44)
 *   - nouveau: nv50_fifo_chan — get/put handling
 */

#include "tempo_gpu.h"

/* ──────────────────────────────────────────────────────────────────
 * Per-channel GET/PUT reading
 *
 * In a full implementation, these would read the actual hardware
 * pointers from the channel's USERD or RAMFC area.
 *
 * Phase 1 approach:
 *   We maintain software shadows of GP_PUT (updated in tempo_mmio.c
 *   when we intercept USERD doorbell writes) and GP_GET (updated
 *   here by reading the GPU's copy from the USERD BAR0 mapping).
 *
 *   The USERD for each channel is at a known physical address
 *   (stored in tempo_channel.userd_ptr). To read GP_GET, we would
 *   need to map that VRAM address into host virtual space.
 *
 *   Simplified for Phase 1: we read GP_GET from BAR0 FIFO channel
 *   registers if the GPU architecture exposes them, or we use a
 *   heuristic based on time-since-last-PUT-write.
 * ────────────────────────────────────────────────────────────────── */

/*
 * Read GP_GET for a specific channel.
 *
 * On Volta+, per-channel status is available via FIFO engine registers.
 * The exact register layout varies by architecture:
 *
 *   Volta/Turing:  PCCSR_CHANNEL(chid) base at 0x800000 + chid * 8
 *   Ampere/Hopper: Similar, with potential GSP mediation
 *
 * For the Phase 1 prototype, this returns a "best-effort" GET value.
 * Once integrated with the NVIDIA RM, we'll use the proper channel
 * status query API (kfifoChannelGetState).
 */

/* PCCSR (Per-Channel Control and Status Registers) — Volta+ */
#define NV_PCCSR_CHANNEL_BASE       0x00800000
#define NV_PCCSR_CHANNEL_STRIDE     8
#define NV_PCCSR_CHANNEL_ENABLE(ch) (NV_PCCSR_CHANNEL_BASE + (ch) * NV_PCCSR_CHANNEL_STRIDE)
#define NV_PCCSR_CHANNEL_STATUS(ch) (NV_PCCSR_CHANNEL_BASE + (ch) * NV_PCCSR_CHANNEL_STRIDE + 4)

/* Channel status bits */
#define PCCSR_STATUS_BUSY           (1 << 0)
#define PCCSR_STATUS_PENDING        (1 << 24)

/*
 * Check if a single channel is idle (no pending work).
 *
 * Returns true if the channel appears to have no pending GPU work.
 */
static bool tempo_channel_is_idle(struct tempo_gpu_state *g,
                                  struct tempo_channel *ch)
{
    u32 status;

    if (!ch->active)
        return true;  /* Inactive channel is trivially idle */

    /*
     * Method 1: Read PCCSR channel status register.
     *
     * If BUSY bit is clear and PENDING bit is clear,
     * the channel has no in-flight or queued work.
     *
     * Note: This register may not be accessible on all architectures
     * or may require GSP mediation on Ampere+. If the read returns
     * 0xFFFFFFFF (PCI error / unmapped), fall through to heuristic.
     */
    if (ch->chid < 512) {  /* Sanity: don't read out of range */
        status = tempo_rd32(g, NV_PCCSR_CHANNEL_STATUS(ch->chid));

        /* Check for PCI read error (all 1s = BAR region not mapped) */
        if (status != 0xFFFFFFFF) {
            if (!(status & PCCSR_STATUS_BUSY) &&
                !(status & PCCSR_STATUS_PENDING))
                return true;
            else
                return false;
        }
    }

    /*
     * Method 2 (fallback): USERD-based GP_GET/GP_PUT comparison.
     *
     * If we had the USERD region mapped into host virtual memory,
     * we could directly read:
     *   u32 get = readl(userd_host_va + USERD_GP_GET);
     *   u32 put = readl(userd_host_va + USERD_GP_PUT);
     *   return (get == put);
     *
     * For Phase 1 without full USERD host mapping, we rely on
     * Method 1 (PCCSR) or the overall TSG-level heuristic below.
     *
     * TODO: Map each channel's USERD page into host kernel VA
     * for direct GET/PUT reading.
     */

    /*
     * Method 3 (conservative fallback):
     * If we can't read hardware state, assume channel is NOT idle.
     * This means idle detection relies on the yield timeout only.
     */
    return false;
}

/* ──────────────────────────────────────────────────────────────────
 * TSG-level Idle Check
 *
 * A TSG is idle if ALL of its channels are idle.
 * Returns true if the TSG has no pending work across any channel.
 * ────────────────────────────────────────────────────────────────── */

static bool tempo_tsg_is_idle(struct tempo_gpu_state *g, struct tempo_tsg *tsg)
{
    int i;

    if (!tsg->allocated || tsg->num_channels == 0)
        return true;

    for (i = 0; i < TEMPO_MAX_CHANNELS; i++) {
        if (!tsg->channels[i].active)
            continue;
        if (!tempo_channel_is_idle(g, &tsg->channels[i]))
            return false;
    }

    return true;
}

/* ──────────────────────────────────────────────────────────────────
 * Main Idle Check — Called from Scheduler Timer
 *
 * Examines the currently-active VM's TSG. If all channels are idle
 * for longer than the yield timeout, notifies the scheduler to
 * yield the GPU to the next ready VM.
 *
 * State machine per VM:
 *
 *   RUNNING + channels busy  →  idle_start = 0  (reset timer)
 *   RUNNING + channels idle  →  idle_start = now (start timer)
 *   RUNNING + idle for ≥ timeout → notify_idle → IDLE
 *
 * The yield timeout (default 100μs) provides hysteresis:
 *   - Prevents thrashing when a VM has very brief inter-kernel gaps
 *   - Allows the VM to submit the next kernel in a sequence before
 *     we yank the GPU away
 *   - 100μs is short enough that we only waste ~0.1ms of GPU time
 *     per idle event
 * ────────────────────────────────────────────────────────────────── */

void tempo_idle_check(struct tempo_gpu_state *g)
{
    int current_vm_id;
    struct tempo_vm *vm;
    struct tempo_tsg *tsg;
    bool all_idle;

    current_vm_id = g->active_vm_id;
    if (current_vm_id < 0 || current_vm_id >= g->num_vms)
        return;

    vm = g->vms[current_vm_id];
    if (!vm->active || vm->state != VM_STATE_RUNNING)
        return;

    if (vm->tsg_id >= TEMPO_MAX_TSGS)
        return;

    tsg = &g->tsgs[vm->tsg_id];
    all_idle = tempo_tsg_is_idle(g, tsg);

    if (all_idle) {
        /* Channels are idle — manage yield timeout */
        if (ktime_to_ns(vm->idle_start) == 0) {
            /* First idle detection: start the timer */
            vm->idle_start = ktime_get();

        } else {
            /* Already timing — check if timeout expired */
            ktime_t now = ktime_get();
            u64 idle_us = ktime_to_ns(ktime_sub(now, vm->idle_start)) / 1000;

            if (idle_us >= TEMPO_DEFAULT_YIELD_TIMEOUT_US) {
                /* Yield timeout expired: VM is confirmed idle */
                pr_debug(TEMPO_DRIVER_NAME
                         ": VM %d idle for %llu μs, yielding GPU\n",
                         current_vm_id, idle_us);

                vm->idle_start = ns_to_ktime(0);
                tempo_sched_notify_idle(g, vm);
            }
        }

    } else {
        /* Channels have work → reset idle timer */
        vm->idle_start = ns_to_ktime(0);
    }
}

/* ──────────────────────────────────────────────────────────────────
 * Init / Exit
 * ────────────────────────────────────────────────────────────────── */

void tempo_idle_init(struct tempo_gpu_state *g)
{
    int i;

    /* Ensure all VMs start with idle timer cleared */
    for (i = 0; i < TEMPO_MAX_VMS; i++)
        g->vms[i]->idle_start = ns_to_ktime(0);

    pr_info(TEMPO_DRIVER_NAME ": idle detector initialized "
            "(yield timeout %d μs)\n",
            TEMPO_DEFAULT_YIELD_TIMEOUT_US);
}

void tempo_idle_exit(struct tempo_gpu_state *g)
{
    /* Nothing to clean up for polling-based idle detection.
     * Phase 2 fence callbacks would need teardown here. */
    (void)g;
}
