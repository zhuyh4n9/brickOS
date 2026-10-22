/*
 * Clock conversion and busy-wait delay. Conversion base is milliseconds (freq/1000 = 62500, exact
 * for QEMU virt's 62.5 MHz arch timer), split into integer + remainder: freq/1000000 truncation
 * (62 or 63) would drift ~1%, and ticks*1000 in one go overflows u64 after long uptime.
 * br_delay_* is only a busy-wait primitive; blocking sleep is br_task_sleep().
 */
#include <br/core/br_time.h>
#include <br/runtime/br_plat.h>

/* QEMU virt arch timer is 62.5 MHz; fallback only when CNTFRQ_EL0 reads 0 or absurdly small. */
#define BR_CLOCK_FALLBACK_HZ   62500000u

/* Conversion base: ticks per millisecond (1000, not 1000000: 62.5 MHz is exact at ms). */
static br_u64 s_freq_hz      = 0;
static br_u64 s_ticks_per_ms = BR_CLOCK_FALLBACK_HZ / 1000u;

void br_clock_init(void)
{
    br_u64 freq = br_plat_ticks_freq();

    /* Fall back only when the frequency is untrustworthy (< 1 kHz or 0). */
    if (freq < 1000u) {
        freq = BR_CLOCK_FALLBACK_HZ;
    }

    s_freq_hz      = freq;
    s_ticks_per_ms = freq / 1000u;
}

br_u64 br_clock_freq_hz(void)
{
    return s_freq_hz;
}

br_u64 br_clock_ticks_per_ms(void)
{
    return s_ticks_per_ms;
}

/* IRQ heartbeat count, single source of truth in core: the platform timer ISR calls
 * br_clock_tick_notify() every tick, consumers read br_clock_tick_count(). Lock-free single-core
 * volatile increment; tick rate = BR_CFG_TICK_HZ (the platform timer reads the same config). */
static volatile br_u32 s_tick_count;

void br_clock_tick_notify(void)
{
    s_tick_count++;
}

br_u32 br_clock_tick_count(void)
{
    return s_tick_count;
}

/* Configured tick rate (Hz) from -DBR_CFG_TICK_HZ (product.toml [kernel].hz, default 200). */
br_u32 br_clock_tick_hz(void)
{
    return (br_u32)BR_CFG_TICK_HZ;
}

br_time_t br_clock_now(void)
{
    const br_u64 ticks = br_plat_ticks_now();

    /*
     * us = ticks * 1000 / ticks_per_ms, split into integer + remainder; ticks*1000 in one go
     * overflows 64 bits after long uptime. Exact for 62.5 MHz: 62500 ticks = 1000 us.
     */
    const br_u64 ms  = ticks / s_ticks_per_ms;
    const br_u64 rem = ticks % s_ticks_per_ms;

    return (br_time_t)((ms * 1000u) + ((rem * 1000u) / s_ticks_per_ms));
}

/*
 * Relative microseconds -> absolute deadline. BR_TIMEOUT_INF means "no deadline" and is returned
 * as-is; overflow saturates to BR_TIMEOUT_INF rather than wrapping into a past instant, which
 * would turn "wait forever" into "time out immediately" (TC-TIME-003).
 */
br_time_t br_deadline_from_now(br_time_t rel_us)
{
    if (rel_us == BR_TIMEOUT_INF) {
        return BR_TIMEOUT_INF;
    }

    const br_time_t now = br_clock_now();

    if (rel_us > (BR_TIMEOUT_INF - now)) {
        return BR_TIMEOUT_INF;
    }

    return now + rel_us;
}

void br_delay_us(br_time_t us)
{
    const br_u64 start = br_plat_ticks_now();

    /*
     * Convert to ticks and round up: a wakeup at expiry never fires early (it may be late).
     * us is split by 1000 first to avoid overflowing us * ticks_per_ms.
     */
    const br_u64 ticks = ((us / 1000u) * s_ticks_per_ms)
                       + ((((us % 1000u) * s_ticks_per_ms) + 999u) / 1000u);

    /*
     * Wrap-safe unsigned difference: (now - start) < ticks, not (now < start + ticks); the
     * former stays correct even if start + ticks wraps.
     */
    while ((br_plat_ticks_now() - start) < ticks) {
        /* Busy-wait; no scheduler and not even an idle task to run. */
    }
}

void br_delay_ms(br_time_t ms)
{
    br_delay_us(ms * BR_US_PER_MS);
}
