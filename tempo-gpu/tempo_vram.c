// SPDX-License-Identifier: GPL-2.0
/*
 * Tempo GPU: VRAM Partitioning and Page Table Management
 *
 * Each VM receives a dedicated VRAM partition for memory isolation.
 * Since temporal sharing means only one VM's context is active at
 * any time, the GPU's hardware context switch naturally swaps page
 * tables — so VM1's page tables (and thus memory access) are only
 * active when VM1's TSG is running. This gives us hardware-enforced
 * memory isolation "for free" with temporal sharing.
 *
 * Phase 1: Static partitioning
 *   - Total VRAM is divided equally among TEMPO_MAX_VMS slots
 *   - Each partition is 2MB-aligned (GPU large page boundary)
 *   - VM's GPU page tables only map its own partition
 *
 * Phase 4 (Month 4): Demand paging / oversubscription
 *   - Hot pages stay in VRAM, cold pages evicted to host memory
 *   - Working set tracking for intelligent eviction
 *   - Enables more VMs than VRAM can hold simultaneously
 *
 * VRAM discovery:
 *   The total VRAM size is read from the GPU. On nvidia-open, this
 *   comes from the Memory Manager (memmgrStateInitLocked → heap size).
 *   For Phase 1, we read it from BAR1 resource length or a known
 *   constant for the target GPU (A100 = 40/80 GB, H100 = 80 GB).
 *
 * GPU page tables:
 *   NVIDIA GPUs use 4-level page tables (PD3→PD2→PD1→PT) with
 *   4KB, 64KB, 2MB, and 512MB page sizes (Volta+).
 *   Each VM's channels point to an Instance Block that contains
 *   the root page directory pointer. The Instance Block is in VRAM
 *   and is part of the context that gets swapped on TSG switch.
 *
 *   In production, GPU page tables are managed through the NVIDIA RM:
 *     NvRmAlloc(FERMI_VASPACE_A) → creates a GPU virtual address space
 *     NvRmVidHeapCtrl(ALLOCATE) → maps VRAM into the address space
 *
 *   For Phase 1, we track partition metadata and rely on the guest
 *   driver's own page table setup, with our MMIO layer ensuring
 *   the guest can only map addresses within its partition.
 */

#include "tempo_gpu.h"

/* ──────────────────────────────────────────────────────────────────
 * Known VRAM sizes for target GPUs (fallback if BAR1 probe fails)
 *
 * Chip IDs from NV_PMC_BOOT_0:
 *   GA100 (A100): chip_id = 0x170
 *   GA102 (RTX 3090): chip_id = 0x172
 *   GH100 (H100): chip_id = 0x180
 *   AD102 (RTX 4090): chip_id = 0x192
 * ────────────────────────────────────────────────────────────────── */

static u64 tempo_vram_detect_size(struct tempo_gpu_state *g)
{
    u64 bar1_size;

    /*
     * Method 1: Use BAR1 resource length as a proxy.
     * BAR1 is the VRAM aperture visible to the CPU via PCIe.
     * On data-center GPUs, BAR1 may equal VRAM size (BAR1 resize).
     * On consumer GPUs, BAR1 is typically 256MB (much less than VRAM).
     */
    bar1_size = pci_resource_len(g->pdev, 1);
    if (bar1_size > 0) {
        pr_info(TEMPO_DRIVER_NAME ": BAR1 size = %llu MB\n",
                bar1_size >> 20);
    }

    /*
     * Method 2: Known GPU sizes by chip ID.
     * This is a prototype shortcut; production should query
     * the NVIDIA RM's fbGetInfo or memmgrGetTotalMemory.
     */
    switch (g->chip_id) {
    case 0x170:     /* GA100 — A100 */
        /* A100 comes in 40GB and 80GB variants.
         * Use BAR1 hint or default to 40GB. */
        if (bar1_size >= 80ULL * 1024 * 1024 * 1024)
            return 80ULL * 1024 * 1024 * 1024;
        return 40ULL * 1024 * 1024 * 1024;

    case 0x180:     /* GH100 — H100 */
        return 80ULL * 1024 * 1024 * 1024;

    case 0x172:     /* GA102 — RTX 3090 */
        return 24ULL * 1024 * 1024 * 1024;

    case 0x192:     /* AD102 — RTX 4090 */
        return 24ULL * 1024 * 1024 * 1024;

    default:
        /* Unknown GPU: use BAR1 if large enough, else 16 GB default */
        if (bar1_size >= 4ULL * 1024 * 1024 * 1024)
            return bar1_size;

        pr_warn(TEMPO_DRIVER_NAME ": unknown chip 0x%x, "
                "defaulting to 16 GB VRAM\n", g->chip_id);
        return 16ULL * 1024 * 1024 * 1024;
    }
}

/* ──────────────────────────────────────────────────────────────────
 * VRAM Subsystem Init / Exit
 * ────────────────────────────────────────────────────────────────── */

int tempo_vram_init(struct tempo_gpu_state *g)
{
    g->total_vram = tempo_vram_detect_size(g);

    pr_info(TEMPO_DRIVER_NAME ": VRAM total = %llu MB (%llu GB)\n",
            g->total_vram >> 20, g->total_vram >> 30);
    pr_info(TEMPO_DRIVER_NAME ": max %d VMs × %llu MB per partition\n",
            TEMPO_MAX_VMS,
            (g->total_vram / TEMPO_MAX_VMS) >> 20);

    return 0;
}

void tempo_vram_exit(struct tempo_gpu_state *g)
{
    /* Nothing to free for static partitioning.
     * Phase 4 will need to clean up demand-paging structures here. */
}

/* ──────────────────────────────────────────────────────────────────
 * VRAM Partition Allocation
 *
 * Assigns a fixed VRAM range [base, base+size) to a VM.
 *
 * Partition layout (example for A100 40GB, 16 VM slots):
 *   VM 0:  [0x000000000, 0x0A0000000)  = 2.5 GB
 *   VM 1:  [0x0A0000000, 0x140000000)  = 2.5 GB
 *   ...
 *   VM 15: [0x960000000, 0xA00000000)  = 2.5 GB
 *
 * Each partition is aligned to TEMPO_VRAM_ALIGN (2 MB).
 * The VM's GPU page tables will only map this range, so even if
 * a malicious guest attempts to access other VRAM, the GPU MMU
 * will fault (page not present).
 * ────────────────────────────────────────────────────────────────── */

int tempo_vram_alloc_partition(struct tempo_gpu_state *g, struct tempo_vm *vm)
{
    u64 partition_size;
    u64 base;

    if (!vm || vm->vm_id < 0 || vm->vm_id >= TEMPO_MAX_VMS)
        return -EINVAL;

    /* Divide total VRAM equally among max VM slots */
    partition_size = g->total_vram / TEMPO_MAX_VMS;

    /* Align down to 2 MB boundary */
    partition_size &= ~(TEMPO_VRAM_ALIGN - 1);

    if (partition_size == 0) {
        pr_err(TEMPO_DRIVER_NAME ": partition size is zero "
               "(total VRAM %llu, max VMs %d)\n",
               g->total_vram, TEMPO_MAX_VMS);
        return -ENOMEM;
    }

    base = (u64)vm->vm_id * partition_size;

    /* Sanity: partition must fit within VRAM */
    if (base + partition_size > g->total_vram) {
        pr_err(TEMPO_DRIVER_NAME ": VM %d partition [0x%llx, 0x%llx) "
               "exceeds VRAM 0x%llx\n",
               vm->vm_id, base, base + partition_size, g->total_vram);
        return -ENOMEM;
    }

    vm->vram_base = base;
    vm->vram_size = partition_size;

    pr_info(TEMPO_DRIVER_NAME ": VM %d VRAM partition: "
            "[0x%llx, 0x%llx) = %llu MB\n",
            vm->vm_id, base, base + partition_size,
            partition_size >> 20);

    /*
     * TODO (Phase 1 hardening):
     * Set up GPU page tables for this VM that only map
     * [vram_base, vram_base + vram_size).
     *
     * Implementation via NVIDIA RM:
     *   1. NvRmAlloc(FERMI_VASPACE_A) — create GPU virtual address space
     *   2. NvRmVidHeapCtrl(ALLOCATE, offset=vram_base, limit=vram_size)
     *      — allocate from the VM's partition only
     *   3. The Instance Block for this VM's channels will reference
     *      these page tables
     *
     * The key insight: because temporal sharing means only one VM's
     * TSG is on the runlist at a time, and the GPU's hardware context
     * switch swaps the page directory root pointer (in the Instance
     * Block), memory isolation is automatic — VM1's page tables
     * are only active when VM1 is running.
     *
     * What we need to ADDITIONALLY enforce:
     *   - Validate that the guest driver doesn't create page table
     *     entries pointing outside its partition (done in tempo_mmio.c
     *     by intercepting page table update commands)
     *   - Prevent the guest from modifying the Instance Block to
     *     point to a different page directory (intercept in MMIO layer)
     */

    return 0;
}

/* ──────────────────────────────────────────────────────────────────
 * VRAM Partition Free
 *
 * Releases a VM's VRAM partition.
 * In Phase 4 with demand paging, this would also evict any pages
 * that were swapped to host memory.
 * ────────────────────────────────────────────────────────────────── */

void tempo_vram_free_partition(struct tempo_gpu_state *g, struct tempo_vm *vm)
{
    if (!vm)
        return;

    pr_debug(TEMPO_DRIVER_NAME ": VM %d VRAM partition freed: "
             "[0x%llx, 0x%llx)\n",
             vm->vm_id, vm->vram_base, vm->vram_base + vm->vram_size);

    /*
     * TODO: Tear down VM's GPU page tables via RM API.
     * For Phase 1, just clear the metadata.
     * The VRAM contents are NOT scrubbed here (security concern:
     * should zero the partition before reallocation).
     */

    vm->vram_base = 0;
    vm->vram_size = 0;
}

/* ──────────────────────────────────────────────────────────────────
 * VRAM Address Validation
 *
 * Checks whether a GPU physical address falls within a VM's
 * allocated VRAM partition. Used by the MMIO layer to validate
 * page table entries and DMA mappings.
 * ────────────────────────────────────────────────────────────────── */

bool tempo_vram_addr_in_partition(struct tempo_vm *vm, u64 gpu_phys_addr,
                                 u64 size)
{
    if (!vm || vm->vram_size == 0)
        return false;

    /* Check: [addr, addr+size) ⊆ [vram_base, vram_base+vram_size) */
    if (gpu_phys_addr < vm->vram_base)
        return false;
    if (gpu_phys_addr + size > vm->vram_base + vm->vram_size)
        return false;

    return true;
}
