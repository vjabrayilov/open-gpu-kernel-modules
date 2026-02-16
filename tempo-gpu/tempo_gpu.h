/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tempo GPU: Workload-Aware Temporal GPU Scheduler
 * Core data structures and hardware definitions
 *
 * Copyright (C) 2026 Tempo GPU Project
 */

#ifndef _TEMPO_GPU_H_
#define _TEMPO_GPU_H_

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/vfio.h>
#include <linux/uuid.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>

/* ──────────────────────────────────────────────────────────────────
 * Constants
 * ────────────────────────────────────────────────────────────────── */

#define TEMPO_DRIVER_NAME       "tempo-gpu"
#define TEMPO_MAX_VMS           16
#define TEMPO_MAX_TSGS          16
#define TEMPO_MAX_CHANNELS      64  /* per TSG */
#define TEMPO_MAX_TOTAL_CHANNELS 512

/* Idle detection: μs before declaring a VM idle after GET==PUT */
#define TEMPO_DEFAULT_YIELD_TIMEOUT_US  100

/* Scheduler timer period in μs (polling frequency) */
#define TEMPO_SCHED_TIMER_PERIOD_US     50

/* Runlist buffer sizing */
#define TEMPO_RL_ENTRY_SIZE     16  /* bytes per runlist entry (128-bit) */
#define TEMPO_RL_MAX_ENTRIES    256

/* Fallback hardware timeslice (set large; WE control preemption) */
#define TEMPO_HW_TIMESLICE_TIMEOUT  0xFF  /* max base value */
#define TEMPO_HW_TIMESLICE_SCALE    0x0F  /* max scale → very long HW timeslice */

/* Preemption timeout in μs */
#define TEMPO_PREEMPT_TIMEOUT_US    2000

/* VRAM alignment (2 MB GPU large page) */
#define TEMPO_VRAM_ALIGN            (2ULL * 1024 * 1024)

/* ──────────────────────────────────────────────────────────────────
 * GPU Register Offsets (Volta / Ampere / Hopper)
 *
 * Sources:
 *   - NVIDIA open-gpu-doc (tu104/dev_fifo.ref.txt)
 *   - nvdebug (Joshua Bakita, UNC Chapel Hill)
 *   - nouveau (drivers/gpu/drm/nouveau/nvkm/engine/fifo/)
 * ────────────────────────────────────────────────────────────────── */

/* BOOT0: chip identification */
#define NV_PMC_BOOT_0                   0x00000000

/* PFIFO: command processor / host engine */
#define NV_PFIFO_RUNLIST_BASE(i)        (0x00002270 + (i) * 0x10)
#define NV_PFIFO_RUNLIST(i)             (0x00002274 + (i) * 0x10)
#define NV_PFIFO_ENG_RUNLIST_BASE(i)    (0x00002280 + (i) * 0x08)

/* Preemption */
#define NV_PFIFO_PREEMPT                0x00002634
#define NV_PFIFO_RUNLIST_PREEMPT        0x00002638

/* Interrupts */
#define NV_PFIFO_INTR_0                 0x00002100
#define NV_PFIFO_INTR_SCHED_ERROR       0x0000254C
#define NV_PFIFO_INTR_CHSW_ERROR       0x0000256C

/* PTIMER: GPU timestamp (nanoseconds) */
#define NV_PTIMER_TIME_0                0x00009400
#define NV_PTIMER_TIME_1                0x00009410

/* PFIFO_PREEMPT register bitfields */
#define PREEMPT_ID_MASK         0x00000FFF  /* [11:0]  TSG/channel ID */
#define PREEMPT_IS_PENDING      (1 << 20)   /* [20]    preemption in progress */
#define PREEMPT_TYPE_CHANNEL    (0 << 24)   /* [25:24] preempt channel */
#define PREEMPT_TYPE_TSG        (1 << 24)   /* [25:24] preempt TSG */

/* Runlist submit register bitfields */
#define RUNLIST_LENGTH_MASK     0x0000FFFF
#define RUNLIST_SUBMIT_TRIGGER  (1 << 16)

/* ──────────────────────────────────────────────────────────────────
 * Runlist Entry Structures (Volta / Ampere / Hopper — 128-bit entries)
 *
 * From nvdebug: gv100_runlist_tsg / gv100_runlist_chan
 * ────────────────────────────────────────────────────────────────── */

/* Entry type field in word0 bit 0 */
#define RL_ENTRY_TYPE_CHANNEL   0
#define RL_ENTRY_TYPE_TSG       1

/*
 * TSG Header Entry (16 bytes)
 *
 * word0: [0]     entry_type = 1 (TSG)
 *        [15:1]  reserved
 *        [19:16] timeslice_scale
 *        [23:20] reserved
 *        [31:24] timeslice_timeout
 * word1: [7:0]   tsg_length (number of following channel entries)
 *        [31:8]  reserved
 * word2: [11:0]  tsgid
 *        [31:12] reserved
 * word3: reserved
 */
struct tempo_rl_tsg_entry {
    u32 word0;
    u32 word1;
    u32 word2;
    u32 word3;
} __packed;

/*
 * Channel Entry (16 bytes)
 *
 * word0: [0]     entry_type = 0 (channel)
 *        [1]     runqueue_selector
 *        [3:2]   reserved
 *        [5:4]   inst_target (aperture: 0=VID_MEM, 1/2/3=SYS_MEM variants)
 *        [7:6]   reserved
 *        [31:8]  userd_ptr_lo >> 8
 * word1: [31:0]  userd_ptr_hi
 * word2: [11:0]  chid
 *        [31:12] inst_ptr_lo >> 12
 * word3: [31:0]  inst_ptr_hi
 */
struct tempo_rl_chan_entry {
    u32 word0;
    u32 word1;
    u32 word2;
    u32 word3;
} __packed;

/* ──────────────────────────────────────────────────────────────────
 * USERD (User Submit Data) — per-channel doorbell offsets
 * ────────────────────────────────────────────────────────────────── */

#define USERD_GP_PUT            0x40
#define USERD_GP_GET            0x44
#define USERD_SIZE              0x200  /* 512 bytes per channel */

/* ──────────────────────────────────────────────────────────────────
 * VM and Scheduler State
 * ────────────────────────────────────────────────────────────────── */

enum tempo_vm_state {
    VM_STATE_IDLE       = 0,    /* No pending GPU work */
    VM_STATE_READY      = 1,    /* Has pending work, not currently on GPU */
    VM_STATE_RUNNING    = 2,    /* Currently executing on GPU */
    VM_STATE_PREEMPTED  = 3,    /* Was running, forcefully preempted */
};

enum tempo_vm_priority {
    PRIO_REALTIME       = 0,    /* Lowest number = highest priority */
    PRIO_STANDARD       = 1,
    PRIO_BESTEFFORT     = 2,
    PRIO_NUM_LEVELS     = 3,
};

/* Per-channel tracking */
struct tempo_channel {
    u32     chid;               /* Hardware channel ID */
    u64     inst_ptr;           /* Instance Block physical address */
    u64     userd_ptr;          /* USERD physical address */
    u8      inst_target;        /* Aperture for inst_ptr */
    u8      runqueue_sel;       /* Which PBDMA to use */
    bool    active;             /* Channel is allocated */
};

/* Per-TSG tracking */
struct tempo_tsg {
    u32     tsg_id;             /* Hardware TSG ID (0-4095) */
    int     owning_vm;          /* VM index that owns this TSG */
    bool    allocated;          /* TSG is in use */
    bool    on_runlist;         /* Currently in the active runlist */

    /* HW scheduling parameters (written into runlist entry) */
    u8      timeslice_timeout;
    u8      timeslice_scale;

    /* Channels belonging to this TSG */
    struct tempo_channel channels[TEMPO_MAX_CHANNELS];
    int     num_channels;
};

/* Per-VM state */
struct tempo_vm {
    /* VFIO device (embedded for mdev) */
    struct vfio_device      vfio_dev;
    int                     vm_id;
    guid_t                  uuid;
    bool                    active;

    /* Scheduling */
    enum tempo_vm_state     state;
    enum tempo_vm_priority  priority;
    u32                     tsg_id;

    /* VRAM partition */
    u64                     vram_base;
    u64                     vram_size;

    /* Timing / profiling */
    ktime_t                 last_scheduled;
    ktime_t                 last_yielded;
    ktime_t                 idle_start;     /* When GET==PUT was first seen */
    u64                     cumulative_gpu_ns;
    u64                     budget_ns;      /* Per-epoch GPU time budget */

    /* SLO tracking */
    u64                     slo_deadline_ns;/* 0 = no SLO */

    /* Statistics */
    u64                     total_switches_in;
    u64                     total_idle_yields;

};

/* ──────────────────────────────────────────────────────────────────
 * Global GPU State
 * ────────────────────────────────────────────────────────────────── */

struct tempo_gpu_state {
    /* PCI device */
    struct pci_dev          *pdev;
    void __iomem            *bar0;
    resource_size_t         bar0_size;

    /* GPU identification */
    u32                     boot0;          /* NV_PMC_BOOT_0 value */
    int                     chip_id;        /* (boot0 >> 20) & 0x1FF */

    /* VM management */
    struct tempo_vm         *vms[TEMPO_MAX_VMS];
    int                     num_vms;
    int                     active_vm_id;   /* Currently running VM, -1 if none */
    spinlock_t              sched_lock;

    /* TSG management */
    struct tempo_tsg        tsgs[TEMPO_MAX_TSGS];
    int                     num_tsgs;

    /* Runlist management */
    void                    *runlist_buf;    /* Host staging buffer (DMA coherent) */
    dma_addr_t              runlist_dma;     /* GPU-visible DMA address */
    int                     runlist_id;      /* Which HW runlist (0 = GR engine) */
    int                     runlist_entries; /* Current entry count */

    /* Scheduler */
    struct hrtimer          sched_timer;
    struct work_struct      sched_work;
    bool                    scheduler_active;

    /* VRAM info */
    u64                     total_vram;     /* Total GPU VRAM in bytes */

    /* Debugfs */
    struct dentry           *debugfs_root;

    /* Statistics */
    u64                     total_context_switches;
    u64                     total_preemptions;
    u64                     cumulative_switch_ns;
};

/* Global instance (single GPU for now) */
extern struct tempo_gpu_state *g_tempo;

/* ──────────────────────────────────────────────────────────────────
 * Inline Helpers: GPU Register Access
 * ────────────────────────────────────────────────────────────────── */

static inline u32 tempo_rd32(struct tempo_gpu_state *g, u32 reg)
{
    return ioread32(g->bar0 + reg);
}

static inline void tempo_wr32(struct tempo_gpu_state *g, u32 reg, u32 val)
{
    iowrite32(val, g->bar0 + reg);
}

/* Read GPU nanosecond timer */
static inline u64 tempo_gpu_time_ns(struct tempo_gpu_state *g)
{
    u32 hi, lo, hi2;
    do {
        hi  = tempo_rd32(g, NV_PTIMER_TIME_1);
        lo  = tempo_rd32(g, NV_PTIMER_TIME_0);
        hi2 = tempo_rd32(g, NV_PTIMER_TIME_1);
    } while (hi != hi2);
    return ((u64)hi << 32) | lo;
}

/* ──────────────────────────────────────────────────────────────────
 * Function Declarations: tempo_scheduler.c
 * ────────────────────────────────────────────────────────────────── */
void tempo_sched_init(struct tempo_gpu_state *g);
void tempo_sched_exit(struct tempo_gpu_state *g);
void tempo_sched_notify_work(struct tempo_gpu_state *g, struct tempo_vm *vm);
void tempo_sched_notify_idle(struct tempo_gpu_state *g, struct tempo_vm *vm);
int  tempo_sched_switch_to(struct tempo_gpu_state *g, int vm_id);

/* ──────────────────────────────────────────────────────────────────
 * Function Declarations: tempo_runlist.c
 * ────────────────────────────────────────────────────────────────── */
int  tempo_runlist_init(struct tempo_gpu_state *g);
void tempo_runlist_exit(struct tempo_gpu_state *g);
int  tempo_runlist_build(struct tempo_gpu_state *g, int *vm_ids, int num_vms);
int  tempo_runlist_submit(struct tempo_gpu_state *g, int num_entries);
int  tempo_runlist_build_and_submit_single(struct tempo_gpu_state *g, int vm_id);
int  tempo_runlist_build_and_submit_empty(struct tempo_gpu_state *g);

/* ──────────────────────────────────────────────────────────────────
 * Function Declarations: tempo_tsg.c
 * ────────────────────────────────────────────────────────────────── */
int  tempo_tsg_init(struct tempo_gpu_state *g);
void tempo_tsg_exit(struct tempo_gpu_state *g);
int  tempo_tsg_alloc(struct tempo_gpu_state *g, int vm_id);
void tempo_tsg_free(struct tempo_gpu_state *g, int tsg_id);
int  tempo_tsg_add_channel(struct tempo_gpu_state *g, int tsg_id,
                           u32 chid, u64 inst_ptr, u8 inst_target,
                           u64 userd_ptr, u8 runqueue_sel);
void tempo_tsg_remove_channel(struct tempo_gpu_state *g, int tsg_id, u32 chid);
int  tempo_preempt_tsg(struct tempo_gpu_state *g, u32 tsg_id);

/* ──────────────────────────────────────────────────────────────────
 * Function Declarations: tempo_vram.c
 * ────────────────────────────────────────────────────────────────── */
int  tempo_vram_init(struct tempo_gpu_state *g);
void tempo_vram_exit(struct tempo_gpu_state *g);
int  tempo_vram_alloc_partition(struct tempo_gpu_state *g, struct tempo_vm *vm);
void tempo_vram_free_partition(struct tempo_gpu_state *g, struct tempo_vm *vm);
bool tempo_vram_addr_in_partition(struct tempo_vm *vm, u64 gpu_phys_addr, u64 size);

/* ──────────────────────────────────────────────────────────────────
 * Function Declarations: tempo_idle.c
 * ────────────────────────────────────────────────────────────────── */
void tempo_idle_init(struct tempo_gpu_state *g);
void tempo_idle_exit(struct tempo_gpu_state *g);
void tempo_idle_check(struct tempo_gpu_state *g);

/* ──────────────────────────────────────────────────────────────────
 * Function Declarations: tempo_debugfs.c
 * ────────────────────────────────────────────────────────────────── */
int  tempo_debugfs_init(struct tempo_gpu_state *g);
void tempo_debugfs_exit(struct tempo_gpu_state *g);

/* ──────────────────────────────────────────────────────────────────
 * Function Declarations: tempo_mmio.c
 * ────────────────────────────────────────────────────────────────── */
ssize_t tempo_mmio_read(struct tempo_gpu_state *g, struct tempo_vm *vm,
                        char __user *buf, size_t count, loff_t *ppos);
ssize_t tempo_mmio_write(struct tempo_gpu_state *g, struct tempo_vm *vm,
                         const char __user *buf, size_t count, loff_t *ppos);
bool    tempo_mmio_is_userd(struct tempo_gpu_state *g, u64 offset);

#endif /* _TEMPO_GPU_H_ */
