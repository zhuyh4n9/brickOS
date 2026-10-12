/*
 * brickOS prototype v0.2.0 — core boot entry (br_core_main): orchestration of the
 * four-phase boot chain
 *
 * Design basis: `1-01` §9 (boot sequence)/§6.2 (four lifecycle phases),
 *           `3-05` (plugin_manager), `3-04` (memory: region table → three pools →
 *           identity map), `3-02` §14.3 (one-way interrupt init chain).
 * Per-item rulings and alternatives: `docs/decisions/0008-core-main-boot-chain.md`.
 *
 * ## Why this entry exists (which debt it repays)
 *   The v0.1/v0.2 boot chain was mixed into the plugin manager: `start.S` called
 *   `br_irq_cpu_init` and then went straight into `br_plugin_manager_run()`, which
 *   before phase driving did five things on the side: "clock/log/segment scan/
 *   platform.early_init/heap/MMU" (see ADR-0005 §2.6). Consequences:
 *     - the `core.init` slot of design `1-01` §9 had no landing place (logged as
 *       br-wa-boot-001 (1));
 *     - `br_mem_init()` was hidden inside platform's memmap —— but "claim pools per
 *       the region table" is core's job;
 *     - `br_plat_mmu_init()` both registered ops and activated (built tables + enabled
 *       MMU) —— mechanism and orchestration mixed in one place.
 *   This change moves orchestration authority here; the plugin manager goes back to
 *   "plugins only" (ADR-0008).
 *
 * ## Four phases (each cell maps to 1-01 §9; the order is hard)
 *
 *   ① core platform-agnostic init —— `core_init_platform_agnostic()`
 *      `br_clock_init()` → `br_log_init()` → `br_irq_cpu_init()` → `br_plugin_manager_init()`
 *      ★ `br_irq_cpu_init()` must run before platform submits the PIC binding table:
 *        it sets every descriptor's `pic_id/dom_id` to -1 (P-IRQ-5); running it after
 *        the binding table would erase the bindings already placed.
 *      ★ This phase is entirely silent: console belongs to phase ②, and writing the
 *        PL011 DR now would be dropped by QEMU.
 *
 *   ② platform plugin init —— `br_plugin_manager_platform_init()`
 *      Explicitly take plugins with `plugin_type == platform` and run `early_init`:
 *      early console → GICv3 PIC registration + binding table → region table
 *      declaration → page-table ops registration.
 *
 *   ③ core modules that depend on the platform implementation —— `core_init_platform_dependent()`
 *      `br_mem_init()` (claim TLSF heap/contiguous pool/page pool per the region
 *      table) → `br_mm_activate()` (build the 4 KiB identity map from the same region
 *      table and enable MMU).
 *      The scheduling framework object (`br_sched`) is pure static BSS with no runtime
 *      construction; its registration is done by the sched plugin in the EARLY phase
 *      (`coop_early_init()` → `br_sched_register()`), not in this phase.
 *
 *   ④ `br_plugin_manager_run()` —— build init edges → Kahn topological sort →
 *      EARLY (rest)/CORE/LATE → globally enable interrupts → START (APP last) →
 *      `br_sched_run()`. Does NOT return.
 *
 * ## Failure granularity (consistent with ADR-0005 ruling G6)
 *   First failure stops the machine, no degradation. On ②③ failure console is already
 *   up ⇒ `br_panic()` (with a `[PANIC]` line); on ① failure console is not up (output
 *   dropped by QEMU) but we still stop —— "keep running a half-initialized system"
 *   only moves symptoms further from the root cause.
 */
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_main.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_plugin.h>
#include <br/core/br_time.h>

/* ==================================================================== phase ① */

static void core_init_platform_agnostic(void)
{
    /* clock → log: the log time base depends on br_clock_init, order is fixed (same as v0.1). */
    br_clock_init();
    br_log_init();
    br_log_set_level(BR_LOG_DEBUG);

    /* Per-CPU interrupt framework state + static descriptor pool init (P-IRQ-5).
     * ★ Running before phase ② is a hard constraint: platform's `br_plat_irq_init()`
     *   fills descriptors' pic_id/dom_id via `br_irq_bindings_set()`, while this
     *   function sets EVERY descriptor to -1 ⇒ reversed order wipes the binding table
     *   to "no bindings" (and the unbound = -ENODEV criterion becomes wrong). */
    br_irq_cpu_init();

    /* Plugin management: scan `.br_plugins` + explicitly find platform plugins
     * (silent —— see the file header note). Building init edges/topological sort/phase
     * driving is phase ④; here we only do "segment → table". */
    br_plugin_manager_init();
}

/* ==================================================================== phase ③ */

static void core_init_platform_dependent(void)
{
    /* memory: core claims the three pools per the platform-declared region table (3-04 §2).
     * Failure = region table and three-pool contract disagree (missing required pool/bad
     * granularity/pool too small), which is a boot failure. */
    if (br_mem_init() != 0) {
        br_panic("core.init: br_mem_init() failed - platform region table disagrees with the three pools");
    }

    /* address mapping: core provides the abstraction and performs the establishment
     * action (`br_mm_activate()`); the page-table construction mechanism comes in via
     * platform `br_mm_register()` in phase ②. This step builds the 4 KiB identity map
     * and enables MMU —— afterwards every fetch/access goes through translation, so it
     * must be the last cell before plugin init (the pages holding the current PC/SP must
     * already be covered by the new page table). */
    if (br_mm_activate() != 0) {
        br_panic("core.init: br_mm_activate() failed (page table build / MMU enable)");
    }
}

/* ==================================================================== entry */

BR_NORETURN void br_core_main(void)
{
    /* ① core platform-agnostic init (clock/log/interrupt framework state/plugin scan) */
    core_init_platform_agnostic();

    /* ② platform plugin init (early console/PIC and binding table/region table/page-table ops) */
    br_plugin_manager_platform_init();

    /* ③ core modules depending on the platform implementation (memory + address map setup) */
    core_init_platform_dependent();

    /* ④ plugin init and start: topology + EARLY/CORE/LATE → enable interrupts → START →
     * scheduling. br_plugin_manager_run() is BR_NORETURN ⇒ the line below is unreachable. */
    br_plugin_manager_run();
}
