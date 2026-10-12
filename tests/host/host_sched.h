/*
 * tests/host/host_sched.h — host-side scheduler test harness (shared harness, not a second target implementation)
 *
 * Why a shared harness: host semantic cases for "sched framework + one scheduler plugin" all
 * need the same set of stand-ins (fake clock / L2 / log / panic / ucontext switch layer /
 * trampoline). The three suites `sched-test` (coop), `rr-test` and `work-test` used to copy
 * this block three times — exactly the start of a "second source of truth" (one copy updated,
 * another forgotten, so some case silently tests other semantics). ⇒ consolidate here.
 *
 * Layer split (consistent with the ruling in ADR-0006 §4):
 *   switch layer = `core/src/sched/switch.S` (**compiled on aarch64 only**);
 *   policy layer = `core/src/sched/sched_core.c` + scheduler plugin (**compiled on both**).
 * This harness provides the host switch layer (ucontext: a real stack switch, not a fake one)
 * and the target-side stubs.
 *
 * Output protocol (per tests/host/mem_test.c):
 *   [HOSTTEST] PASS|FAIL <tag> <desc> … [HOSTTEST] SUMMARY pass=N fail=M total=T
 */
#ifndef BR_TESTS_HOST_SCHED_H
#define BR_TESTS_HOST_SCHED_H

#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* Create a supervisor thread and enter `br_sched_run()` (does not return; the supervisor body
 * runs the cases and exits). Run the chosen scheduler plugin's `*_early_init()` first (it does
 * the `br_sched_register`). */
void host_sched_main(void (*supervisor)(void *));

/* Fake clock: current value / advance (advancing does **not** deliver a tick — the case decides when to tick). */
br_time_t host_sched_now(void);
void      host_sched_advance_us(br_time_t us);

/* Number of idle entries (core calls the host hook when nothing is runnable; each advance is
 * 100 us and delivers one tick). */
br_u32 host_sched_idle_calls(void);

/* Make `br_log_write` also watch for a substring (count on hit); used to turn a suite's
 * `[XXXCONF] FAIL` into a host exit-code verdict. */
void host_sched_watch(const char *needle);
br_u32 host_sched_watch_hits(void);

/* Deliver one tick to **the core tick driver point** (equivalent to the timer ISR line on target). */
void host_sched_tick(void);

/*
 * Turn "read the clock" into an **asynchronous tick source** (off by default).
 *
 * On target the timer ISR is asynchronous: even a busy loop that never yields gets interrupted
 * by a tick. Host has no interrupts, so the only place to "cut in while someone else runs" is
 * **the moment it itself reads the clock** — so with this switch on, `br_clock_now()` advances
 * `HOST_CLOCK_STEP_US` each call, and once a tick period accumulates it delivers
 * `br_sched_on_tick()` + `br_sched_irq_epilogue()` when **the interrupt-disable depth is 0**
 * (line for line the same as the target ISR exit). It does **not** deliver inside an L2
 * critical section (= interrupts masked on target), but time still advances — which is exactly
 * why `TC-TASK-103` ("no preemption inside a critical section") is meaningful on host too.
 *
 * Why off by default: cases needing a **deterministic tick** (e.g. the rr-test AABB sequence)
 * inject their own; an automatic source would mix an extra tick into the sequence.
 */
void host_sched_auto_tick(br_bool on);

#endif /* BR_TESTS_HOST_SCHED_H */
