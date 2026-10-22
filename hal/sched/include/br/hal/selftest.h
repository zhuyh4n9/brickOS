/*
 * `hal/sched` selftest suite entries (TASKCONF / SYNCCONF / WORKCONF).
 *
 * These are **not** plugin public capabilities: a test entry in `br_hal/br_sched.h` would make
 * adding a case look like an interface change. They live here so the aggregator (core's
 * `br_core_selftest`, which runs after every start) can call them without carrying a
 * cross-translation-unit declaration of its own (ADR-0038 / R1). Compiled only while the product
 * selftest switch is on (`[selftest] enabled`).
 */
#ifndef BR_HAL_SCHED_SELFTEST_H
#define BR_HAL_SCHED_SELFTEST_H

int br_sched_selftest(void);   /* scheduling framework + chosen policy (create/yield/join/sleep) */
int br_sync_selftest(void);    /* sync primitives + time (the wait surface) */
int br_work_selftest(void);    /* bottom half / workqueue (bounded/FIFO/non-reentrant/bh bans) */

#endif /* BR_HAL_SCHED_SELFTEST_H */
