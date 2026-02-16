// SPDX-License-Identifier: GPL-2.0
/*
 * Tempo GPU: Scheduler Engine
 *
 * Month 1: Priority round-robin with instant idle yield.
 * Future months add kernel-boundary switching, bubble harvesting,
 * and SLO-aware preemption.
 *
 * Scheduling flow:
 *   1. USERD doorbell write detected (tempo_mmio.c)
 *      → tempo_sched_notify_work() — marks VM as READY
 *   2. Idle detection timer fires (tempo_idle.c)
 *      → tempo_sched_notify_idle() — marks VM as IDLE
 *   3. Scheduler work runs (this file)
 *      → picks highest-priority READY VM, switches runlist
 */

#include "tempo_gpu.h"

/* ──────────────────────────────────────────────────────────────────
 * Scheduler Work Function (bottom-half, runs in process context)
 * ────────────────────────────────────────────────────────────────── */

static void tempo_scheduler_work_fn(struct work_struct *work)
{
    struct tempo_gpu_state *g = g_tempo;
    unsigned long flags;
    int best_vm = -1;
    int best_prio = PRIO_NUM_LEVELS;
    int current_vm;
    bool should_switch = false;

    spin_lock_irqsave(&g->sched_lock, flags);

    current_vm = g->active_vm_id;

    /* Find highest-priority READY VM */
    for (int i = 0; i < g->num_vms; i++) {
        struct tempo_vm *vm = g->vms[i];

        if (!vm->active)
            continue;
        if (vm->state != VM_STATE_READY)
            continue;
        if ((int)vm->priority < best_prio) {
            best_prio = vm->priority;
            best_vm = i;
        }
    }

    /* Decide whether to switch */
    if (current_vm < 0 && best_vm >= 0) {
        /* GPU idle, schedule any ready VM */
        should_switch = true;

    } else if (current_vm >= 0 && best_vm >= 0) {
        struct tempo_vm *active = g->vms[current_vm];

        if (best_prio < (int)active->priority) {
            /* Higher-priority VM waiting → preempt */
            should_switch = true;

        } else if (active->state == VM_STATE_IDLE) {
            /* Active VM went idle → yield to any ready VM */
            should_switch = true;

        } else if (active->state == VM_STATE_RUNNING &&
                   active->budget_ns > 0) {
            /* Check budget exhaustion (Phase 1: simple time tracking) */
            ktime_t now = ktime_get();
            u64 elapsed = ktime_to_ns(ktime_sub(now, active->last_scheduled));

            if (elapsed >= active->budget_ns) {
                /* Budget exceeded → round-robin to next ready VM */
                should_switch = true;
            }
        }
    }

    spin_unlock_irqrestore(&g->sched_lock, flags);

    if (should_switch && best_vm >= 0)
        tempo_sched_switch_to(g, best_vm);
}

/* ──────────────────────────────────────────────────────────────────
 * Context Switch Execution
 *
 * Builds a runlist with only the target VM's TSG and submits it.
 * The GPU will preempt the current TSG and switch to the new one.
 * ────────────────────────────────────────────────────────────────── */

int tempo_sched_switch_to(struct tempo_gpu_state *g, int new_vm_id)
{
    struct tempo_vm *new_vm;
    int old_vm_id;
    ktime_t t_start, t_end;
    unsigned long flags;
    int ret;

    if (new_vm_id < 0 || new_vm_id >= g->num_vms)
        return -EINVAL;

    new_vm = g->vms[new_vm_id];
    if (!new_vm->active)
        return -EINVAL;

    t_start = ktime_get();

    spin_lock_irqsave(&g->sched_lock, flags);
    old_vm_id = g->active_vm_id;

    /* Update outgoing VM state */
    if (old_vm_id >= 0 && old_vm_id < g->num_vms) {
        struct tempo_vm *old_vm = g->vms[old_vm_id];
        ktime_t now = ktime_get();

        old_vm->cumulative_gpu_ns +=
            ktime_to_ns(ktime_sub(now, old_vm->last_scheduled));

        if (old_vm->state == VM_STATE_RUNNING)
            old_vm->state = VM_STATE_READY;
    }

    /* Update incoming VM state */
    new_vm->state = VM_STATE_RUNNING;
    new_vm->last_scheduled = ktime_get();
    new_vm->idle_start = 0;
    new_vm->total_switches_in++;
    g->active_vm_id = new_vm_id;
    g->total_context_switches++;

    spin_unlock_irqrestore(&g->sched_lock, flags);

    /* Submit runlist with only the new VM's TSG */
    ret = tempo_runlist_build_and_submit_single(g, new_vm_id);
    if (ret) {
        pr_err(TEMPO_DRIVER_NAME ": runlist submit failed for VM %d: %d\n",
               new_vm_id, ret);
        return ret;
    }

    t_end = ktime_get();
    g->cumulative_switch_ns += ktime_to_ns(ktime_sub(t_end, t_start));

    pr_debug(TEMPO_DRIVER_NAME ": switch VM %d → VM %d (%llu μs)\n",
             old_vm_id, new_vm_id,
             ktime_to_ns(ktime_sub(t_end, t_start)) / 1000);

    return 0;
}

/* ──────────────────────────────────────────────────────────────────
 * Notification Interfaces (called from mmio.c and idle.c)
 * ────────────────────────────────────────────────────────────────── */

/*
 * Called when a VM submits GPU work (USERD doorbell write detected).
 * If VM was idle, mark it READY and trigger scheduling.
 */
void tempo_sched_notify_work(struct tempo_gpu_state *g, struct tempo_vm *vm)
{
    unsigned long flags;
    bool need_schedule = false;

    spin_lock_irqsave(&g->sched_lock, flags);

    if (vm->state == VM_STATE_IDLE) {
        vm->state = VM_STATE_READY;
        need_schedule = true;

        /* Fast path: if GPU is idle, schedule immediately */
        if (g->active_vm_id < 0) {
            spin_unlock_irqrestore(&g->sched_lock, flags);
            tempo_sched_switch_to(g, vm->vm_id);
            return;
        }

        /* Check if this VM should preempt the current one */
        if (g->active_vm_id >= 0) {
            struct tempo_vm *active = g->vms[g->active_vm_id];
            if (vm->priority < active->priority)
                need_schedule = true;
        }
    }

    spin_unlock_irqrestore(&g->sched_lock, flags);

    if (need_schedule)
        schedule_work(&g->sched_work);
}

/*
 * Called when a VM goes idle (GET==PUT for all channels, timeout expired).
 */
void tempo_sched_notify_idle(struct tempo_gpu_state *g, struct tempo_vm *vm)
{
    unsigned long flags;

    spin_lock_irqsave(&g->sched_lock, flags);

    if (vm->state == VM_STATE_RUNNING) {
        vm->state = VM_STATE_IDLE;
        vm->last_yielded = ktime_get();
        vm->total_idle_yields++;
    }

    spin_unlock_irqrestore(&g->sched_lock, flags);

    /* Trigger scheduler to find the next ready VM */
    schedule_work(&g->sched_work);
}

/* ──────────────────────────────────────────────────────────────────
 * hrtimer callback: periodic scheduler tick
 *
 * This is a fallback timer that fires every TEMPO_SCHED_TIMER_PERIOD_US
 * to check for budget exhaustion and other time-based scheduling events.
 * Primary scheduling is event-driven (doorbell writes + idle detection).
 * ────────────────────────────────────────────────────────────────── */

static enum hrtimer_restart tempo_sched_timer_fn(struct hrtimer *timer)
{
    struct tempo_gpu_state *g =
        container_of(timer, struct tempo_gpu_state, sched_timer);

    if (!g->scheduler_active)
        return HRTIMER_NORESTART;

    /* Check idle state of active VM */
    tempo_idle_check(g);

    /* Check budget-based preemption (Phase 1: basic time accounting) */
    if (g->active_vm_id >= 0) {
        struct tempo_vm *vm = g->vms[g->active_vm_id];

        if (vm->budget_ns > 0 && vm->state == VM_STATE_RUNNING) {
            u64 elapsed = ktime_to_ns(
                ktime_sub(ktime_get(), vm->last_scheduled));

            if (elapsed >= vm->budget_ns)
                schedule_work(&g->sched_work);
        }
    }

    hrtimer_forward_now(timer,
                        ns_to_ktime(TEMPO_SCHED_TIMER_PERIOD_US * 1000ULL));
    return HRTIMER_RESTART;
}

/* ──────────────────────────────────────────────────────────────────
 * Init / Exit
 * ────────────────────────────────────────────────────────────────── */

void tempo_sched_init(struct tempo_gpu_state *g)
{
    INIT_WORK(&g->sched_work, tempo_scheduler_work_fn);

    hrtimer_init(&g->sched_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    g->sched_timer.function = tempo_sched_timer_fn;
    hrtimer_start(&g->sched_timer,
                  ns_to_ktime(TEMPO_SCHED_TIMER_PERIOD_US * 1000ULL),
                  HRTIMER_MODE_REL);

    g->scheduler_active = true;
    pr_info(TEMPO_DRIVER_NAME ": scheduler started (timer %d μs)\n",
            TEMPO_SCHED_TIMER_PERIOD_US);
}

void tempo_sched_exit(struct tempo_gpu_state *g)
{
    g->scheduler_active = false;
    hrtimer_cancel(&g->sched_timer);
    cancel_work_sync(&g->sched_work);

    pr_info(TEMPO_DRIVER_NAME ": scheduler stopped (%llu total switches)\n",
            g->total_context_switches);
}
