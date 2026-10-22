/*
 * Single entry of the core selftest suite (core/c_core/selftest/): interface tests, kept out of the
 * production .c files so "selftest off" means the test code is not in the image. Aggregates only,
 * does not judge: returns the sum of failing items, each suite logs its own lines, and no failure
 * halts the run.
 */
#include <br/core/br_types.h>

#include <br/hal/selftest.h>

#include "selftest_internal.h"

/* The only public entry; weakly referenced by plugin_mgr (needs -Wmissing-prototypes). */
int br_core_selftest(void);

int br_core_selftest(void)
{
    /* Order matters: static composition (plugin manager) first, then core runtime facilities.
     * sync follows sched because it really creates threads and blocks; work comes last because
     * case_wq_005 emits a bh-ban trace record that could disturb other cases' checks. */
    int fails = 0;

    fails += br_plugin_selftest();
    fails += br_service_selftest();
    fails += br_sched_selftest();
    fails += br_sync_selftest();
    fails += br_work_selftest();

    return fails;
}
