/*
 * brickOS prototype v0.2.0 — the **single entry** of the core selftest suite (core/selftest/)
 *
 * Design basis and per-item rulings: `docs/decisions/0010-plugin-selftest.md`.
 *
 * ## What this directory is
 *   **Interface tests**, separate from core code. The design side treats "target-side conformance
 *   cases" as part of the deliverable (`6-01-test.md`'s `TC-*` catalog), while the prototype
 *   previously **mixed their implementations into production .c files**
 *   (svc.c / sync.c / sched_core.c / plugin_mgr.c each carried a `*_conformance` block).
 *   This directory gathers the tests in one place, so:
 *     - production files keep only mechanism, no longer carrying hundreds of case lines;
 *     - "selftest off = test code not in the image" becomes a **mechanical fact** (the whole directory is out of the build);
 *     - readers of a case need not dig for the decision logic inside production code.
 *
 * ## Why one shared switch, not a separate one
 *   `product.toml [selftest]` governs plugin selftest and core selftest together — a product either
 *   ships tests or it does not. Two switches would introduce the half-done state "core tested,
 *   plugins not", which has no legitimate use (ADR-0010 §2.3).
 *
 * ## The aggregator's stance
 *   Same as what `br_plugin_manager_selftest()` does for plugins: **aggregate only, do not judge** —
 *   it returns the **sum of failing items**, logs are printed by each suite itself
 *   (`[PLGCONF]`/`[SVCCONF]`/...); red/green is judged by the gate (`tests/gates.toml` forbid).
 *   **No failure halts the run** (ADR-0010 §2.4).
 *
 * ## Relation to the boot chain
 *   This function is called by `br_plugin_manager_selftest()` through a **weak reference** (see
 *   plugin_mgr.c): with selftest off this directory is not built ⇒ the symbol does not exist ⇒
 *   null pointer ⇒ the manager skips it. So "was it compiled" and "should it run" are the same
 *   fact, and no second switch can drift.
 */
#include <br/core/br_types.h>

/*
 * Selftest entries of each subsystem. They **used to be** `*_conformance()` in production files,
 * now defined in this directory with declarations gathered here (instead of scattered across four
 * public headers) — rationale: selftest entries are **not plugin public capabilities** and must not
 * enter the golden surface of `br_*.h` (same source as ADR-0005 ruling 9 on hooks: putting the test
 * surface into the interface turns "edit one case" into an interface change).
 */
int br_plugin_selftest(void);   /* player: plugin manager (segments/topology/phases/APP last) */
int br_service_selftest(void);  /* service registry (publish/lookup/error codes) */
int br_sched_selftest(void);    /* scheduling framework + chosen scheduler (real-thread create/yield/join/sleep) */
int br_sync_selftest(void);     /* sync primitives + time (the wait surface of sync.c/sched_core.c) */
int br_work_selftest(void);     /* bottom half / workqueue (ADR-0011: bounded/FIFO/non-reentrant/bh bans) */

/* The only public entry (weakly referenced by plugin_mgr); the prototype enables -Wmissing-prototypes. */
int br_core_selftest(void);

int br_core_selftest(void)
{
    /* Order: first "is the static composition correct" (plugin manager), then core runtime
     * facilities (service table → scheduling → sync → bottom half). The sync suite follows
     * scheduling because it **really creates threads and blocks**, changing scheduler state; the
     * workqueue comes last — it does not change scheduler state, but case_wq_005 deliberately
     * triggers one bh-ban trace record, so it goes last to keep that trace from disturbing other
     * cases' "any new event" checks. */
    int fails = 0;

    fails += br_plugin_selftest();
    fails += br_service_selftest();
    fails += br_sched_selftest();
    fails += br_sync_selftest();
    fails += br_work_selftest();

    return fails;
}
