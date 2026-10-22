/*
 * sched/coop -- coop scheduler ops for core: public surface is the ops table it fills at
 * registration; policy lives in src/coop.c.
 * COOP semantics: stacks switch only at explicit points (yield / block / exit / first schedule),
 * and an interrupt return always resumes the interrupted thread.
 */
#ifndef BR_SCHED_COOP_H
#define BR_SCHED_COOP_H

#include <br/hal/br_sched.h>
#include <br/core/br_types.h>

/* EARLY: fill the ops table and call br_sched_register() exactly once. Registration
 * materializes the calling boot context as the main thread, so create/join/yield work
 * immediately, before run. */
int coop_early_init(void);

/* CORE: no action -- registration already happened in EARLY (the scheduler must be in place
 * before any thread is created). */
int coop_init(void);

/* START: no action -- the plugin manager enables interrupts and enters br_sched_run(). */
int coop_start(void);

#endif /* BR_SCHED_COOP_H */
