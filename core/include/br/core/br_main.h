/*
 * brickOS prototype v0.2.0 — core boot entry (br_core_main)
 *
 * Design basis: `docs/1-architecture/1-01-architecture.md` §9 (boot sequence) / §8 (three-layer
 * platform-capability model). Per-item rulings and why orchestration authority moved back here
 * from the plugin manager: `docs/decisions/0008-core-main-boot-chain.md`
 * (this header is its landing contract).
 *
 * ======================= boot chain (single source of truth here) =======================
 *   platform plugin:  reset asm (vector table / BSS clear / core select / SP)   [start.S]
 *          │
 *          └─ bl br_core_main ────────────────────────────────────────┐
 *                                                                     ▼
 *   core:  ① platform-agnostic init: clock → log → IRQ framework state → plugin manager scan (silent)
 *          ② platform plugin init: early_init of plugin_type == platform
 *             (early console / PIC registration + binding table / region table / page-table ops)
 *          ③ core modules that depend on platform impl: br_mem_init() (region table → three pools)
 *             → br_mm_activate() (region table → 4 KiB identity map + MMU on)
 *          ④ br_plugin_manager_run(): topology + phase drive
 *             (EARLY rest → CORE → LATE → global IRQ on → START) → br_sched_run()
 * ======================================================================
 *
 * Division of labor (aligned with 1-01 §8's three-layer model):
 *   platform plugin supplies the **mechanism and data** that change per SoC/arch
 *                   (console/PIC/region table/page-table build);
 *   core            supplies **abstraction and orchestration** (plugin manager/log/clock/
 *                   IRQ framework/heap/address-map setup).
 *   In one line: platform's Start.S only does reset/BSS, then `bl br_core_main`; every slot of
 *   the boot chain order lands in this entry's four stages (slot-by-slot with 1-01 §9).
 */
#ifndef BR_CORE_BR_MAIN_H
#define BR_CORE_BR_MAIN_H

#include <br/core/br_types.h>

/*
 * core boot entry (**never returns**).
 *
 * Four phases (the order is hard; per-item rationale in the comments of `core/src/main.c`):
 *   ① core platform-agnostic: `br_irq_cpu_init()` must run **before** platform submits the
 *      PIC binding table (it sets the descriptor pool's `pic_id/dom_id` to -1, see P-IRQ-5);
 *      this phase is **entirely silent** — console is only configured by the platform
 *      `early_init` of phase ②, and writing PL011 DR before that is dropped by QEMU
 *      (CR.UARTEN = 0).
 *   ② platform plugin init: explicitly take the `plugin_type == platform` entry and run
 *      `early_init` (no relying on topological luck: PIC/page table/console are not ready yet).
 *   ③ core depends on platform impl: `br_mem_init()` (claim three pools per the region table)
 *      → `br_mm_activate()` (identity map from that table + MMU on). The scheduler object
 *      (`br_sched`) is pure static BSS; the sched plugin registers it in EARLY (`coop_early_init`).
 *   ④ `br_plugin_manager_run()`: build init edges → topological sort → four-phase drive → `br_sched_run()`.
 *
 * Failure granularity = stop on first failure (ADR-0005 ruling G6): ②③ failures go through
 * `br_panic` (console is up); a ① failure has no console yet so output is dropped by QEMU, but
 * it still halts rather than running on half-initialized.
 */
BR_NORETURN void br_core_main(void);

#endif /* BR_CORE_BR_MAIN_H */
