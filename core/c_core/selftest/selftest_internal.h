/*
 * Self-test suite entries of the subsystems (core-private; ADR-0038 / R1).
 *
 * These are **not** plugin public capabilities: a test entry in `br_*.h` would make adding a case
 * look like an interface change. They are declared here — one header, so the aggregator in
 * `core_selftest.c` carries no cross-translation-unit declaration of its own — and each suite
 * defines its entry in the same selftest source file that implements the cases.
 */
#ifndef BR_CORE_SELFTEST_INTERNAL_H
#define BR_CORE_SELFTEST_INTERNAL_H

/* Plugin manager (segment scan / topology / phases / APP last). */
int br_plugin_selftest(void);

/* Service registry (publish / lookup / error codes). */
int br_service_selftest(void);

/* The scheduling framework's suites moved to `hal/sched` with the framework itself (ADR-0039):
 * `br_sched_selftest` / `br_sync_selftest` / `br_work_selftest` are declared in
 * `<br/hal/selftest.h>` and compiled into that brick's unit. The aggregator below calls them, so
 * the core plugin declares a dependency on `hal/sched` (it already boots it). */

#endif /* BR_CORE_SELFTEST_INTERNAL_H */
