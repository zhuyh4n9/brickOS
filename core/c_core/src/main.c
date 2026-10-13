/*
 * Core boot entry (br_core_main): four-phase boot chain, the order is hard.
 *  (1) platform-agnostic init, silent (console not up yet, QEMU drops output);
 *  (2) platform plugin early_init; (3) br_mem_init() pools + br_mm_activate() identity
 *  map/MMU; (4) br_plugin_manager_run() -> edges/toposort/EARLY-CORE-LATE -> interrupts
 *  on -> START -> br_sched_run(), does not return. First failure stops the machine
 *  (br_panic() once console is up). br_irq_cpu_init() must precede the platform binding
 *  table, else every descriptor's pic_id/dom_id is wiped back to -1 (P-IRQ-5).
 */
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_main.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_plugin.h>
#include <br/core/br_time.h>

static void core_init_platform_agnostic(void)
{
    /* Log time base depends on br_clock_init; order is fixed. */
    br_clock_init();
    br_log_init();
    br_log_set_level(BR_LOG_DEBUG);

    /* Must run before phase 2: platform fills descriptor pic_id/dom_id via
     * br_irq_bindings_set(), while this sets every descriptor to -1 (P-IRQ-5). */
    br_irq_cpu_init();

    /* Scan .br_plugins and collect platform plugins (silent). Edge building and
     * topological sort happen in phase 4. */
    br_plugin_manager_init();
}

static void core_init_platform_dependent(void)
{
    /* Claim the three pools per the platform region table; failure means the table and
     * the three-pool contract disagree (missing pool / bad granularity / too small). */
    if (br_mem_init() != 0) {
        br_panic("core.init: br_mem_init() failed - platform region table disagrees with the three pools");
    }

    /* Build the 4 KiB identity map and enable the MMU; must be the last step before
     * plugin init, so the pages holding the current PC/SP are already covered. */
    if (br_mm_activate() != 0) {
        br_panic("core.init: br_mm_activate() failed (page table build / MMU enable)");
    }
}

BR_NORETURN void br_core_main(void)
{
    /* 1: platform-agnostic init (clock/log/interrupt framework state/plugin scan) */
    core_init_platform_agnostic();

    /* 2: platform plugin init (early console/PIC and binding table/region table/page-table ops) */
    br_plugin_manager_platform_init();

    /* 3: core modules depending on the platform implementation (memory + address map setup) */
    core_init_platform_dependent();

    /* 4: plugin init and start (BR_NORETURN, so the line below is unreachable) */
    br_plugin_manager_run();
}
