/*
 * Minimal core-owned ISR-safe trace ring of 16 B events (WORKAROUND(br-wa-boot-001): no service
 * registry yet). br_trace_emit is single-producer lock-free: one slot write + one head increment,
 * no lock/alloc/clock read. Overflow overwrites unconsumed slots rather than blocking interrupts.
 */
#ifndef BR_CORE_BR_TRACE_H
#define BR_CORE_BR_TRACE_H

#include <br/core/br_types.h>

enum {
    BR_TRACE_NONE = 0,
    /* interrupt path */
    BR_TRACE_IRQ_ENTER = 1,          /* a = virq, b = hwirq */
    BR_TRACE_IRQ_EXIT = 2,           /* a = virq */
    BR_TRACE_IRQ_SPURIOUS_OWNED = 3, /* a = hwirq (arrived despite hardware mask) */
    BR_TRACE_IRQ_SPURIOUS_NOCAP = 4, /* a = hwirq (expected drop when masking is unavailable) */
    BR_TRACE_IRQ_SUPPRESSED = 5,     /* a = virq (software suppression with CAP_MASK=0) */
    BR_TRACE_IRQ_STORM = 6,          /* a = virq, b = count in window */
    BR_TRACE_IRQ_PRIO_QUANTIZED = 7, /* a = virq, b = (encoded << 8) | logical */
    BR_TRACE_IRQ_DISPATCH_IGNORED = 8, /* a = virq, b = ignored dispatch form */
    BR_TRACE_IRQ_UNLOCK_UNDERFLOW = 9, /* a = excess credits (caught by the underflow guard) */
    /* cascade domain */
    BR_TRACE_IRQ_DEMUX = 10,          /* a = dom_id, b = sub-bitmap */
    BR_TRACE_IRQ_DEMUX_OVERFLOW = 11, /* a = dom_id (rounds exhausted, still not drained) */
    BR_TRACE_IRQ_DOMAIN_ORPHAN = 12,  /* a = dom_id, b = sub (no owner => mask) */
    BR_TRACE_IRQ_DOMAIN_DROP = 13,    /* a = dom_id (defer/drop on SLOW domain commit failure) */
    BR_TRACE_IRQ_DOMAIN_NOBH = 14,    /* a = requested flags (SLOW rejected: no bh) */
    /* fault path */
    BR_TRACE_FAULT = 15,              /* a = EC, b = ESR_EL1 */
    BR_TRACE_FAULT_FIXUP = 16,        /* a = (br_u32)ELR, b = errno_val */
    BR_TRACE_FAULT_DOUBLE = 17,       /* a = in_fault (>=2) */
    /* bottom half */
    BR_TRACE_IRQ_BH_DEFER = 18,       /* a = virq (line deferred to bh; hardware line masked in the window) */
    BR_TRACE_IRQ_BH_DROP = 19,        /* a = virq (queue full => roll back uncommitted) */
    BR_TRACE_IRQ_DISPATCH_SUPPRESSED = 20, /* a = virq (disabled at dispatch) */
    BR_TRACE_BH_CTX_VIOLATION = 21,   /* a = violating API id (block/yield/exit called inside bh) */
    BR_TRACE_SCHED_PREEMPT = 22,      /* a = 0, b = 1 real stack switch, 0 = no switch needed */
    BR_TRACE__COUNT
};

typedef struct {
    br_u32 id;
    br_u32 a;
    br_u64 b;
} br_trace_evt_t;

_Static_assert(sizeof(br_trace_evt_t) == 16u, "trace event must be exactly 16 B");

#define BR_TRACE_RING_SIZE  256u

/* ISR-safe: record one event (no lock / no allocation / no clock read). */
void br_trace_emit(br_u32 id, br_u32 a, br_u64 b);

/* Cumulative number of events emitted (monotonic). */
br_u32 br_trace_total(void);

/* Copy up to `max` unconsumed events into `out`; returns the count. Thread context only. */
br_u32 br_trace_drain(br_trace_evt_t *out, br_u32 max);

/* Times a producer overwrote an unconsumed slot (>0: ring too small or consumer too slow). */
br_u32 br_trace_overrun(void);

/* Event name text; unknown id => "?". */
const char *br_trace_name(br_u32 id);

#endif /* BR_CORE_BR_TRACE_H */
