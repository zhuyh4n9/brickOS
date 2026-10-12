/*
 * brickOS prototype v0.1.0 — aarch64 arch timer (CNTFRQ/CNTPCT)
 *
 * Design mapping (1-01 §8 three-layer pattern):
 *   core's `br_clock` tickless semantics  <- contract
 *   arch timer read                       <- ISA common part (this file)
 *   frequency/calibration                 <- platform data (CNTFRQ_EL0 set by platform/firmware)
 *
 * Why read CNTPCT_EL0 instead of CNTVCT_EL0: the physical counter needs no enable at EL1,
 * is independent of the virtual offset, and is always available on QEMU virt. The tickless
 * "comparator/interrupt" is M1's business.
 *
 * Paid off (ADR-0005): this file is part of Platform Entry (not turned into a plugin — that
 * is an **in-design** shape, not an omission).
 */
#include <br/platform/br_plat.h>

br_u64 br_plat_ticks_freq(void)
{
    br_u64 freq;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

br_u64 br_plat_ticks_now(void)
{
    br_u64 ticks;

    /* isb: ensure all instructions before the counter read have taken effect
     * (out-of-order/prefetch cannot cross the read).
     * Interrupt-safe paths that do not read the clock never come here — on the design side
     * that is the discussion in 3-02 §8.3.1. */
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks;
}
