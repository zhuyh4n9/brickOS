/*
 * brickOS prototype v0.1.0 — minimal trace ring (ISR-safe event tracing facility)
 *
 * Design basis: `docs/5-debug/5-01-debug.md` (16 B fixed-size event + ctx field; recordable inside ISR)
 * and the many "**count + trace**" obligations in `3-02-int.md`:
 *   §4.2 capability-degradation behavior (must trace), §7.1 priority quantization **reports both directions**,
 *   §5.2 spurious, §8.3 storm protection (default action = trace + continue), §9.3/§9.4 domain events.
 *
 * ⚠ Ownership note: on the design side trace is a **Service/observation facility** (5-01), not a core internal.
 *   This prototype has no service registry (WORKAROUND br-wa-boot-001), so this is a
 *   **minimal ring buffer inside core**, only to turn those "must trace" rulings into observable evidence;
 *   once service/trace lands, the `br_trace_emit` call sites stay and only the implementation is swapped for the service.
 *
 * ISR safety: `br_trace_emit` is a single-producer-write, single-consumer-read lock-free ring (single core, v0.1) —
 * just one slot write + one head increment; no lock, no allocation, no clock read.
 * The consumer (thread context) snapshots head, copies entries one by one, then advances tail; a producer overflow overwrites
 * **unconsumed** old slots (recorded as `br_trace_overrun()`), a deliberate trade of "drop old events rather than block interrupts".
 */
#ifndef BR_CORE_BR_TRACE_H
#define BR_CORE_BR_TRACE_H

#include <br/core/br_types.h>

/* event ids (same shape as the 5-01 §1 event family: plain numbers, for both text and binary presentation) */
enum {
    BR_TRACE_NONE = 0,
    /* interrupt path */
    BR_TRACE_IRQ_ENTER = 1,          /* a = virq, b = hwirq */
    BR_TRACE_IRQ_EXIT = 2,           /* a = virq */
    BR_TRACE_IRQ_SPURIOUS_OWNED = 3, /* a = hwirq (arrived despite hardware mask ⇒ real bug signal) */
    BR_TRACE_IRQ_SPURIOUS_NOCAP = 4, /* a = hwirq (expected drop when masking is unavailable) */
    BR_TRACE_IRQ_SUPPRESSED = 5,     /* a = virq (software suppression with CAP_MASK=0) */
    BR_TRACE_IRQ_STORM = 6,          /* a = virq, b = count in window */
    BR_TRACE_IRQ_PRIO_QUANTIZED = 7, /* a = virq, b = (encoded << 8) | logical */
    BR_TRACE_IRQ_DISPATCH_IGNORED = 8, /* a = virq, b = ignored dispatch form (★ only THREAD remains after ADR-0011) */
    BR_TRACE_IRQ_UNLOCK_UNDERFLOW = 9, /* a = excess credits (caught by the underflow guard) */
    /* cascade domain */
    BR_TRACE_IRQ_DEMUX = 10,          /* a = dom_id, b = sub-bitmap */
    BR_TRACE_IRQ_DEMUX_OVERFLOW = 11, /* a = dom_id (rounds exhausted, still not drained) */
    BR_TRACE_IRQ_DOMAIN_ORPHAN = 12,  /* a = dom_id, b = sub (no owner ⇒ mask) */
    BR_TRACE_IRQ_DOMAIN_DROP = 13,    /* a = dom_id (defer/drop on SLOW domain commit failure) */
    BR_TRACE_IRQ_DOMAIN_NOBH = 14,    /* a = requested flags (reason SLOW domain was rejected; only "bh unavailable" after ADR-0011) */
    /* fault path */
    BR_TRACE_FAULT = 15,              /* a = EC, b = ESR_EL1 */
    BR_TRACE_FAULT_FIXUP = 16,        /* a = (br_u32)ELR, b = errno_val */
    BR_TRACE_FAULT_DOUBLE = 17,       /* a = in_fault (≥2) */
    /* bottom half (ADR-0011 — design 3-02 §11.1/§11.4.1/§9.4) */
    BR_TRACE_IRQ_BH_DEFER = 18,       /* a = virq (line deferred to bh; hardware line masked in the window) */
    BR_TRACE_IRQ_BH_DROP = 19,        /* a = virq (queue full ⇒ roll back uncommitted; IR-10's only failure path) */
    BR_TRACE_IRQ_DISPATCH_SUPPRESSED = 20, /* a = virq (disabled at dispatch ⇒ suppress handler, no replay) */
    BR_TRACE_BH_CTX_VIOLATION = 21,   /* a = violating API id (block/yield/exit called inside bh) */
    BR_TRACE_SCHED_PREEMPT = 22,      /* a = 0, b = 1 real stack switch / 0 = requested but no switch needed */
    BR_TRACE__COUNT
};

/* 16 B fixed-size event (the shape in 5-01 §1) */
typedef struct {
    br_u32 id;
    br_u32 a;
    br_u64 b;
} br_trace_evt_t;

_Static_assert(sizeof(br_trace_evt_t) == 16u, "trace event must be exactly 16 B");

#define BR_TRACE_RING_SIZE  256u

/* ISR-safe: record one event (no lock / no allocation / no clock read). */
void br_trace_emit(br_u32 id, br_u32 a, br_u64 b);

/* Cumulative number of events emitted (monotonic; used to tell whether anything is new). */
br_u32 br_trace_total(void);

/* Copy **unconsumed** events into out (at most max), returns the number copied. Called from thread context. */
br_u32 br_trace_drain(br_trace_evt_t *out, br_u32 max);

/* Times a producer overwrote an unconsumed slot (>0 means the trace ring is too small / consumption too slow). */
br_u32 br_trace_overrun(void);

/* Readable text for an event name (for presentation; unknown id ⇒ "?"). */
const char *br_trace_name(br_u32 id);

#endif /* BR_CORE_BR_TRACE_H */
