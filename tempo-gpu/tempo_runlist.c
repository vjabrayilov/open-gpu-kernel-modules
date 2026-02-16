// SPDX-License-Identifier: GPL-2.0
/*
 * Tempo GPU: Runlist Construction and Submission
 *
 * The runlist is the core scheduling primitive for NVIDIA GPUs.
 * It is an ordered array of TSG headers and channel entries stored
 * in GPU-visible memory. The GPU's Host engine (PFIFO) reads the
 * runlist to determine which TSGs to execute and in what order.
 *
 * To switch which VM is running on the GPU, we:
 *   1. Build a new runlist containing only the target VM's TSG
 *   2. Write the runlist base address to NV_PFIFO_RUNLIST_BASE
 *   3. Write the entry count + trigger bit to NV_PFIFO_RUNLIST
 *   4. The GPU preempts the current TSG and loads from the new runlist
 *
 * Runlist entry format (Volta/Ampere/Hopper, 128-bit per entry):
 *
 *   TSG Header:
 *     word0: entry_type(1) | timeslice_scale[19:16] | timeslice_timeout[31:24]
 *     word1: tsg_length[7:0]  (number of channel entries following)
 *     word2: tsgid[11:0]
 *     word3: reserved
 *
 *   Channel:
 *     word0: entry_type(0) | runqueue_sel[1] | inst_target[5:4] | userd_ptr_lo[31:8]
 *     word1: userd_ptr_hi[31:0]
 *     word2: chid[11:0] | inst_ptr_lo[31:12]
 *     word3: inst_ptr_hi[31:0]
 *
 * Sources:
 *   - nvdebug (Joshua Bakita, UNC): gv100_runlist_tsg / gv100_runlist_chan
 *   - NVIDIA open-gpu-doc: tu104/dev_fifo.ref.txt
 *   - nouveau: drivers/gpu/drm/nouveau/nvkm/engine/fifo/gv100.c
 */

#include "tempo_gpu.h"

/* ──────────────────────────────────────────────────────────────────
 * Runlist DMA Buffer Init / Exit
 * ────────────────────────────────────────────────────────────────── */

int tempo_runlist_init(struct tempo_gpu_state *g)
{
    /*
     * Allocate a DMA-coherent buffer for the runlist.
     * This buffer must be visible to both the CPU (for writing entries)
     * and the GPU (for reading via NV_PFIFO_RUNLIST_BASE).
     *
     * DMA coherent memory is uncached on the CPU side, so GPU reads
     * see our writes without explicit cache flushes.
     */
    size_t buf_size = TEMPO_RL_MAX_ENTRIES * TEMPO_RL_ENTRY_SIZE;

    g->runlist_buf = dma_alloc_coherent(&g->pdev->dev, buf_size,
                                        &g->runlist_dma, GFP_KERNEL);
    if (!g->runlist_buf) {
        pr_err(TEMPO_DRIVER_NAME ": failed to allocate runlist DMA buffer "
               "(%zu bytes)\n", buf_size);
        return -ENOMEM;
    }

    memset(g->runlist_buf, 0, buf_size);
    g->runlist_id      = 0;  /* Runlist 0 = GR/Compute engine */
    g->runlist_entries = 0;

    pr_info(TEMPO_DRIVER_NAME ": runlist buffer allocated at DMA 0x%llx "
            "(%zu bytes, %d max entries)\n",
            (u64)g->runlist_dma, buf_size, TEMPO_RL_MAX_ENTRIES);

    return 0;
}

void tempo_runlist_exit(struct tempo_gpu_state *g)
{
    if (g->runlist_buf) {
        size_t buf_size = TEMPO_RL_MAX_ENTRIES * TEMPO_RL_ENTRY_SIZE;

        dma_free_coherent(&g->pdev->dev, buf_size,
                          g->runlist_buf, g->runlist_dma);
        g->runlist_buf = NULL;
    }
}

/* ──────────────────────────────────────────────────────────────────
 * Runlist Entry Construction Helpers
 * ────────────────────────────────────────────────────────────────── */

/*
 * Fill a TSG header entry at the given buffer offset.
 * Returns the number of bytes written (always TEMPO_RL_ENTRY_SIZE).
 */
static int fill_tsg_entry(void *buf, struct tempo_tsg *tsg)
{
    struct tempo_rl_tsg_entry *entry = buf;

    memset(entry, 0, TEMPO_RL_ENTRY_SIZE);

    /* word0: entry_type=TSG(1) | timeslice_scale[19:16] | timeout[31:24] */
    entry->word0 = RL_ENTRY_TYPE_TSG
                 | ((u32)(tsg->timeslice_scale & 0xF) << 16)
                 | ((u32)(tsg->timeslice_timeout & 0xFF) << 24);

    /* word1: tsg_length = number of channel entries following this header */
    entry->word1 = tsg->num_channels & 0xFF;

    /* word2: tsgid[11:0] */
    entry->word2 = tsg->tsg_id & 0xFFF;

    /* word3: reserved */
    entry->word3 = 0;

    return TEMPO_RL_ENTRY_SIZE;
}

/*
 * Fill a channel entry at the given buffer offset.
 * Returns the number of bytes written (always TEMPO_RL_ENTRY_SIZE).
 */
static int fill_chan_entry(void *buf, struct tempo_channel *ch)
{
    struct tempo_rl_chan_entry *entry = buf;

    memset(entry, 0, TEMPO_RL_ENTRY_SIZE);

    /*
     * word0: entry_type=CHANNEL(0) | runqueue_sel[1] | inst_target[5:4]
     *        | userd_ptr_lo[31:8]  (userd_ptr >> 8, lower 24 bits)
     *
     * USERD pointer is 256-byte aligned, so bits [7:0] are always 0.
     * We store bits [31:8] of the address in word0[31:8].
     */
    entry->word0 = RL_ENTRY_TYPE_CHANNEL
                 | ((u32)(ch->runqueue_sel & 0x1) << 1)
                 | ((u32)(ch->inst_target & 0x3) << 4)
                 | ((u32)(lower_32_bits(ch->userd_ptr) >> 8) << 8);

    /* word1: userd_ptr_hi (upper 32 bits of userd address) */
    entry->word1 = upper_32_bits(ch->userd_ptr);

    /*
     * word2: chid[11:0] | inst_ptr_lo[31:12]
     *
     * Instance Block pointer is 4KB-aligned, so bits [11:0] are 0.
     * We store chid in [11:0] and inst_ptr bits [31:12] in [31:12].
     */
    entry->word2 = (ch->chid & 0xFFF)
                 | (lower_32_bits(ch->inst_ptr) & 0xFFFFF000u);

    /* word3: inst_ptr_hi (upper 32 bits of instance block address) */
    entry->word3 = upper_32_bits(ch->inst_ptr);

    return TEMPO_RL_ENTRY_SIZE;
}

/* ──────────────────────────────────────────────────────────────────
 * Runlist Build
 *
 * Constructs the in-memory runlist from a list of VM IDs.
 * Each VM contributes one TSG header + N channel entries.
 * Returns the total number of entries written.
 * ────────────────────────────────────────────────────────────────── */

int tempo_runlist_build(struct tempo_gpu_state *g, int *vm_ids, int num_vms)
{
    void *buf = g->runlist_buf;
    int offset = 0;
    int total_entries = 0;
    int i, c;

    if (!buf)
        return -EINVAL;

    for (i = 0; i < num_vms; i++) {
        int vm_id = vm_ids[i];
        struct tempo_vm *vm;
        struct tempo_tsg *tsg;

        if (vm_id < 0 || vm_id >= g->num_vms)
            continue;

        vm = g->vms[vm_id];
        if (!vm->active || vm->tsg_id >= TEMPO_MAX_TSGS)
            continue;

        tsg = &g->tsgs[vm->tsg_id];
        if (!tsg->allocated || tsg->num_channels == 0)
            continue;

        /* Safety check: don't overflow buffer */
        int entries_needed = 1 + tsg->num_channels;  /* TSG header + channels */
        if (total_entries + entries_needed > TEMPO_RL_MAX_ENTRIES) {
            pr_warn(TEMPO_DRIVER_NAME ": runlist overflow, skipping VM %d\n",
                    vm_id);
            break;
        }

        /* Write TSG header */
        offset += fill_tsg_entry(buf + offset, tsg);
        total_entries++;

        /* Write channel entries for this TSG */
        for (c = 0; c < TEMPO_MAX_CHANNELS; c++) {
            struct tempo_channel *ch = &tsg->channels[c];

            if (!ch->active)
                continue;

            offset += fill_chan_entry(buf + offset, ch);
            total_entries++;
        }

        tsg->on_runlist = true;
    }

    /* Zero the remainder of the buffer to avoid stale data */
    if (offset < TEMPO_RL_MAX_ENTRIES * TEMPO_RL_ENTRY_SIZE)
        memset(buf + offset, 0,
               TEMPO_RL_MAX_ENTRIES * TEMPO_RL_ENTRY_SIZE - offset);

    g->runlist_entries = total_entries;
    return total_entries;
}

/* ──────────────────────────────────────────────────────────────────
 * Runlist Submit
 *
 * Tells the GPU to use the runlist we just built.
 * This triggers:
 *   1. Preemption of the currently-running TSG
 *   2. GPU re-reads the runlist from DMA memory
 *   3. GPU begins executing the first TSG in the new runlist
 *
 * On GSP-enabled GPUs (Ampere+), this should ideally go through
 * kfifoRunlistUpdate_HAL() in the NVIDIA RM. For Phase 1 prototype,
 * we use direct register writes which work on pre-GSP GPUs and
 * may work on GSP GPUs for the PFIFO registers that remain
 * host-accessible.
 *
 * IMPORTANT: If direct writes don't work on GSP-enabled GPUs,
 * we fall back to hooking into the nvidia-open driver's RM API.
 * See Risk 1 in the implementation guide.
 * ────────────────────────────────────────────────────────────────── */

int tempo_runlist_submit(struct tempo_gpu_state *g, int num_entries)
{
    u32 base_lo, submit_val;
    int rl_id = g->runlist_id;

    if (num_entries < 0 || num_entries > TEMPO_RL_MAX_ENTRIES)
        return -EINVAL;

    /*
     * Step 1: Write the runlist base address register.
     *
     * NV_PFIFO_RUNLIST_BASE format:
     *   [31:0] = (runlist_dma_addr >> 12)
     *
     * The address is 4KB-aligned (low 12 bits assumed zero).
     * On Volta+ with 64-bit addressing, the upper bits may go
     * into a separate register; for simplicity we assume the
     * DMA address fits in the low 44 bits (16 TB addressable).
     */
    base_lo = lower_32_bits(g->runlist_dma >> 12);
    tempo_wr32(g, NV_PFIFO_RUNLIST_BASE(rl_id), base_lo);

    /*
     * Step 2: Write the submit register.
     *
     * NV_PFIFO_RUNLIST format:
     *   [15:0]  = number of entries in the runlist
     *   [16]    = trigger bit (writing 1 tells GPU to re-read)
     *
     * Writing this register with the trigger bit initiates:
     *   - Preemption of any currently-executing TSG on this runlist
     *   - GPU DMA-reads the new runlist from the base address
     *   - Execution begins from entry 0 of the new runlist
     */
    submit_val = (num_entries & RUNLIST_LENGTH_MASK) | RUNLIST_SUBMIT_TRIGGER;
    tempo_wr32(g, NV_PFIFO_RUNLIST(rl_id), submit_val);

    /* Memory barrier to ensure register writes are ordered */
    wmb();

    pr_debug(TEMPO_DRIVER_NAME ": submitted runlist %d: %d entries, "
             "base=0x%llx\n", rl_id, num_entries, (u64)g->runlist_dma);

    return 0;
}

/* ──────────────────────────────────────────────────────────────────
 * Convenience: Build + Submit for a single VM
 *
 * This is the primary path for context switches.
 * Builds a runlist with exactly one TSG (the target VM's) and submits.
 * ────────────────────────────────────────────────────────────────── */

int tempo_runlist_build_and_submit_single(struct tempo_gpu_state *g, int vm_id)
{
    int vm_list[1] = { vm_id };
    int num_entries;

    num_entries = tempo_runlist_build(g, vm_list, 1);
    if (num_entries <= 0) {
        pr_warn(TEMPO_DRIVER_NAME ": empty runlist for VM %d\n", vm_id);
        return -EINVAL;
    }

    return tempo_runlist_submit(g, num_entries);
}

/* ──────────────────────────────────────────────────────────────────
 * Convenience: Submit an empty runlist (GPU goes idle)
 *
 * Used when the last VM is descheduled or during shutdown.
 * Submitting an empty runlist causes the GPU to preempt the
 * current TSG and then sit idle with no work to execute.
 * ────────────────────────────────────────────────────────────────── */

int tempo_runlist_build_and_submit_empty(struct tempo_gpu_state *g)
{
    /* Zero the buffer */
    memset(g->runlist_buf, 0, TEMPO_RL_MAX_ENTRIES * TEMPO_RL_ENTRY_SIZE);
    g->runlist_entries = 0;

    /*
     * Submit with 0 entries. This is a valid operation:
     * the GPU will preempt whatever is running and then idle.
     */
    return tempo_runlist_submit(g, 0);
}
