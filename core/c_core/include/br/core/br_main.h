/*
 * Core boot entry (br_core_main) contract. The platform Start.S does reset/BSS only, then calls
 * br_core_main; the four-stage boot chain (platform-agnostic init, platform early_init, modules
 * needing the platform impl, plugin manager + scheduler) is ordered and single-sourced here.
 */
#ifndef BR_CORE_BR_MAIN_H
#define BR_CORE_BR_MAIN_H

#include <br/core/br_types.h>

/*
 * Core boot entry (**never returns**). Order is hard: (1) platform-agnostic init, where
 * `br_irq_cpu_init()` must run **before** the platform submits the PIC binding table, and which
 * emits nothing (console comes up in phase 2; earlier PL011 writes are dropped); (2) platform
 * plugin early_init (do not rely on topological order); (3) `br_mem_init()` then
 * `br_mm_activate()`; (4) `br_plugin_manager_run()` then `br_sched_run()`.
 * Stops on first failure: phases 2/3 go through br_panic, a phase-1 failure has no console but
 * still halts rather than continuing half-initialized.
 */
BR_NORETURN void br_core_main(void);

#endif /* BR_CORE_BR_MAIN_H */
