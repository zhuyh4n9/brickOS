/*
 * brickOS prototype v0.2.0 — selftest aggregation for the platform plugin (platform/qemu-aarch64/src/selftest.c)
 *
 * Design basis and ruling: `docs/decisions/0010-plugin-selftest.md` (selftest kept separate from core code, driven
 * uniformly by core "after all starts, before the scheduler takes over").
 *
 * ## Why this file is an "aggregate" and not an "implementation"
 *   platform's two case suites were **already** separated from production code -- they live in:
 *     `src/irq_conf.c`  `br_plat_irq_conformance()`  interrupt subsystem cases (TC-IRQ-... / GIC-*)
 *     `src/mm_conf.c`   `br_plat_mem_conformance()`  memory map cases (TC-MEM-... / TC-MM-...)
 *   and those two files contain **no** production code (production is in `board_irq.c`/`gicv3.c`/`memmap.c`/`mmu.c`).
 *   So what this cut must do for platform is not "move tests" but **give them a descriptor hook** -- so core can
 *   drive it like any other plugin, instead of platform running the suites itself inside its own start.
 *
 *   * Historical baggage in file/function names: the two files are called `*_conf.c` and their entries
 *     `*_conformance()`, which does not fully match the new `selftest` vocabulary. This cut **does not rename
 *     them** -- renaming would drift `git log`/external references/gate tags together, and they are already in
 *     the "tests separated from core" shape; stating the relation in this comment is enough (ADR-0010 §3 ruling 8).
 *
 * ## How failure counts are combined
 *   Both entries return **their own failure count**, and this function returns the sum of the two -- consistent
 *   with what core's `br_plugin_manager_selftest()` does for plugins (it adds up each hook's return value).
 *   Logging is untouched: the two suites print their own `[IRQCONF]`/`[MEMCONF]` PASS/FAIL and SUMMARY lines,
 *   the gates (gates.toml) judge red/green on those lines; this function only adds one "platform selftest total" line.
 */
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/platform/br_plat.h>

/* Hook prototype convention matching the production files (name = symbol_prefix + phase; generated code refers to this function). */
int qemu_aarch64_selftest(void);

int qemu_aarch64_selftest(void)
{
    const int irq_fail = br_plat_irq_conformance();
    const int mem_fail = br_plat_mem_conformance();

    br_log_info("platform: selftest irq=%d mem=%d (failures)", irq_fail, mem_fail);
    return irq_fail + mem_fail;
}
