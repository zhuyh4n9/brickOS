/*
 * sched/rr — public declaration surface (hand-written; capability surface = one
 * implementation of `br_sched_ops_t`)
 *
 * This plugin's public surface is not a set of new functions but the **scheduler ops table
 * filled into core** (design `3-01` §5.1: "core owns semantics, plugins own policy").
 * So only three lifecycle callbacks are declared here; the real policy lives in `src/rr.c`.
 *
 * Basis: `3-03-sched.md` (scheduling overview), `3-01` §5.1/§2.1, `3-02` §11.2/§11.3,
 *        `docs/decisions/0011-round-robin-bh-workqueue.md` (preemption seam table + rulings).
 *
 * RR semantics in one line: **besides explicit points, every tick also swaps stacks**. When the
 * current thread's slice expires ⇒ requeue at the ready tail + core switches at IRQ exit to the
 * next same-priority thread; no priorities, no PI.
 */
#ifndef BR_SCHED_RR_H
#define BR_SCHED_RR_H

#include <br/core/br_sched.h>
#include <br/core/br_types.h>

/* Slice: in units of the **platform tick** (tick = `1/HZ`; `product.toml [kernel].hz`, default
 * 200 ⇒ 5 ms; ADR-0017). Value 2 ⇒ by default the next thread runs 10 ms before rotation (the
 * wall-clock length scales with HZ). Why ticks and not microseconds: the decrement happens in
 * `on_tick`, so a tick is its natural granularity — microseconds would only force a "read the
 * clock and divide" hot path without better precision. */
#define RR_SLICE_TICKS 2u

/* EARLY phase: fill the ops table and `br_sched_register()` (register exactly once; 3-01 §5.1).
 * ★ Registration materializes "the boot context of the caller" into the main thread (ADR-0006 §1,
 *   I1), so create/join/yield work immediately before run. */
int rr_early_init(void);

/* CORE phase: no action — registration already happened in EARLY (the scheduler must be in place
 * before any thread is created). */
int rr_init(void);

/* start phase: no action — enabling interrupts globally and entering the scheduling loop
 * (`br_sched_run`) is done by the plugin manager. */
int rr_start(void);

#endif /* BR_SCHED_RR_H */
