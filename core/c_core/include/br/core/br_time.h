/*
 * Delay and clock contract. br_time_t is uint64 microseconds. br_delay_us/br_delay_ms are a
 * prototype-era busy-wait exit only (WORKAROUND(br-wa-boot-001)): they burn CPU, are never woken
 * and never give up the CPU, so call sites should migrate to br_task_sleep() once the scheduler
 * exists; they suit only platform early_init and platform-internal code.
 */
#ifndef BR_CORE_BR_TIME_H
#define BR_CORE_BR_TIME_H

#include <br/core/br_types.h>

/* Absolute point in time in microseconds. */
typedef br_u64 br_time_t;

#define BR_US_PER_MS   1000u
#define BR_US_PER_SEC  1000000u

/* Timeout states: BR_TIMEOUT_ZERO = non-blocking (trylock), BR_TIMEOUT_INF = no deadline.
 * A relative value becomes an absolute deadline via br_deadline_from_now() (tickless-friendly). */
#define BR_TIMEOUT_ZERO  ((br_time_t)0u)
#define BR_TIMEOUT_INF   ((br_time_t)~(br_time_t)0u)

/* Relative microseconds -> absolute deadline (saturating; overflow must not mean "timeout now"). */
br_time_t br_deadline_from_now(br_time_t rel_us);

/* Initialise clock conversion with the arch timer frequency. Call once before br_clock_now/
 * br_delay_*, and before br_log_init() (log timestamps need it). */
void br_clock_init(void);

/* Monotonically non-decreasing microseconds since boot (2^64 us ~ 580,000 years). */
br_time_t br_clock_now(void);

/* Platform clock frequency (Hz); valid only after br_clock_init(). */
br_u64 br_clock_freq_hz(void);

/* Ticks per millisecond (= freq/1000); diagnostics only. */
br_u64 br_clock_ticks_per_ms(void);

/*
 * ---- Clock tick frequency (clock interrupts per second) ----
 * The timer ISR calls br_clock_tick_notify() once per 1/HZ second; default 200 => 5 ms per tick.
 * The value comes from product.toml [kernel].hz via -DBR_CFG_TICK_HZ; the #ifndef is the fallback
 * for builds that bypass brickie. The 100000 bound keeps the tick period from truncating to 0.
 */
#ifndef BR_CFG_TICK_HZ
#define BR_CFG_TICK_HZ 200u
#endif
_Static_assert((BR_CFG_TICK_HZ) >= 1u && (BR_CFG_TICK_HZ) <= 100000u,
               "BR_CFG_TICK_HZ out of range: valid range 1..100000 (product.toml [kernel].hz)");
/* One tick's length (us). A non-dividing HZ rounds down (per-tick error < 1 us). */
#define BR_CFG_TICK_PERIOD_US (1000000u / (BR_CFG_TICK_HZ))

/* Configured tick frequency (Hz), defined in core/c_core/src/time.c. */
br_u32 br_clock_tick_hz(void);

/*
 * Tick counter (jiffies), owned by core; the platform timer ISR calls br_clock_tick_notify() once
 * per tick. br_clock_tick_notify() is ISR-safe (single increment, no lock/log/alloc);
 * br_clock_tick_count() is a read-only snapshot. u32 wraps after ~248 days at 200 Hz -- compare
 * differences with the wrap-safe unsigned idiom, and use br_clock_now() for wall-clock time.
 */
void br_clock_tick_notify(void);
br_u32 br_clock_tick_count(void);

/* Busy-wait delay (**not sleep**): never gives up the CPU and cannot be woken. Use br_task_sleep()
 * once available. Guarantees no early wakeup and no upper bound on lateness, so the conversion
 * rounds ticks/us up. */
void br_delay_us(br_time_t us);
void br_delay_ms(br_time_t ms);

#endif /* BR_CORE_BR_TIME_H */
