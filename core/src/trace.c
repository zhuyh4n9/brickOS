/*
 * brickOS prototype v0.1.0 — minimal trace ring (ISR-safe event trail)
 *
 * Authoritative design: `docs/5-debug/5-01-debug.md` (16 B fixed events, recordable
 * from ISR) and the many "count + trail" obligations in `docs/3-os-core/3-02-int.md`:
 *   §4.2 capability downgrade must leave a trace / §7.1 priority quantization reported
 *   both ways / §5.2 spurious / §8.3 storm protection default action = trace + continue /
 *   §9.3/§9.6 domain events / §10.5/§10.6 fault.
 *
 * ISR safety (hard constraint): br_trace_emit does only "one slot write + one head
 * increment", **no locks, no allocation, no clock read, no logging**. That makes it the
 * only trail facility callable from ISR/fault paths (spirit of the CA-3 whitelist).
 * Single-core v0.1 ⇒ single producer/single consumer, no atomics needed.
 */
#include <br/core/br_trace.h>

static br_trace_evt_t s_ring[BR_TRACE_RING_SIZE];
static br_u32 s_head;      /* producer: total emitted (monotonic) */
static br_u32 s_tail;      /* consumer: total consumed */
static br_u32 s_overrun;   /* producer overwrites of not-yet-consumed slots */

void br_trace_emit(br_u32 id, br_u32 a, br_u64 b)
{
    /* ring full (head - tail == SIZE) ⇒ this entry overwrites the oldest unconsumed slot */
    if ((s_head - s_tail) >= (br_u32)BR_TRACE_RING_SIZE) {
        s_overrun++;
        /* ★ tail is **not advanced here**: when full the semantics are "drop oldest, keep
         *   newest", and only the consumer (drain) may advance tail. The cost is that the
         *   consumer may replay overwritten slots — so br_trace_overrun() > 0 is the only
         *   honest signal that this history is untrustworthy. */
    }

    br_trace_evt_t *slot = &s_ring[s_head % (br_u32)BR_TRACE_RING_SIZE];
    slot->id = id;
    slot->a  = a;
    slot->b  = b;

    s_head++;
}

br_u32 br_trace_total(void)
{
    return s_head;
}

br_u32 br_trace_drain(br_trace_evt_t *out, br_u32 max)
{
    if (out == BR_NULL || max == 0u) {
        return 0u;
    }

    /* Snapshot head first: later producer (possibly ISR) progress does not affect this view */
    const br_u32 head    = s_head;
    const br_u32 backlog = head - s_tail;
    br_u32 start;
    br_u32 avail;

    /*
     * ★ The start must be the **oldest unconsumed event** (s_tail), not `head - avail`
     *   — the latter reads the "latest max entries" when `avail` is clipped by `max`
     *   and pushes `s_tail` straight to head, **silently dropping the backlog in
     *   between** (observed: 160 backlogged during a storm, consumer takes only 16 per
     *   call ⇒ the offending entry is skipped and the test case fails).
     *   The only allowed discard is when the producer has run more than one full lap:
     *   then the earliest entries are **physically** overwritten, so start at
     *   `head - SIZE` (and br_trace_overrun() > 0).
     */
    if (backlog > (br_u32)BR_TRACE_RING_SIZE) {
        start = head - (br_u32)BR_TRACE_RING_SIZE;   /* overwritten part cannot be read back */
        avail = (br_u32)BR_TRACE_RING_SIZE;
    } else {
        start = s_tail;
        avail = backlog;
    }

    if (avail > max) {
        avail = max;                                 /* take only the oldest max, leave the rest */
    }

    for (br_u32 i = 0u; i < avail; i++) {
        out[i] = s_ring[(start + i) % (br_u32)BR_TRACE_RING_SIZE];
    }
    s_tail = start + avail;

    return avail;
}

br_u32 br_trace_overrun(void)
{
    return s_overrun;
}

const char *br_trace_name(br_u32 id)
{
    switch (id) {
    case BR_TRACE_NONE:                    return "none";
    /* interrupt path */
    case BR_TRACE_IRQ_ENTER:               return "irq_enter";
    case BR_TRACE_IRQ_EXIT:                return "irq_exit";
    case BR_TRACE_IRQ_SPURIOUS_OWNED:      return "irq_spurious_owned";
    case BR_TRACE_IRQ_SPURIOUS_NOCAP:      return "irq_spurious_nocap";
    case BR_TRACE_IRQ_SUPPRESSED:          return "irq_suppressed";
    case BR_TRACE_IRQ_STORM:               return "irq_storm";
    case BR_TRACE_IRQ_PRIO_QUANTIZED:      return "irq_prio_quantized";
    case BR_TRACE_IRQ_DISPATCH_IGNORED:    return "irq_dispatch_ignored";
    case BR_TRACE_IRQ_UNLOCK_UNDERFLOW:    return "irq_unlock_underflow";
    /* cascade domain */
    case BR_TRACE_IRQ_DEMUX:               return "irq_demux";
    case BR_TRACE_IRQ_DEMUX_OVERFLOW:      return "irq_demux_overflow";
    case BR_TRACE_IRQ_DOMAIN_ORPHAN:       return "irq_domain_orphan";
    case BR_TRACE_IRQ_DOMAIN_DROP:         return "irq_domain_drop";
    case BR_TRACE_IRQ_DOMAIN_NOBH:         return "irq_domain_nobh";
    /* fault path */
    case BR_TRACE_FAULT:                   return "fault";
    case BR_TRACE_FAULT_FIXUP:             return "fault_fixup";
    case BR_TRACE_FAULT_DOUBLE:            return "fault_double";
    /* bottom half (ADR-0011) */
    case BR_TRACE_IRQ_BH_DEFER:            return "irq_bh_defer";
    case BR_TRACE_IRQ_BH_DROP:             return "irq_bh_drop";
    case BR_TRACE_IRQ_DISPATCH_SUPPRESSED: return "irq_dispatch_suppressed";
    case BR_TRACE_BH_CTX_VIOLATION:        return "bh_ctx_violation";
    case BR_TRACE_SCHED_PREEMPT:           return "sched_preempt";
    default:                               return "?";
    }
}
