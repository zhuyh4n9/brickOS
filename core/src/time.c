/*
 * brickOS prototype v0.1.0 — clock conversion and busy-wait delay
 *
 * Design mapping: 3-01 §4 (br_clock_now / br_deadline_from_now, group br-sched).
 * v0.1.0 implements only the reading half; the timeout table and wakeups belong to the scheduler (M1), not faked here.
 *
 * Why conversion avoids the simple "ticks / (freq/1e6)":
 *   QEMU virt's arch timer is 62.5 MHz = 62.5 ticks per microsecond, not an integer.
 *   `freq/1000000` truncation yields 62 or 63, either of which brings ~1% systematic time drift
 *   (rounding up to 63 -> readings run 0.8% slower than real time, 8 s off after 1000 s).
 *   Here the conversion base is milliseconds (freq/1000 = 62500, exact for 62.5 MHz),
 *   then integer part + remainder are computed separately, so there is neither drift nor overflow.
 *
 * WORKAROUND(br-wa-boot-001) ① is fully repaid (v0.2.0): with the scheduler in place, sleeping is a real
 * blocking switch via `br_task_sleep()`; `br_delay_*` in this file is only a busy-wait primitive from now on
 * (no call sites in the tree currently; kept for early phases / debugging).
 * The design has no br_delay_* (only br_task_sleep, since it necessarily blocks and switches).
 *
 * This file also owns the IRQ heartbeat count (`br_clock_tick_notify` / `br_clock_tick_count`):
 * the platform timer ISR notifies every tick and core keeps the count — so the P0 APP need not read the platform
 * plugin interface directly (repays `br-wa-boot-001` ②; see docs/decisions/0016-core-timer-heartbeat.md).
 */
#include <br/core/br_time.h>
#include <br/platform/br_plat.h>

/* Platform clock frequency default: QEMU virt's arch timer is 62.5 MHz.
 * Used only as a fallback when CNTFRQ_EL0 reads 0/absurdly small; not the normal path. */
#define BR_CLOCK_FALLBACK_HZ   62500000u

/* Conversion base: ticks per millisecond. Uses 1000 rather than 1000000 because 62.5 MHz is exact at milliseconds. */
static br_u64 s_freq_hz      = 0;
static br_u64 s_ticks_per_ms = BR_CLOCK_FALLBACK_HZ / 1000u;

void br_clock_init(void)
{
    br_u64 freq = br_plat_ticks_freq();

    /* Fall back only when the frequency is untrustworthy (< 1 kHz or read as 0); the normal path reads CNTFRQ_EL0. */
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

/* ------------------------------------------------------------------ IRQ heartbeat count
 *
 * Single source of truth in core: the platform timer ISR calls `br_clock_tick_notify()` every tick and
 * consumers (APP/service/debug) read `br_clock_tick_count()`. For why it lives in core see the br_time.h
 * comments and `docs/decisions/0016-core-timer-heartbeat.md` — in one line: the P0 APP should not need to
 * know the platform plugin interface just to read a diagnostic count (repays `br-wa-boot-001` ②).
 *
 * Lock-free: single-core prototype with a `volatile` increment; its multicore/PM shape (per-CPU count or
 * atomics) is deferred until the design's PM/IPI work lands, not faked now.
 *
 * Tick rate = `BR_CFG_TICK_HZ` (from `product.toml [kernel].hz`, default 200 => 5 ms per tick;
 * see ADR-0017). The platform timer's reload period reads the same config; core only counts here.
 */
static volatile br_u32 s_tick_count;

void br_clock_tick_notify(void)
{
    s_tick_count++;
}

br_u32 br_clock_tick_count(void)
{
    return s_tick_count;
}

/* Configured tick rate (Hz). The macro comes in from `product.toml [kernel].hz` via `-DBR_CFG_TICK_HZ`
 * (default 200, see br_time.h); this is its only runtime read point. */
br_u32 br_clock_tick_hz(void)
{
    return (br_u32)BR_CFG_TICK_HZ;
}

br_time_t br_clock_now(void)
{
    const br_u64 ticks = br_plat_ticks_now();

    /*
     * us = ticks * 1000 / ticks_per_ms, split into integer + remainder:
     *   computing ticks*1000 in one go overflows 64 bits after long uptime;
     *   split, (ticks % ticks_per_ms) < ticks_per_ms (~6e4), so multiplying by 1000 cannot overflow.
     * This is an exact integer conversion (for 62.5 MHz: 62500 ticks = 1000 us, one to one).
     */
    const br_u64 ms  = ticks / s_ticks_per_ms;
    const br_u64 rem = ticks % s_ticks_per_ms;

    return (br_time_t)((ms * 1000u) + ((rem * 1000u) / s_ticks_per_ms));
}

/*
 * Relative microseconds -> absolute deadline (design `3-01` §14 CA-4: blocking APIs take relative
 * values and internally unify to an absolute deadline; the tickless framework only compares deadlines). Two deliberate points:
 *   - `BR_TIMEOUT_INF` is returned as-is: it means "no deadline", not "a very distant deadline";
 *   - overflow saturates to `BR_TIMEOUT_INF` (rather than wrapping into a past instant — that would turn
 *     "wait forever" into "time out immediately", a silent semantic inversion; see 6-01 TC-TIME-003 "saturate, do not wrap").
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
     * Convert to ticks and round up — this is where the "never wake early" semantics lands
     * (design 3-01 §2.1: a wakeup at expiry never fires early, with no bound on being late).
     * us is split by 1000 first to avoid overflowing us * ticks_per_ms.
     */
    const br_u64 ticks = ((us / 1000u) * s_ticks_per_ms)
                       + ((((us % 1000u) * s_ticks_per_ms) + 999u) / 1000u);

    /*
     * Unsigned wrap-safe difference comparison: written as (now - start) < ticks instead of
     * (now < start + ticks); the former's correctness does not rely on start + ticks not wrapping.
     */
    while ((br_plat_ticks_now() - start) < ticks) {
        /* Busy-wait. No scheduler and no other thread to run — not even an idle task yet. */
    }
}

void br_delay_ms(br_time_t ms)
{
    br_delay_us(ms * BR_US_PER_MS);
}
