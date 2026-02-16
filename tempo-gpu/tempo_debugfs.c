// SPDX-License-Identifier: GPL-2.0
/*
 * Tempo GPU: Debugfs Interface
 *
 * Provides a debug/measurement interface under /sys/kernel/debug/tempo-gpu/
 * for development, benchmarking, and verification of the scheduler.
 *
 * Files exposed:
 *
 *   status          (read-only)   Global scheduler state and GPU info
 *   vms             (read-only)   Per-VM state table
 *   stats           (read-only)   Cumulative performance statistics
 *   scheduler       (write-only)  Manual scheduling commands:
 *                                   "switch <vm_id>"  — force switch to VM
 *                                   "preempt <tsg_id>" — force preempt a TSG
 *   switch_test     (write-only)  Run N context switch iterations and
 *                                 report average latency
 *   priority        (write-only)  Set VM priority:
 *                                   "<vm_id> rt|std|be"
 *
 * Example usage:
 *
 *   # View scheduler status
 *   cat /sys/kernel/debug/tempo-gpu/status
 *
 *   # Force context switch to VM 1
 *   echo "switch 1" > /sys/kernel/debug/tempo-gpu/scheduler
 *
 *   # Run 1000-iteration context switch benchmark
 *   echo 1000 > /sys/kernel/debug/tempo-gpu/switch_test
 *
 *   # Set VM 0 to realtime priority
 *   echo "0 rt" > /sys/kernel/debug/tempo-gpu/priority
 *
 *   # View per-VM statistics
 *   cat /sys/kernel/debug/tempo-gpu/vms
 */

#include "tempo_gpu.h"
#include <linux/seq_file.h>

/* ──────────────────────────────────────────────────────────────────
 * status — Global scheduler state
 * ────────────────────────────────────────────────────────────────── */

static int tempo_debugfs_status_show(struct seq_file *s, void *unused)
{
    struct tempo_gpu_state *g = s->private;

    seq_printf(s, "Tempo GPU Scheduler Status\n");
    seq_printf(s, "==========================\n\n");

    seq_printf(s, "GPU: %s\n", pci_name(g->pdev));
    seq_printf(s, "  BOOT0:    0x%08x\n", g->boot0);
    seq_printf(s, "  Chip ID:  0x%03x\n", g->chip_id);
    seq_printf(s, "  BAR0:     %llu KB\n", (u64)g->bar0_size >> 10);
    seq_printf(s, "  VRAM:     %llu MB\n", g->total_vram >> 20);
    seq_printf(s, "\n");

    seq_printf(s, "Scheduler:\n");
    seq_printf(s, "  Active:     %s\n",
               g->scheduler_active ? "yes" : "no");
    seq_printf(s, "  Active VM:  %d\n", g->active_vm_id);
    seq_printf(s, "  Total VMs:  %d / %d\n", g->num_vms, TEMPO_MAX_VMS);
    seq_printf(s, "  Total TSGs: %d / %d\n", g->num_tsgs, TEMPO_MAX_TSGS);
    seq_printf(s, "  Timer:      %d μs period\n", TEMPO_SCHED_TIMER_PERIOD_US);
    seq_printf(s, "  Yield TO:   %d μs\n", TEMPO_DEFAULT_YIELD_TIMEOUT_US);
    seq_printf(s, "\n");

    seq_printf(s, "Runlist:\n");
    seq_printf(s, "  ID:         %d\n", g->runlist_id);
    seq_printf(s, "  DMA addr:   0x%llx\n", (u64)g->runlist_dma);
    seq_printf(s, "  Entries:    %d\n", g->runlist_entries);
    seq_printf(s, "\n");

    seq_printf(s, "Statistics:\n");
    seq_printf(s, "  Context switches: %llu\n", g->total_context_switches);
    seq_printf(s, "  Preemptions:      %llu\n", g->total_preemptions);
    if (g->total_context_switches > 0) {
        seq_printf(s, "  Avg switch time:  %llu μs\n",
                   (g->cumulative_switch_ns / g->total_context_switches)
                   / 1000);
    }

    return 0;
}

static int tempo_debugfs_status_open(struct inode *inode, struct file *file)
{
    return single_open(file, tempo_debugfs_status_show, inode->i_private);
}

static const struct file_operations tempo_debugfs_status_fops = {
    .owner   = THIS_MODULE,
    .open    = tempo_debugfs_status_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* ──────────────────────────────────────────────────────────────────
 * vms — Per-VM state table
 * ────────────────────────────────────────────────────────────────── */

static const char *vm_state_str(enum tempo_vm_state s)
{
    switch (s) {
    case VM_STATE_IDLE:      return "IDLE";
    case VM_STATE_READY:     return "READY";
    case VM_STATE_RUNNING:   return "RUNNING";
    case VM_STATE_PREEMPTED: return "PREEMPTED";
    default:                 return "???";
    }
}

static const char *vm_prio_str(enum tempo_vm_priority p)
{
    switch (p) {
    case PRIO_REALTIME:   return "RT";
    case PRIO_STANDARD:   return "STD";
    case PRIO_BESTEFFORT: return "BE";
    default:              return "???";
    }
}

static int tempo_debugfs_vms_show(struct seq_file *s, void *unused)
{
    struct tempo_gpu_state *g = s->private;
    int i;

    seq_printf(s, "%-4s %-8s %-5s %-4s %-12s %-12s %-10s %-10s %-10s\n",
               "VM", "State", "Prio", "TSG",
               "VRAM(MB)", "GPU_time(ms)",
               "Switches", "Yields", "SLO(ms)");
    seq_printf(s, "---- -------- ----- ---- "
               "------------ ------------ "
               "---------- ---------- ----------\n");

    for (i = 0; i < g->num_vms; i++) {
        struct tempo_vm *vm = g->vms[i];

        if (!vm->active)
            continue;

        seq_printf(s, "%-4d %-8s %-5s %-4u %-12llu %-12llu %-10llu %-10llu",
                   vm->vm_id,
                   vm_state_str(vm->state),
                   vm_prio_str(vm->priority),
                   vm->tsg_id,
                   vm->vram_size >> 20,
                   vm->cumulative_gpu_ns / 1000000,
                   vm->total_switches_in,
                   vm->total_idle_yields);

        if (vm->slo_deadline_ns > 0)
            seq_printf(s, " %-10llu", vm->slo_deadline_ns / 1000000);
        else
            seq_printf(s, " %-10s", "none");

        seq_printf(s, "\n");
    }

    return 0;
}

static int tempo_debugfs_vms_open(struct inode *inode, struct file *file)
{
    return single_open(file, tempo_debugfs_vms_show, inode->i_private);
}

static const struct file_operations tempo_debugfs_vms_fops = {
    .owner   = THIS_MODULE,
    .open    = tempo_debugfs_vms_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* ──────────────────────────────────────────────────────────────────
 * scheduler — Manual scheduling commands
 *
 * Commands:
 *   "switch <vm_id>"   — Force context switch to the specified VM
 *   "preempt <tsg_id>" — Force preempt a specific TSG
 * ────────────────────────────────────────────────────────────────── */

static ssize_t tempo_debugfs_scheduler_write(struct file *file,
                                             const char __user *ubuf,
                                             size_t count, loff_t *ppos)
{
    struct tempo_gpu_state *g = file->private_data;
    char buf[64];
    int id;
    int ret;

    if (count >= sizeof(buf))
        return -EINVAL;
    if (copy_from_user(buf, ubuf, count))
        return -EFAULT;
    buf[count] = '\0';

    if (sscanf(buf, "switch %d", &id) == 1) {
        if (id < 0 || id >= g->num_vms) {
            pr_err(TEMPO_DRIVER_NAME ": invalid VM ID %d\n", id);
            return -EINVAL;
        }
        pr_info(TEMPO_DRIVER_NAME ": [debugfs] manual switch to VM %d\n", id);
        ret = tempo_sched_switch_to(g, id);
        if (ret)
            return ret;

    } else if (sscanf(buf, "preempt %d", &id) == 1) {
        if (id < 0 || id >= TEMPO_MAX_TSGS) {
            pr_err(TEMPO_DRIVER_NAME ": invalid TSG ID %d\n", id);
            return -EINVAL;
        }
        pr_info(TEMPO_DRIVER_NAME ": [debugfs] manual preempt TSG %d\n", id);
        ret = tempo_preempt_tsg(g, id);
        if (ret)
            return ret;

    } else {
        pr_err(TEMPO_DRIVER_NAME ": unknown command: %s", buf);
        return -EINVAL;
    }

    return count;
}

static int tempo_debugfs_scheduler_open(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;
    return 0;
}

static const struct file_operations tempo_debugfs_scheduler_fops = {
    .owner   = THIS_MODULE,
    .open    = tempo_debugfs_scheduler_open,
    .write   = tempo_debugfs_scheduler_write,
};

/* ──────────────────────────────────────────────────────────────────
 * switch_test — Context switch latency benchmark
 *
 * Write a number N to run N context switch iterations between
 * VM 0 and VM 1. Reports min/max/avg latency to dmesg.
 *
 * Prerequisites: at least 2 VMs must be created.
 *
 * Example:
 *   echo 1000 > /sys/kernel/debug/tempo-gpu/switch_test
 *   dmesg | tail
 * ────────────────────────────────────────────────────────────────── */

static ssize_t tempo_debugfs_switch_test_write(struct file *file,
                                               const char __user *ubuf,
                                               size_t count, loff_t *ppos)
{
    struct tempo_gpu_state *g = file->private_data;
    char buf[32];
    int iterations;
    int i;
    ktime_t t_start, t_end;
    u64 total_ns = 0;
    u64 min_ns = U64_MAX;
    u64 max_ns = 0;

    if (count >= sizeof(buf))
        return -EINVAL;
    if (copy_from_user(buf, ubuf, count))
        return -EFAULT;
    buf[count] = '\0';

    if (kstrtoint(buf, 10, &iterations) || iterations <= 0)
        return -EINVAL;

    if (iterations > 100000)
        iterations = 100000;  /* Safety cap */

    if (g->num_vms < 2) {
        pr_err(TEMPO_DRIVER_NAME ": switch_test requires at least 2 VMs\n");
        return -EINVAL;
    }

    pr_info(TEMPO_DRIVER_NAME ": starting switch test: %d iterations\n",
            iterations);

    for (i = 0; i < iterations; i++) {
        int target = i & 1;  /* Alternate between VM 0 and VM 1 */
        u64 elapsed;

        t_start = ktime_get();
        tempo_sched_switch_to(g, target);
        t_end = ktime_get();

        elapsed = ktime_to_ns(ktime_sub(t_end, t_start));
        total_ns += elapsed;
        if (elapsed < min_ns) min_ns = elapsed;
        if (elapsed > max_ns) max_ns = elapsed;
    }

    pr_info(TEMPO_DRIVER_NAME ": switch test results (%d iterations):\n",
            iterations);
    pr_info(TEMPO_DRIVER_NAME ":   avg = %llu ns (%llu μs)\n",
            total_ns / iterations, total_ns / iterations / 1000);
    pr_info(TEMPO_DRIVER_NAME ":   min = %llu ns (%llu μs)\n",
            min_ns, min_ns / 1000);
    pr_info(TEMPO_DRIVER_NAME ":   max = %llu ns (%llu μs)\n",
            max_ns, max_ns / 1000);

    return count;
}

static int tempo_debugfs_switch_test_open(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;
    return 0;
}

static const struct file_operations tempo_debugfs_switch_test_fops = {
    .owner   = THIS_MODULE,
    .open    = tempo_debugfs_switch_test_open,
    .write   = tempo_debugfs_switch_test_write,
};

/* ──────────────────────────────────────────────────────────────────
 * priority — Set VM priority class
 *
 * Format: "<vm_id> <priority>"
 * Priority values: rt (realtime), std (standard), be (best-effort)
 *
 * Example:
 *   echo "0 rt" > /sys/kernel/debug/tempo-gpu/priority
 *   echo "1 be" > /sys/kernel/debug/tempo-gpu/priority
 * ────────────────────────────────────────────────────────────────── */

static ssize_t tempo_debugfs_priority_write(struct file *file,
                                            const char __user *ubuf,
                                            size_t count, loff_t *ppos)
{
    struct tempo_gpu_state *g = file->private_data;
    char buf[32];
    char prio_str[8];
    int vm_id;
    enum tempo_vm_priority prio;

    if (count >= sizeof(buf))
        return -EINVAL;
    if (copy_from_user(buf, ubuf, count))
        return -EFAULT;
    buf[count] = '\0';

    if (sscanf(buf, "%d %7s", &vm_id, prio_str) != 2)
        return -EINVAL;

    if (vm_id < 0 || vm_id >= g->num_vms)
        return -EINVAL;

    if (strcmp(prio_str, "rt") == 0)
        prio = PRIO_REALTIME;
    else if (strcmp(prio_str, "std") == 0)
        prio = PRIO_STANDARD;
    else if (strcmp(prio_str, "be") == 0)
        prio = PRIO_BESTEFFORT;
    else
        return -EINVAL;

    g->vms[vm_id]->priority = prio;
    pr_info(TEMPO_DRIVER_NAME ": VM %d priority set to %s\n",
            vm_id, prio_str);

    return count;
}

static int tempo_debugfs_priority_open(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;
    return 0;
}

static const struct file_operations tempo_debugfs_priority_fops = {
    .owner   = THIS_MODULE,
    .open    = tempo_debugfs_priority_open,
    .write   = tempo_debugfs_priority_write,
};

/* ──────────────────────────────────────────────────────────────────
 * GPU Registers — Read raw GPU registers for debugging
 *
 * Reads selected PFIFO and PTIMER registers.
 * ────────────────────────────────────────────────────────────────── */

static int tempo_debugfs_regs_show(struct seq_file *s, void *unused)
{
    struct tempo_gpu_state *g = s->private;
    int rl_id = g->runlist_id;

    seq_printf(s, "GPU Register Dump\n");
    seq_printf(s, "=================\n\n");

    seq_printf(s, "NV_PMC_BOOT_0:            0x%08x\n",
               tempo_rd32(g, NV_PMC_BOOT_0));
    seq_printf(s, "NV_PFIFO_RUNLIST_BASE(%d): 0x%08x\n",
               rl_id, tempo_rd32(g, NV_PFIFO_RUNLIST_BASE(rl_id)));
    seq_printf(s, "NV_PFIFO_RUNLIST(%d):      0x%08x\n",
               rl_id, tempo_rd32(g, NV_PFIFO_RUNLIST(rl_id)));
    seq_printf(s, "NV_PFIFO_PREEMPT:         0x%08x\n",
               tempo_rd32(g, NV_PFIFO_PREEMPT));
    seq_printf(s, "NV_PFIFO_INTR_0:          0x%08x\n",
               tempo_rd32(g, NV_PFIFO_INTR_0));
    seq_printf(s, "NV_PFIFO_INTR_SCHED_ERR:  0x%08x\n",
               tempo_rd32(g, NV_PFIFO_INTR_SCHED_ERROR));
    seq_printf(s, "NV_PFIFO_INTR_CHSW_ERR:   0x%08x\n",
               tempo_rd32(g, NV_PFIFO_INTR_CHSW_ERROR));
    seq_printf(s, "\n");
    seq_printf(s, "NV_PTIMER_TIME:           %llu ns\n",
               tempo_gpu_time_ns(g));

    return 0;
}

static int tempo_debugfs_regs_open(struct inode *inode, struct file *file)
{
    return single_open(file, tempo_debugfs_regs_show, inode->i_private);
}

static const struct file_operations tempo_debugfs_regs_fops = {
    .owner   = THIS_MODULE,
    .open    = tempo_debugfs_regs_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* ──────────────────────────────────────────────────────────────────
 * Init / Exit
 * ────────────────────────────────────────────────────────────────── */

int tempo_debugfs_init(struct tempo_gpu_state *g)
{
    struct dentry *root;

    root = debugfs_create_dir(TEMPO_DRIVER_NAME, NULL);
    if (IS_ERR_OR_NULL(root)) {
        pr_warn(TEMPO_DRIVER_NAME ": failed to create debugfs directory\n");
        return -ENODEV;
    }

    g->debugfs_root = root;

    debugfs_create_file("status",      0444, root, g,
                        &tempo_debugfs_status_fops);
    debugfs_create_file("vms",         0444, root, g,
                        &tempo_debugfs_vms_fops);
    debugfs_create_file("registers",   0444, root, g,
                        &tempo_debugfs_regs_fops);
    debugfs_create_file("scheduler",   0200, root, g,
                        &tempo_debugfs_scheduler_fops);
    debugfs_create_file("switch_test", 0200, root, g,
                        &tempo_debugfs_switch_test_fops);
    debugfs_create_file("priority",    0200, root, g,
                        &tempo_debugfs_priority_fops);

    pr_info(TEMPO_DRIVER_NAME ": debugfs mounted at /sys/kernel/debug/%s\n",
            TEMPO_DRIVER_NAME);
    return 0;
}

void tempo_debugfs_exit(struct tempo_gpu_state *g)
{
    if (g->debugfs_root) {
        debugfs_remove_recursive(g->debugfs_root);
        g->debugfs_root = NULL;
    }
}
