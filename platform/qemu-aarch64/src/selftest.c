/*
 * Platform plugin selftest aggregator: returns the sum of the IRQ and memory suite failure
 * counts, matching core's plugin-manager convention (it adds up each hook's return value).
 * The suites print their own [IRQCONF]/[MEMCONF] PASS/FAIL and SUMMARY lines, which the gates
 * judge; this function only adds a "platform selftest total" line.
 */
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/platform/br_plat.h>

/* Hook prototype convention (symbol_prefix + phase); generated code refers to this function. */
int qemu_aarch64_selftest(void);

int qemu_aarch64_selftest(void)
{
    const int irq_fail = br_plat_irq_conformance();
    const int mem_fail = br_plat_mem_conformance();

    br_log_info("platform: selftest irq=%d mem=%d (failures)", irq_fail, mem_fail);
    return irq_fail + mem_fail;
}
