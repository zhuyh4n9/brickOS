/*
 * Host-side scheduler test harness shared by the sched-test (coop), rr-test and work-test suites:
 * fake clock / L2 / log / panic stand-ins plus the ucontext switch layer (a real stack switch).
 * Output protocol: `[HOSTTEST] PASS|FAIL <tag> <desc>` and one `[HOSTTEST] SUMMARY` line.
 */
#ifndef BR_TESTS_HOST_SCHED_H
#define BR_TESTS_HOST_SCHED_H

#include <br/hal/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* Create a supervisor thread, run the selected plugin's `*_early_init()` first (it does the
 * `br_sched_register`), then enter `br_sched_run()`. Does not return. */
void host_sched_main(void (*supervisor)(void *));

/* Fake clock: current value / advance (advancing does **not** deliver a tick). */
br_time_t host_sched_now(void);
void      host_sched_advance_us(br_time_t us);

/* Number of idle entries (core calls the host hook when nothing is runnable; each advance is
 * 100 us and delivers one tick). */
br_u32 host_sched_idle_calls(void);

/* Make `br_log_write` also count occurrences of a substring; used to turn a suite's
 * `[XXXCONF] FAIL` into a host exit-code verdict. */
void host_sched_watch(const char *needle);
br_u32 host_sched_watch_hits(void);

/* Deliver one tick to **the core tick driver point** (the target timer ISR line). */
void host_sched_tick(void);

/*
 * Turn "read the clock" into an asynchronous tick source (off by default): br_clock_now() advances
 * HOST_CLOCK_STEP_US per call and, once a tick period accumulates, delivers br_sched_on_tick() +
 * br_sched_irq_epilogue() at depth 0, as on the target ISR exit. Time advances inside an L2
 * critical section but no tick is delivered (TC-TASK-103); deterministic cases inject their own.
 */
void host_sched_auto_tick(br_bool on);

#endif /* BR_TESTS_HOST_SCHED_H */
