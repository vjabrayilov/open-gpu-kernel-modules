// SPDX-License-Identifier: GPL-2.0
/*
 * Tempo GPU: TSG and Channel Management
 *
 * Time Slice Groups (TSGs) are the GPU's unit of scheduling.
 * Each TSG contains one or more channels (hardware command queues).
 * The GPU's hardware scheduler cycles through TSGs on the runlist,
 * giving each TSG a time slice before switching to the next.
 *
 * In Tempo GPU, each VM gets exactly one TSG. The TSG's channels
 * carry the VM's compute and copy commands. By controlling which
 * TSGs appear on the runlist, we control which VM runs on the GPU.
 *
 * TSG lifecycle in NVIDIA hardware:
 *   1. Allocate via RM API: NvRmAlloc(KEPLER_CHANNEL_GROUP_A)
 *      → Creates TSG metadata, assigns tsg_id, allocates context save area
 *   2. Add channels: NvRmAlloc(KEPLER_CHANNEL_GPFIFO_A/B/C)
 *      → Each channel gets a pushbuffer, USERD, Instance Block
 *   3. Place on runlist: kfifoRunlistUpdate()
 *      → TSG becomes eligible for GPU execution
 *   4. Remove from runlist (preempt): kfifoRunlistUpdate() or NV_PFIFO_PREEMPT
 *   5. Destroy: NvRmFree()
 *
 * Phase 1 approach:
 *   For the prototype, we manage TSG metadata ourselves and interface
 *   with the GPU hardware directly (register writes). Production code
 *   should route through the NVIDIA RM API for GSP compatibility.
 *
 * Preemption:
 *   NV_PFIFO_PREEMPT register (0x2634) allows preempting a specific TSG.
 *   Write tsg_id | PREEMPT_TYPE_TSG, then poll IS_PENDING until done.
 *   Measured latency: 50–750μs depending on GPU workload state.
 *   At kernel boundaries (GPU pipeline drained): 5–50μs.
 *
 * Sources:
 *   - nvdebug: preempt_tsg() in runlist.c
 *   - GCAPS (ECRTS '24): TSG-level preemptive scheduling
 *   - NVIDIA open-gpu-doc: dev_fifo.ref.txt PREEMPT register
 */

#include "tempo_gpu.h"
#include <linux/delay.h>

/* ──────────────────────────────────────────────────────────────────
 * TSG Subsystem Init / Exit
 * ────────────────────────────────────────────────────────────────── */

int tempo_tsg_init(struct tempo_gpu_state *g)
{
    int i;

    for (i = 0; i < TEMPO_MAX_TSGS; i++) {
        g->tsgs[i].tsg_id    = i;
        g->tsgs[i].owning_vm = -1;
        g->tsgs[i].allocated = false;
        g->tsgs[i].on_runlist = false;
        g->tsgs[i].num_channels = 0;

        /* Set large HW timeslice as safety net.
         * We control preemption ourselves; the HW timer is a fallback
         * in case our scheduler fails to preempt.
         * Effective timeslice = timeout × 2^scale
         *   0xFF × 2^0x0F = 255 × 32768 ≈ 8.35 seconds */
        g->tsgs[i].timeslice_timeout = TEMPO_HW_TIMESLICE_TIMEOUT;
        g->tsgs[i].timeslice_scale   = TEMPO_HW_TIMESLICE_SCALE;
    }

    g->num_tsgs = 0;

    pr_info(TEMPO_DRIVER_NAME ": TSG subsystem initialized (%d slots)\n",
            TEMPO_MAX_TSGS);
    return 0;
}

void tempo_tsg_exit(struct tempo_gpu_state *g)
{
    int i;

    /* Preempt and free any active TSGs */
    for (i = 0; i < TEMPO_MAX_TSGS; i++) {
        if (g->tsgs[i].allocated) {
            if (g->tsgs[i].on_runlist)
                tempo_preempt_tsg(g, i);
            g->tsgs[i].allocated = false;
            g->tsgs[i].on_runlist = false;
        }
    }

    pr_info(TEMPO_DRIVER_NAME ": TSG subsystem cleaned up\n");
}

/* ──────────────────────────────────────────────────────────────────
 * TSG Allocation
 *
 * Allocates a TSG slot and assigns it to a VM.
 *
 * In production, this would call into the NVIDIA RM API:
 *   pRmApi->Alloc(client, device, &handle,
 *                 KEPLER_CHANNEL_GROUP_A, &params)
 * which sends an RPC to GSP firmware to allocate the TSG hardware
 * resources (context save area in VRAM, etc.).
 *
 * For Phase 1, we just manage metadata and use the TSG ID directly
 * when constructing runlist entries.
 *
 * Returns: TSG ID (0-based) on success, negative errno on failure.
 * ────────────────────────────────────────────────────────────────── */

int tempo_tsg_alloc(struct tempo_gpu_state *g, int vm_id)
{
    int i;

    if (vm_id < 0 || vm_id >= TEMPO_MAX_VMS)
        return -EINVAL;

    for (i = 0; i < TEMPO_MAX_TSGS; i++) {
        if (!g->tsgs[i].allocated) {
            struct tempo_tsg *tsg = &g->tsgs[i];

            tsg->allocated   = true;
            tsg->owning_vm   = vm_id;
            tsg->on_runlist  = false;
            tsg->num_channels = 0;

            /* Clear all channel slots */
            memset(tsg->channels, 0, sizeof(tsg->channels));

            g->num_tsgs++;

            pr_debug(TEMPO_DRIVER_NAME ": allocated TSG %d for VM %d\n",
                     i, vm_id);

            /*
             * TODO (Phase 1 hardening):
             * Call into NVIDIA RM to allocate the actual GPU-side TSG:
             *
             *   NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS params = {
             *       .engineType = NV2080_ENGINE_TYPE_GR0,
             *   };
             *   NvRmAlloc(hClient, hDevice, &tsg->rm_handle,
             *             KEPLER_CHANNEL_GROUP_A, &params, sizeof(params));
             *
             * This allocates the context save area in VRAM and registers
             * the TSG with the GPU's firmware scheduler.
             */

            return i;
        }
    }

    pr_err(TEMPO_DRIVER_NAME ": no free TSG slots\n");
    return -ENOSPC;
}

/* ──────────────────────────────────────────────────────────────────
 * TSG Free
 *
 * Releases a TSG and all its channels back to the pool.
 * ────────────────────────────────────────────────────────────────── */

void tempo_tsg_free(struct tempo_gpu_state *g, int tsg_id)
{
    struct tempo_tsg *tsg;

    if (tsg_id < 0 || tsg_id >= TEMPO_MAX_TSGS)
        return;

    tsg = &g->tsgs[tsg_id];
    if (!tsg->allocated)
        return;

    /* Preempt if on runlist */
    if (tsg->on_runlist)
        tempo_preempt_tsg(g, tsg_id);

    /*
     * TODO: Call NvRmFree() to release RM-allocated TSG resources.
     * For Phase 1, we just clear our metadata.
     */

    tsg->allocated    = false;
    tsg->owning_vm    = -1;
    tsg->on_runlist   = false;
    tsg->num_channels = 0;
    memset(tsg->channels, 0, sizeof(tsg->channels));

    if (g->num_tsgs > 0)
        g->num_tsgs--;

    pr_debug(TEMPO_DRIVER_NAME ": freed TSG %d\n", tsg_id);
}

/* ──────────────────────────────────────────────────────────────────
 * Channel Management
 *
 * Channels are hardware command queues within a TSG. Each channel
 * has its own pushbuffer (GP_PUT/GP_GET pointers), USERD doorbell,
 * and Instance Block (page tables + context pointer).
 *
 * When the VM's guest NVIDIA driver creates a channel (e.g.,
 * cuCtxCreate → NvRmAlloc(KEPLER_CHANNEL_GPFIFO_C)), our MMIO
 * interception layer captures the channel parameters and calls
 * tempo_tsg_add_channel() to register it in our tracking.
 * ────────────────────────────────────────────────────────────────── */

int tempo_tsg_add_channel(struct tempo_gpu_state *g, int tsg_id,
                          u32 chid, u64 inst_ptr, u8 inst_target,
                          u64 userd_ptr, u8 runqueue_sel)
{
    struct tempo_tsg *tsg;
    int slot;

    if (tsg_id < 0 || tsg_id >= TEMPO_MAX_TSGS)
        return -EINVAL;

    tsg = &g->tsgs[tsg_id];
    if (!tsg->allocated)
        return -EINVAL;

    if (tsg->num_channels >= TEMPO_MAX_CHANNELS)
        return -ENOSPC;

    /* Find an unused channel slot */
    for (slot = 0; slot < TEMPO_MAX_CHANNELS; slot++) {
        if (!tsg->channels[slot].active)
            break;
    }
    if (slot >= TEMPO_MAX_CHANNELS)
        return -ENOSPC;

    tsg->channels[slot].chid         = chid;
    tsg->channels[slot].inst_ptr     = inst_ptr;
    tsg->channels[slot].inst_target  = inst_target;
    tsg->channels[slot].userd_ptr    = userd_ptr;
    tsg->channels[slot].runqueue_sel = runqueue_sel;
    tsg->channels[slot].active       = true;

    tsg->num_channels++;

    pr_debug(TEMPO_DRIVER_NAME ": TSG %d: added channel %u "
             "(inst=0x%llx userd=0x%llx)\n",
             tsg_id, chid, inst_ptr, userd_ptr);

    return 0;
}

void tempo_tsg_remove_channel(struct tempo_gpu_state *g, int tsg_id, u32 chid)
{
    struct tempo_tsg *tsg;
    int i;

    if (tsg_id < 0 || tsg_id >= TEMPO_MAX_TSGS)
        return;

    tsg = &g->tsgs[tsg_id];
    if (!tsg->allocated)
        return;

    for (i = 0; i < TEMPO_MAX_CHANNELS; i++) {
        if (tsg->channels[i].active && tsg->channels[i].chid == chid) {
            tsg->channels[i].active = false;
            memset(&tsg->channels[i], 0, sizeof(tsg->channels[i]));
            if (tsg->num_channels > 0)
                tsg->num_channels--;

            pr_debug(TEMPO_DRIVER_NAME ": TSG %d: removed channel %u\n",
                     tsg_id, chid);
            return;
        }
    }
}

/* ──────────────────────────────────────────────────────────────────
 * TSG Preemption
 *
 * Forces the GPU to stop executing a specific TSG.
 *
 * Method: Write to NV_PFIFO_PREEMPT register (0x2634):
 *   [11:0]  = TSG ID to preempt
 *   [25:24] = 1 (PREEMPT_TYPE_TSG)
 *
 * Then poll IS_PENDING bit [20] until it clears (preemption complete).
 *
 * The GPU preempts at the next preemption point:
 *   - For compute: thread-block boundary (all threads in the block
 *     must finish before context is saved)
 *   - For graphics: pixel boundary
 *   - For copy: chunk boundary
 *
 * If the TSG has no active work (pipeline is drained), preemption
 * completes almost instantly (~5μs for runlist mechanics).
 *
 * If the TSG is mid-kernel, preemption requires:
 *   1. Wait for current thread block(s) to finish
 *   2. Save register file (~256KB per SM × 108 SMs on A100)
 *   3. Flush L2 cache lines tagged to this context
 *   Total: 50–750μs measured on Ampere/Hopper
 *
 * Sources:
 *   - nvdebug: preempt_tsg() function
 *   - GCAPS paper: "runlist update delay ε = α + θ"
 *     where α = ioctl + scheduling algo, θ = GPU context switch
 *     Measured ε = 50–750μs
 * ────────────────────────────────────────────────────────────────── */

int tempo_preempt_tsg(struct tempo_gpu_state *g, u32 tsg_id)
{
    u32 preempt_val;
    int timeout_us = TEMPO_PREEMPT_TIMEOUT_US;
    ktime_t t_start, t_end;

    if (tsg_id >= TEMPO_MAX_TSGS)
        return -EINVAL;

    t_start = ktime_get();

    /*
     * Write TSG ID + type=TSG to the PREEMPT register.
     *
     * Note: On GSP-enabled GPUs, NV_PFIFO_PREEMPT may still be
     * host-accessible (it's a time-critical operation that would
     * suffer from GSP RPC latency). nvdebug confirms this register
     * is writable on Turing/Ampere from the host.
     *
     * If this doesn't work on a specific GPU, the alternative is
     * submitting a new runlist without this TSG (implicit preemption).
     */
    preempt_val = (tsg_id & PREEMPT_ID_MASK) | PREEMPT_TYPE_TSG;
    tempo_wr32(g, NV_PFIFO_PREEMPT, preempt_val);

    /* Ensure write is posted before polling */
    wmb();

    /* Poll for completion: IS_PENDING bit clears when preemption is done */
    while (timeout_us > 0) {
        u32 status = tempo_rd32(g, NV_PFIFO_PREEMPT);

        if (!(status & PREEMPT_IS_PENDING)) {
            /* Preemption complete */
            t_end = ktime_get();
            g->total_preemptions++;

            pr_debug(TEMPO_DRIVER_NAME ": preempted TSG %u in %llu μs\n",
                     tsg_id,
                     ktime_to_ns(ktime_sub(t_end, t_start)) / 1000);

            if (tsg_id < TEMPO_MAX_TSGS)
                g->tsgs[tsg_id].on_runlist = false;

            return 0;
        }

        udelay(10);
        timeout_us -= 10;
    }

    /* Timeout: preemption didn't complete within the deadline */
    pr_warn(TEMPO_DRIVER_NAME ": TSG %u preemption timed out after %d μs!\n",
            tsg_id, TEMPO_PREEMPT_TIMEOUT_US);

    /*
     * Check for scheduling errors that may have caused the timeout.
     * NV_PFIFO_INTR_SCHED_ERROR codes:
     *   0x06 = RL_ACK_TIMEOUT
     *   0x08 = RL_RDAT_TIMEOUT
     *   0x0A = CTXSW_TIMEOUT
     *   0x20 = BAD_TSG
     */
    {
        u32 sched_err = tempo_rd32(g, NV_PFIFO_INTR_SCHED_ERROR);
        u32 chsw_err  = tempo_rd32(g, NV_PFIFO_INTR_CHSW_ERROR);

        if (sched_err)
            pr_err(TEMPO_DRIVER_NAME ": SCHED_ERROR=0x%02x\n",
                   sched_err & 0xFF);
        if (chsw_err)
            pr_err(TEMPO_DRIVER_NAME ": CHSW_ERROR=0x%02x\n",
                   chsw_err & 0xFF);
    }

    return -ETIMEDOUT;
}
