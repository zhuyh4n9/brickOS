/*
 * aarch64 arch timer: CNTFRQ_EL0 frequency and CNTPCT_EL0 physical counter. The physical
 * counter needs no EL1 enable and is independent of the virtual offset; frequency is
 * platform/firmware data. Tickless comparator/interrupt handling is M1's business.
 */
#include <br/runtime/br_plat.h>

br_u64 br_plat_ticks_freq(void)
{
    br_u64 freq;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

br_u64 br_plat_ticks_now(void)
{
    br_u64 ticks;

    /* ISB: no instruction may cross the counter read. */
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks;
}
