/*
 * sched/rr -- round-robin preemptive scheduler ops for core: public surface is the ops table it
 * fills at registration; policy lives in src/rr.c.
 * Every tick can switch stacks: on slice expiry the thread is requeued at the ready tail and core
 * switches at IRQ exit; no priorities, no PI.
 */
#ifndef BR_SCHED_RR_H
#define BR_SCHED_RR_H

#include <br/core/br_sched.h>
#include <br/core/br_types.h>

/* Slice in platform ticks (tick = 1/HZ, `product.toml [kernel].hz`, default 200 => 5 ms); value 2
 * => 10 ms per turn. Ticks are the natural granularity because on_tick does the decrement. */
#define RR_SLICE_TICKS 2u

/* EARLY: fill the ops table and call br_sched_register() exactly once; registration materializes
 * the calling boot context as the main thread. */
int rr_early_init(void);

/* CORE: no action -- registration already happened in EARLY (the scheduler must be in place
 * before any thread is created). */
int rr_init(void);

/* START: no action -- the plugin manager enables interrupts and enters br_sched_run(). */
int rr_start(void);

#endif /* BR_SCHED_RR_H */
