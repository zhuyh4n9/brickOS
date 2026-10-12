/*
 * sched/coop — public declaration surface (hand-written; the capability side is one
 * implementation of `br_sched_ops_t`)
 *
 * This plugin's "public face" is not a set of new functions but the **scheduler ops table
 * it fills for core** (design `3-01` §5.1: "core owns semantics, plugins own policy").
 * So only the three lifecycle callbacks plus the ops registration point are declared here;
 * the actual policy lives in `src/coop.c`.
 *
 * Design basis: `3-03-sched.md` (scheduler overview), `3-01` §5.1/§6, `3-02` §11.3 (coop
 * interrupt-scheduling semantics), `docs/decisions/0006-scheduler-framework.md` (ops table
 * and per-item rulings).
 *
 * COOP semantics in one line: stacks switch only at **explicit points** (yield / block /
 * exit / first schedule); an interrupt return always resumes the interrupted thread (even
 * if the ISR woke another thread) — that is by design, not a bug.
 */
#ifndef BR_SCHED_COOP_H
#define BR_SCHED_COOP_H

#include <br/core/br_sched.h>
#include <br/core/br_types.h>

/* EARLY phase: fill the ops table and `br_sched_register()` (exactly once; 3-01 §5.1).
 * ★ Registration materializes the calling boot context as the main thread (ADR-0006 §1 I1),
 *   so create/join/yield work immediately before run. */
int coop_early_init(void);

/* CORE phase: no action — registration already happened in EARLY (the scheduler must be
 * in place before any thread is created). */
int coop_init(void);

/* start phase: no action — enabling interrupts globally and entering the scheduling
 * loop (`br_sched_run`) are done by the plugin manager. */
int coop_start(void);

#endif /* BR_SCHED_COOP_H */
