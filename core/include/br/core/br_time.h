/*
 * brickOS prototype v0.1.0 — delay and clock contract
 *
 * Mapping to the design docs (on the brickOS-Design branch):
 *   - `br_time_t` = uint64 microseconds (3-01 §14 CA-1) — this header copies that decision, it does
 *     not invent a unit of its own
 *   - `br_clock_now()` (3-01 §4) keeps only the "monotonic reading" half of its semantics in v0.1.0
 *   - **deliberately absent**: the **implementation** of br_task_sleep / br_task_sleep_until
 *     (declared in br_sched.h, implemented by core's scheduling framework) — from v0.2 on br_delay_*
 *     is a temporary "busy-wait" exit and call sites should migrate to br_task_sleep (see
 *     br-wa-boot-001 in WORKAROUNDS.md).
 *
 * Naming note: `br_delay_us`/`br_delay_ms` here are a **prototype-era temporary exit**
 *   (they are not in the design list — the design always uses br_task_sleep for "sleep", because it
 *   necessarily blocks and switches).
 *   **From v0.2 on `br_task_sleep` is implemented in core's scheduling framework** ⇒ these two
 *   functions are **busy-wait only** (they burn CPU counting cycles, never give up the CPU and cannot
 *   be woken), **no longer a "sleep" implementation**; whenever "give up the CPU and wait a while" is
 *   needed, always use `br_task_sleep()` (`br_sched.h`). They are kept only for the window before the
 *   scheduler exists (platform early_init) and for platform-internal parts.
 */
#ifndef BR_CORE_BR_TIME_H
#define BR_CORE_BR_TIME_H

#include <br/core/br_types.h>

/* Absolute point in time counted in microseconds (design CA-1: uint64 microseconds). */
typedef br_u64 br_time_t;

#define BR_US_PER_MS   1000u
#define BR_US_PER_SEC  1000000u

/*
 * The three timeout states (design `3-01` §14 CA-4):
 *   BR_TIMEOUT_ZERO = non-blocking (trylock semantics)
 *   BR_TIMEOUT_INF  = wait forever (not "a very long time" — "no deadline")
 * A relative value supplied by the user goes through `br_deadline_from_now()` and uniformly becomes
 * an **absolute deadline** (tickless-friendly).
 */
#define BR_TIMEOUT_ZERO  ((br_time_t)0u)
#define BR_TIMEOUT_INF   ((br_time_t)~(br_time_t)0u)

/* Relative microseconds → absolute deadline (saturating: overflow must not wrap into "timeout now"). */
br_time_t br_deadline_from_now(br_time_t rel_us);

/*
 * Initialise clock conversion with the platform arch timer frequency. Must be called once before
 * br_clock_now/br_delay_*, and **must come before br_log_init()** (log timestamps need it).
 */
void br_clock_init(void);

/* Monotonically non-decreasing microseconds since boot. No overflow-protection promise (2^64 us ≈ 580,000 years). */
br_time_t br_clock_now(void);

/* Platform clock frequency (Hz), for logging and diagnostics only. Valid only after br_clock_init(). */
br_u64 br_clock_freq_hz(void);

/* Conversion basis: ticks per millisecond (= freq/1000). For diagnostics — it sets the resolution. */
br_u64 br_clock_ticks_per_ms(void);

/*
 * ---- Clock tick frequency (product configuration; cf. Linux's CONFIG_HZ) ----
 *
 * Semantics: **how many clock interrupts per second**. Every 1/HZ second the platform's timer ISR
 * calls `br_clock_tick_notify()` once, so core's tick count (= jiffies) advances at HZ; the default
 * 200 ⇒ one tick per 5 ms.
 *
 * ★ The value comes from `product.toml`'s `[kernel].hz`, which `brickie build` turns into
 *   `-DBR_CFG_TICK_HZ=<hz>` for all translation units (ADR-0017) — so "how many clock interrupts per
 *   second" has a single source of truth: the platform's reload period and core's jiffies read the
 *   same macro. The `#ifndef` below is the fallback for **builds that bypass brickie** (host cases /
 *   single-file self-tests), using the same 200 as the product default.
 *
 * The 100000 upper bound is a compile-time guard (see `_Static_assert`): any higher and the period
 * would truncate to 0.
 */
#ifndef BR_CFG_TICK_HZ
#define BR_CFG_TICK_HZ 200u
#endif
_Static_assert((BR_CFG_TICK_HZ) >= 1u && (BR_CFG_TICK_HZ) <= 100000u,
               "BR_CFG_TICK_HZ out of range: valid range 1..100000 (product.toml [kernel].hz)");
/* One tick's length (us). A non-dividing HZ rounds down (actual frequency slightly above HZ, per-tick error < 1 us). */
#define BR_CFG_TICK_PERIOD_US (1000000u / (BR_CFG_TICK_HZ))

/* The tick frequency configured above (Hz); the single read point for logs/cases (defined in core/src/time.c). */
br_u32 br_clock_tick_hz(void);

/*
 * ---- IRQ heartbeat count (owned by core; the platform's timer ISR notifies every tick) ----
 *
 * ★ Why it moved into core: the counter used to live in the platform plugin, so consumers (APP) had
 *   to `#include <br/platform/br_plat.h>` just to read it — that left an `app → platform` runtime
 *   edge on the declaration surface, while design `1-01` §8's three-layer pattern and `3-01` §13.6's
 *   privilege tiers both require APP to be a **P0 consumer** only (it has no "interrupt control"
 *   capability, so it should not know the platform plugin's interface).
 *   ⇒ The orientation is reversed: **platform notifies core every tick, core holds the count**, and
 *   consumers read only the core interface. On the platform side only the single write entry
 *   `br_clock_tick_notify()` remains (called from its timer ISR).
 *
 * ISR safety: `br_clock_tick_notify()` only does one increment (no lock/log/allocation) and may be
 * called from interrupt context. `br_clock_tick_count()` is a read-only snapshot, callable from any
 * context.
 *
 * Relation to the design: `3-01` §4's time group has only `br_clock_now` / `br_deadline_from_now` —
 * this pair is a **prototype extension** (recorded in `docs/decisions/0016-core-timer-heartbeat.md`);
 * what it repays is item ② of `br-wa-boot-001` in `WORKAROUNDS.md` ("APP reads platform identity/
 * heartbeat directly"), not a new general capability.
 *
 * Capacity: `br_u32` ⇒ at the default 200 Hz, 2^32 ticks ≈ 248 days without wrap (same shape as
 * Linux's jiffies). It is a **jiffies-semantics** diagnostic/tick counter; for wall-clock time
 * (microseconds) use `br_clock_now()`, and do not derive time from tick counts. Compare tick
 * differences with the unsigned wrap-safe idiom (the old Linux rule).
 */
void br_clock_tick_notify(void);
br_u32 br_clock_tick_count(void);

/*
 * Busy-wait delay (**busy-wait only, not sleep**; from v0.2 the official sleep entry is
 * `br_task_sleep()`). There is no semantic "can be woken" and it never gives up the CPU ⇒ it suits
 * only the window where "the scheduler does not exist yet" (platform early_init) and
 * platform-internal parts. It guarantees **no early wakeup** (same semantics as br_task_sleep: no
 * early wakeup, no upper bound on lateness) — hence the conversion rounds ticks/us **up**.
 */
void br_delay_us(br_time_t us);
void br_delay_ms(br_time_t ms);

#endif /* BR_CORE_BR_TIME_H */
