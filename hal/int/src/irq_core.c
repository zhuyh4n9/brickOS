/* IRQ core: descriptor pool, CPU-local state, lifecycle, masking, storm detection, bottom half.
 * Lifecycle = ack -> ISR -> single eoi point; three mask layers; smaller prio value = higher
 * priority. bh runs at IRQ exit (after eoi, before ERET) via the workqueue that `hal/sched`
 * installs in `br_irq_hooks_t`; DISPATCH_THREAD is still unimplemented (ignored + traced). No
 * controller model is named here; hardware goes through br_pic_ops.
 *
 * This file deliberately has **no** `hal/sched` include: the exit hook and the bh primitives arrive
 * through the hook table (ADR-0039), which is what keeps the interrupt brick below the scheduler. */
#include <br/core/br_error.h>
#include <br/hal/br_exc.h>
#include <br/hal/br_fault.h>
#include <br/hal/br_irq.h>
#include <br/hal/br_pic.h>
#include <br/core/br_trace.h>

#include "irq_internal.h"

/* ---- Static pools (all BSS, no runtime construction) ---- */

static br_irq_desc_t s_desc[BR_IRQ_MAX];   /* config and identity */
static br_irq_rt_t   s_rt[BR_IRQ_MAX];     /* runtime counters */
static br_irq_cpu_t  s_cpu;                /* this core's IRQ state (TPIDR_EL1 points here) */

/* Cumulative counters saturate rather than wrap (a wrapped counter reads as "problem gone");
 * cursor-like values (rt->storm) are exempt. */
static br_u32 sat_inc_u32(br_u32 v)
{
    return (v == 0xFFFFFFFFu) ? v : (v + 1u);
}

/* Saturating u32 -> u16 narrowing, only for small window values. */
static br_u16 sat_u16(br_u32 v)
{
    return (v > 0xFFFFu) ? (br_u16)0xFFFFu : (br_u16)v;
}

/* ---- CPU-local (TPIDR_EL1 is core-exclusive) ---- */

br_irq_cpu_t *br_irq_cpu(void)
{
    return &s_cpu;
}

void br_irq_cpu_init(void)
{
    /* Must run after BSS zeroing and before br_irq_bindings_set(): sets pic_id/dom_id of all
     * descriptors to -1 (P-IRQ-5), since 0 is legal for both and unbound must report -ENODEV. */
    for (br_u32 v = 0u; v < BR_IRQ_MAX; v++) {
        s_desc[v].pic_id = (br_s8)-1;
        s_desc[v].dom_id = (br_s8)-1;
    }

    /* v1 has no fault handler chain, so in_handler stays 0xFFFF; keeps diagnostics honest. */
    s_cpu.in_handler = BR_IRQ_NO_HANDLER;

    /* Attach this core's CPU-local to TPIDR_EL1. */
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&s_cpu) : "memory");
}

void br_irq_cpu_enable(void)
{
    /* Unmasks PSTATE.I only (F/A/D unchanged); called after the LATE phase, before START. */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

/* ---- Descriptor / runtime table access; out of range and BR_IRQ_INVALID both give NULL ---- */

br_irq_desc_t *br_irq_desc_of(br_u32 virq)
{
    if (virq >= BR_IRQ_MAX) {          /* BR_IRQ_INVALID (UINT32_MAX) lands here too */
        return BR_NULL;
    }
    return &s_desc[virq];
}

br_irq_rt_t *br_irq_rt_of(br_u32 virq)
{
    if (virq >= BR_IRQ_MAX) {
        return BR_NULL;
    }
    return &s_rt[virq];
}

/* ---- IRQ-exit hooks (ADR-0039) ----
 * Installed by `hal/sched`'s early_init; NULL until then (see br_irq.h). The IRQ core must not
 * depend on the scheduler brick, so the mechanism owner registers instead of being called. */
static const br_irq_hooks_t *s_hooks;

void br_irq_set_hooks(const br_irq_hooks_t *hooks)
{
    s_hooks = hooks;
}

/* Bh submit for the rest of the interrupt brick (irq_domain.c): the hook table is the only path to
 * the workqueue, so the brick never names `hal/sched`. -EAGAIN when nothing is installed yet
 * (boot) -- the caller's rollback path is the same one a full queue takes. */
int br_irq_bh_submit(void (*fn)(void *arg), void *arg)
{
    if (s_hooks == BR_NULL || s_hooks->bh_submit == BR_NULL) {
        return -EAGAIN;
    }
    return s_hooks->bh_submit(fn, arg);
}

br_bool br_irq_in_isr(void)
{
    return (s_cpu.isr_nest > 0u) ? BR_TRUE : BR_FALSE;
}

/* Thread-only enforcement tightens to "in atomic context": bh runs with isr_nest == 0 but on the
 * IRQ-disabled exception exit, where registering or blocking would stall the frame. Kept separate
 * from br_irq_in_isr so tests can tell the contexts apart. */
br_bool br_irq_in_atomic(void)
{
    if (br_irq_in_isr() != 0) {
        return BR_TRUE;
    }
    return (s_hooks != BR_NULL && s_hooks->in_bh != BR_NULL && s_hooks->in_bh() != BR_FALSE)
               ? BR_TRUE
               : BR_FALSE;
}

br_u32 br_irq_lock_depth(void)
{
    return s_cpu.irq_depth;
}

/* ---- L2: br_irq_lock / br_irq_unlock (ISR-safe: no locking, tracing or clock reads) ---- */

/* br_irq_state_t: bit0..3 = I/F/A/D before entry, [17:4] = depth copy. */
static br_irq_state_t encode_state(br_u32 raw_daif, br_u32 depth)
{
    br_irq_state_t st = 0u;

    if ((raw_daif & BR_DAIF_I) != 0u) {
        st |= BR_IRQ_ST_I_MASK;
    }
    if ((raw_daif & BR_DAIF_F) != 0u) {
        st |= BR_IRQ_ST_F_MASK;
    }
    if ((raw_daif & BR_DAIF_A) != 0u) {
        st |= BR_IRQ_ST_A_MASK;
    }
    if ((raw_daif & BR_DAIF_D) != 0u) {
        st |= BR_IRQ_ST_D_MASK;
    }

    st |= (br_irq_state_t)((depth & 0x3FFFu) << BR_IRQ_ST_DEPTH_SHIFT);
    return st;
}

/* Rebuild the dialect raw from st's 4 mask bits; valid only where raw DAIF bit positions are the
 * PSTATE positions (aarch64). The underflow guard still honors st's I bit. */
static br_u32 raw_from_state(br_irq_state_t st)
{
    br_u32 raw = 0u;

    if ((st & BR_IRQ_ST_I_MASK) != 0u) {
        raw |= BR_DAIF_I;
    }
    if ((st & BR_IRQ_ST_F_MASK) != 0u) {
        raw |= BR_DAIF_F;
    }
    if ((st & BR_IRQ_ST_A_MASK) != 0u) {
        raw |= BR_DAIF_A;
    }
    if ((st & BR_IRQ_ST_D_MASK) != 0u) {
        raw |= BR_DAIF_D;
    }
    return raw;
}

static void restore_irq_raw(br_u32 raw)
{
    const br_u32 pic_id = br_irq_cpu_pic_id();
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);

    if (pic != BR_NULL && ops != BR_NULL && ops->cpu_unmask != BR_NULL) {
        ops->cpu_unmask(pic, raw);
    }
}

br_irq_state_t br_irq_lock(void)
{
    br_irq_cpu_t *cpu = &s_cpu;
    const br_u32 pic_id = br_irq_cpu_pic_id();
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    br_u32 raw;

    if (pic != BR_NULL && ops != BR_NULL && ops->cpu_mask != BR_NULL) {
        raw = ops->cpu_mask(pic);          /* L2 proper = dialect wrapper over PSTATE.I */
    } else {
        /* Before the PIC is registered there is no dialect to call and IRQs cannot really be
         * disabled (that window is the global-IRQ-disabled EARLY phase), so synthesize all mask
         * bits set. P-IRQ-6. */
        raw = BR_DAIF_I | BR_DAIF_F | BR_DAIF_A | BR_DAIF_D;
    }

    const br_irq_state_t st = encode_state(raw, cpu->irq_depth);

    /* The outermost (0->1) lock records the raw state for unlock's exact restore. P-IRQ-14: store
     * the dialect DAIF raw rather than the st encoding, since cpu_unmask takes dialect raw; the u32
     * field fits either, so the layout is unchanged. */
    if (cpu->irq_depth == 0u) {
        cpu->saved_daif = raw;
    }
    cpu->irq_depth++;

    return st;
}

void br_irq_unlock(br_irq_state_t st)
{
    br_irq_cpu_t *cpu = &s_cpu;

    /* The underflow guard must come first: decrementing a zero depth wraps to 0xFFFFFFFF, so it
     * never reaches 0 again and this core's IRQs stay off with no error reported. */
    if (cpu->irq_depth == 0u) {
        /* No dedicated underflow event exists, so st's depth copy distinguishes stale from surplus
         * unlock. */
        const br_u32 extra = (br_u32)((st & BR_IRQ_ST_DEPTH_MASK) >> BR_IRQ_ST_DEPTH_SHIFT);
        br_trace_emit(BR_TRACE_IRQ_UNLOCK_UNDERFLOW, extra, 0u);
        restore_irq_raw(raw_from_state(st));    /* honors st's I bit */
        return;
    }

    cpu->irq_depth--;
    if (cpu->irq_depth != 0u) {
        return;                                 /* inner unlock never enables IRQs (TC-IRQ-004) */
    }

    /* v1 manages I only; restore the raw captured at the outermost lock. */
    restore_irq_raw(cpu->saved_daif);
}

/* ---- Per-line masking (L1) ---- */

/* Valid for direct lines only: a domain member's pic_id names the parent PIC and its hwirq is the
 * in-domain child number, so masking it here would hit the wrong physical line. Domain child IRQs
 * are masked via br_irq_domain_ops.mask. */
void br_irq_pgm_mask(br_irq_desc_t *d)
{
    if (d == BR_NULL || d->pic_id < 0 || d->dom_id >= 0) {
        return;
    }
    const br_u32 pic_id = (br_u32)d->pic_id;
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);

    if (pic == BR_NULL || ops == BR_NULL || caps == BR_NULL) {
        return;
    }
    /* No hardware mask capability => the software shadow is the last gate; nothing to do. */
    if ((caps->caps & BR_PIC_CAP_MASK) == 0u) {
        return;
    }
    if (ops->mask != BR_NULL) {
        ops->mask(pic, d->hwirq);
    }
}

void br_irq_pgm_unmask(br_irq_desc_t *d)
{
    if (d == BR_NULL || d->pic_id < 0 || d->dom_id >= 0) {
        return;
    }
    const br_u32 pic_id = (br_u32)d->pic_id;
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);

    if (pic == BR_NULL || ops == BR_NULL || caps == BR_NULL) {
        return;
    }
    if ((caps->caps & BR_PIC_CAP_MASK) == 0u) {
        return;
    }
    if (ops->unmask != BR_NULL) {
        ops->unmask(pic, d->hwirq);
    }
}

/* ---- Capability degradation and priority quantization (IR-7) ---- */

/* Whether the controller can express the trigger; a missing capability is never rewritten. */
static br_bool trigger_supported(const br_pic_caps_t *caps, br_u8 trigger)
{
    switch (trigger) {
    case BR_IRQ_TRIG_LEVEL_HIGH:
    case BR_IRQ_TRIG_LEVEL_LOW:
        return BR_TRUE;
    case BR_IRQ_TRIG_EDGE_RISE:
    case BR_IRQ_TRIG_EDGE_FALL:
        return ((caps->caps & BR_PIC_CAP_TRIG_EDGE) != 0u) ? BR_TRUE : BR_FALSE;
    case BR_IRQ_TRIG_EDGE_BOTH:
        return ((caps->caps & BR_PIC_CAP_TRIG_BOTH) != 0u) ? BR_TRUE : BR_FALSE;
    default:
        return BR_FALSE;
    }
}

/* P-IRQ-3: step = 1 << (8 - prio_bits) assumes MSB-aligned hardware encoding (GICv3), so rounding
 * up saturates the logical priority toward lower priority. LSB-aligned dialects encode on their
 * side; the core only sees the "small = high" order. */
static void trace_prio_quantized(br_u32 virq, br_u8 prio, const br_pic_caps_t *caps)
{
    /* Caller invokes this only when CAP_PRIO is set; the no-CAP_PRIO trace happens in register. */
    if (caps->prio_bits < 1u || caps->prio_bits > 7u) {
        return;     /* prio_bits 0 or >= 8: no quantization */
    }

    const br_u32 step = 1u << (8u - (br_u32)caps->prio_bits);
    if (((br_u32)prio & (step - 1u)) == 0u) {
        return;     /* already on a realizable step */
    }

    /* Round toward larger value = lower priority, never down (which would raise it). enc stays u32:
     * for prio near 0xFF the rounded value is 256, faithfully recording "not expressible" instead
     * of wrapping to 0, which would read as highest priority. */
    const br_u32 enc = ((br_u32)prio + step - 1u) & ~(step - 1u);

    /* b = (encoded << 8) | logical */
    br_trace_emit(BR_TRACE_IRQ_PRIO_QUANTIZED, virq,
                  ((br_u64)enc << 8) | (br_u64)prio);
}

/* ---- Native API: register / enable / disable (all thread-only) ---- */

/* Runtime enforcement of thread-only: a call in ISR or bh is rejected with -EINVAL. The per-line
 * mask path may hold a core lock, and taking it in an ISR self-deadlocks; bh sits on the
 * IRQ-disabled exception exit, where register/enable would make the return path unpredictable. */
static int direct_api_guard(br_u32 irq, br_irq_desc_t **out)
{
    if (br_irq_in_atomic() != BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    br_irq_desc_t *d = br_irq_desc_of(irq);
    /* out of range, domain member, or unregistered all give -EINVAL */
    if (d == BR_NULL || d->dom_id >= 0 || d->isr == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *out = d;
    return BR_OK;
}

int br_irq_register(br_u32 irq, void (*isr)(void *), void *arg,
                    const br_irq_attr_t *attr)
{
    if (br_irq_in_atomic() != BR_FALSE) {
        return BR_ERR(EINVAL);
    }

    br_irq_desc_t *d = br_irq_desc_of(irq);
    if (d == BR_NULL || isr == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /* Families are exclusive: direct register on a domain member must fail now, else its sub number
     * would later mask an unrelated line. */
    if (d->dom_id >= 0) {
        return BR_ERR(EINVAL);
    }
    /* INV-A: each physical line has exactly one owner */
    if (d->isr != BR_NULL) {
        return BR_ERR(EBUSY);
    }
    /* Unbound => -ENODEV: a wrong init order must be reported, not silently succeed. */
    if (d->pic_id < 0) {
        return BR_ERR(ENODEV);
    }

    const br_u32 pic_id = (br_u32)d->pic_id;
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);
    if (pic == BR_NULL || ops == BR_NULL || caps == BR_NULL) {
        return BR_ERR(ENODEV);
    }

    /* Static fact from the binding table. */
    const br_bool bound_percpu = ((d->flags & (br_u16)BR_IRQ_F_PERCPU) != 0u);

    br_u32 flags = 0u;
    br_u8  prio = d->prio;            /* binding table static default (used when attr.prio == DEFAULT) */
    br_u8  trigger = d->trigger;      /* binding table static default (used when attr.trigger == DEFAULT) */
    br_bool trigger_requested = BR_FALSE;

    if (attr != BR_NULL) {
        flags = attr->flags;

        /* Unknown flag bit => -EINVAL, deliberately not ignored: a plugin built against a newer
         * header would otherwise quietly lose its requested attribute. */
        if ((flags & ~(br_u32)BR_IRQ_F_KNOWN_MASK) != 0u) {
            return BR_ERR(EINVAL);
        }
        /* Shared lines are out of scope for v1. */
        if ((flags & BR_IRQ_F_SHARED) != 0u) {
            return BR_ERR(ENOTSUP);
        }
        /* Hand-filled PERCPU contradicting the binding table => -EINVAL; the reverse (binding says
         * PPI, driver left it unset) is legal, since the bit is derived from the binding table. */
        if ((flags & BR_IRQ_F_PERCPU) != 0u && bound_percpu == BR_FALSE) {
            return BR_ERR(EINVAL);
        }
        /* Dispatch form: DISPATCH_BH defers that line's ISR to the bottom half and stays in the
         * descriptor flags as the runtime truth; DISPATCH_THREAD is accepted but ignored and
         * traced, then cleared so a descriptor never claims a form with no effect in this version.
         */
        if ((flags & BR_IRQ_F_DISPATCH_THREAD) != 0u) {
            br_trace_emit(BR_TRACE_IRQ_DISPATCH_IGNORED, irq,
                          (br_u64)BR_IRQ_F_DISPATCH_THREAD);
            flags &= ~(br_u32)BR_IRQ_F_DISPATCH_THREAD;
        }
        if ((flags & BR_IRQ_F_STATS) != 0u && (BR_IRQ_STATS == 0)) {
            return BR_ERR(ENOTSUP);       /* bit set while stats are compiled out => -ENOTSUP */
        }

        /* DEFAULT => use the binding static value. */
        if (attr->prio != (br_u8)BR_IRQ_PRIO_DEFAULT) {
            prio = attr->prio;
        }
        if (attr->trigger != (br_u8)BR_IRQ_TRIG_DEFAULT) {
            trigger = attr->trigger;
            trigger_requested = BR_TRUE;
        }
    }

    /* Legal trigger encoding range. */
    if (trigger > (br_u8)BR_IRQ_TRIG_EDGE_BOTH) {
        return BR_ERR(EINVAL);
    }

    /* Capability degradation: either deterministic weakening + trace, or -ENOTSUP. The trigger caps
     * check targets only an explicit driver request (attr.trigger != DEFAULT); the binding table's
     * static trigger is platform data about its own hardware and is trusted. */
    if (trigger_requested == BR_TRUE &&
        (caps->caps & BR_PIC_CAP_TRIG_EDGE) == 0u) {
        return BR_ERR(ENOTSUP);           /* non-DEFAULT request, no programmable trigger => reject */
    }
    if (trigger_requested == BR_TRUE && trigger == (br_u8)BR_IRQ_TRIG_EDGE_BOTH &&
        (caps->caps & BR_PIC_CAP_TRIG_BOTH) == 0u) {
        return BR_ERR(ENOTSUP);
    }

    const br_bool caps_prio = ((caps->caps & BR_PIC_CAP_PRIO) != 0u) ? BR_TRUE : BR_FALSE;
    if (caps_prio == BR_FALSE) {
        /* Controller without priority support: attr.prio is still recorded and traced, not silently
         * dropped; set_prio is not called. P-IRQ-7: the frozen event family has no "prio
         * unsupported" item, so PRIO_QUANTIZED is reused with encoded = 0 (encoded 0 + non-zero
         * logical = missing capability). */
        br_trace_emit(BR_TRACE_IRQ_PRIO_QUANTIZED, irq, ((br_u64)0u << 8) | (br_u64)prio);
    }

    /* Hardware configuration runs inside the L2 critical section, L2 before L1. */
    const br_irq_state_t st = br_irq_lock();
    int r = BR_OK;

    if (caps_prio == BR_TRUE && ops->set_prio != BR_NULL) {
        r = ops->set_prio(pic, d->hwirq, prio);
    }
    if (r == BR_OK && ops->set_trigger != BR_NULL &&
        trigger_supported(caps, trigger) == BR_TRUE) {
        r = ops->set_trigger(pic, d->hwirq, trigger);
    }
    br_irq_unlock(st);

    if (r != BR_OK) {
        return r;       /* propagate the errno; never leave a half-registered line */
    }

    /* Priority quantization trace (report on hit). */
    if (caps_prio == BR_TRUE) {
        trace_prio_quantized(irq, prio, caps);
    }

    /*
     * Commit the registration last: isr is set only now, so failure paths above leave no owner.
     * hwirq/pic_id came from br_irq_bindings_set and are not rewritten. Flags are OR-ed, not
     * overwritten: the binding table may already have written DISPATCH_BH, a separate source.
     */
    const br_u16 binding_flags =
        (br_u16)(d->flags & (br_u16)(BR_IRQ_F_PERCPU | BR_IRQ_F_DISPATCH_BH));

    d->isr     = isr;
    d->arg     = arg;
    d->flags   = (br_u16)((br_u16)flags | binding_flags |
                          (bound_percpu == BR_TRUE ? (br_u16)BR_IRQ_F_PERCPU : (br_u16)0u));
    d->prio    = prio;
    d->trigger = trigger;
    d->sub     = 0u;                 /* direct line: child number always 0 */
    d->depth   = 1u;                 /* registered = masked */

    /* After register the hardware must be masked, else enable sees depth == 0 and never unmasks. */
    br_irq_pgm_mask(d);

    return BR_OK;
}

int br_irq_enable(br_u32 irq)
{
    br_irq_desc_t *d = BR_NULL;
    const int guard = direct_api_guard(irq, &d);
    if (guard != BR_OK) {
        return guard;
    }

    const br_irq_state_t st = br_irq_lock();
    if (d->depth == 0u) {
        br_irq_unlock(st);
        return BR_OK;                /* already unmasked: idempotent success */
    }
    d->depth--;
    if (d->depth == 0u) {
        br_irq_pgm_unmask(d);        /* 1 -> 0: only the outermost touches hardware */
    }
    br_irq_unlock(st);
    return BR_OK;
}

int br_irq_disable(br_u32 irq)
{
    br_irq_desc_t *d = BR_NULL;
    const int guard = direct_api_guard(irq, &d);
    if (guard != BR_OK) {
        return guard;
    }

    const br_irq_state_t st = br_irq_lock();   /* L2 first, then hardware */
    if (d->depth == 0u) {
        br_irq_pgm_mask(d);                    /* 0 -> 1: really taken down */
    }
    d->depth++;
    br_irq_unlock(st);
    return BR_OK;
}

/* ---- Windowed storm detection (always on; safety feature) ---- */

void br_irq_storm_check(br_irq_desc_t *d)
{
    if (d == BR_NULL) {
        return;
    }

    br_irq_cpu_t *cpu = &s_cpu;

    /* Known legitimate high-rate lines can be exempted. */
    if ((d->flags & (br_u16)BR_IRQ_F_NO_STORM_GUARD) != 0u) {
        return;
    }

    /* Descriptor pointer -> virq; callers pass in-pool descriptors only. */
    const br_u32 virq = (br_u32)(d - &s_desc[0]);
    if (virq >= BR_IRQ_MAX) {
        return;
    }
    br_irq_rt_t *rt = &s_rt[virq];

    /* Window time base: br_irq_enter does the single saturating isr_seq +1 at entry (P-IRQ-8); this
     * function only samples. epoch is u16 while isr_seq is u32, so compare in 16-bit modular
     * arithmetic (P-IRQ-9): a plain 32-bit compare would reset storm forever once isr_seq passes
     * ~65792. */
    const br_u16 now = sat_u16(cpu->isr_seq);
    if ((br_u16)(now - rt->epoch) > (br_u16)BR_IRQ_STORM_WINDOW) {
        rt->epoch = now;                 /* window expired: restart counting */
        rt->storm = 1u;
    } else {
        rt->storm++;                     /* the N-th within the window */
    }

    /* Report only on the crossing hit (storm == LIMIT + 1), not on every over-limit one. */
    if (rt->storm == (br_u16)(BR_IRQ_STORM_LIMIT + 1u)) {
        cpu->storm_total = sat_inc_u32(cpu->storm_total);
        br_trace_emit(BR_TRACE_IRQ_STORM, virq, (br_u64)rt->storm);
#if BR_IRQ_STORM_GUARD
        /* Off by default (IR-9): auto-mask turns a livelock into "this interrupt silently stops
         * working", which is harder to find; by default only count + trace. */
        br_irq_pgm_mask(d);
#endif
    }
}

/* ---- Bottom half (bh): deferral, suppression and rollback of BR_IRQ_F_DISPATCH_BH ----
 * Execution point is after eoi (the line never stays Active, so no system-wide IRQ starvation) and
 * before ERET, and before the irq_exit hook (threads woken in bh are visible to the
 * preemption decision of the same exit). bh runs with IRQs disabled: items must be short and must
 * not block. */

br_bool br_irq_dispatch_is_bh(const br_irq_desc_t *d)
{
    if (d == BR_NULL) {
        return BR_FALSE;
    }
    return ((d->flags & (br_u16)BR_IRQ_F_DISPATCH_BH) != 0u) ? BR_TRUE : BR_FALSE;
}

void br_irq_bh_note_defer(void)
{
    s_cpu.bh_deferred = sat_inc_u32(s_cpu.bh_deferred);
}

void br_irq_bh_note_drop(void)
{
    s_cpu.bh_dropped = sat_inc_u32(s_cpu.bh_dropped);
}

/* Descriptor -> virq; the bounds check only catches a core bug. */
static br_u32 desc_virq(const br_irq_desc_t *d)
{
    const br_u32 v = (br_u32)(d - &s_desc[0]);

    return (v < BR_IRQ_MAX) ? v : BR_IRQ_INVALID;
}

/*
 * bh work item body: run this line's ISR in bh, then unmask the line. The suppression check lives
 * here: for an item queued but not yet started, re-check the mask state -- if disabled, skip the
 * handler (the item is still consumed), count and trace, with no catch-up/replay after enable. This
 * window is usually unobservable now, but is the only correct behavior when items linger.
 */
static void br_irq_bh_run(void *arg)
{
    br_irq_desc_t *d = (br_irq_desc_t *)arg;

    if (d == BR_NULL || d->isr == BR_NULL) {
        return;
    }

    const br_u32 virq = desc_virq(d);

    if (d->depth > 0u) {
        /* Suppressed: no handler call and no unmask; disable means "stay masked". */
        s_cpu.suppressed_dispatch = sat_inc_u32(s_cpu.suppressed_dispatch);
        br_trace_emit(BR_TRACE_IRQ_DISPATCH_SUPPRESSED, virq, 0u);
        return;
    }

    d->isr(d->arg);
    br_irq_pgm_unmask(d);          /* bh window ended: unmask (a level line re-asserts) */
}

/*
 * Dispatch decision at the ISR slot: BH masks the same line and submits a work item. Without the
 * mask a level line re-asserts before bh runs and floods the static queue from the same line;
 * masking is MMIO, so it is safe in an ISR.
 */
static void br_irq_dispatch_defer(br_irq_desc_t *d, br_u32 virq)
{
    br_irq_pgm_mask(d);

    if (br_irq_bh_submit(br_irq_bh_run, d) != 0) {
        /* IR-10: a failed submit must be rolled back. The line was just masked, so without the
         * rollback it silently stops working forever. Unmask only if still logically unmasked; a
         * level line re-asserts immediately, an edge line loses this one (observable loss over
         * silent deadlock). */
        if (d->depth == 0u) {
            br_irq_pgm_unmask(d);
        }
        br_irq_bh_note_drop();
        br_trace_emit(BR_TRACE_IRQ_BH_DROP, virq, 0u);
        return;
    }

    br_irq_bh_note_defer();
    br_trace_emit(BR_TRACE_IRQ_BH_DEFER, virq, 0u);
}

/* ---- IRQ entry: ack -> dispatch -> ISR -> single-exit eoi; called by the asm stub ---- */

void br_irq_enter(br_exc_frame_t *frame)
{
    /* The exception frame is not used to switch stacks: the ISR returns to the interrupted context.
     * The IRQ exit does switch stacks inside the irq_exit hook, but that is a C call
     * boundary, so the preempted context stays complete on this frame and vectors.S saving
     * caller-saved only still holds. */
    (void)frame;

    br_irq_cpu_t *cpu = &s_cpu;

    /* Window time base: saturating +1 per IRQ entry, the only increment point (P-IRQ-8). */
    cpu->isr_seq = sat_inc_u32(cpu->isr_seq);

    const br_u32 pic_id = br_irq_cpu_pic_id();
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);

    if (pic == BR_NULL || ops == BR_NULL || ops->ack == BR_NULL) {
        /* No CPU-interface PIC: nothing claimed, so never eoi. */
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_NOCAP, BR_PIC_NO_IRQ, 0u);
        return;
    }

    const br_u32 hwirq = ops->ack(pic);      /* Pending -> Active (one IAR read) */
    if (hwirq == BR_PIC_NO_IRQ) {
        /* Not claimed => never eoi; a stray write breaks other lines' state machine. */
        cpu->spurious_nocap = sat_inc_u32(cpu->spurious_nocap);
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_NOCAP, hwirq, 0u);
        return;
    }

    br_u32 virq = 0u;
    br_irq_desc_t *d = BR_NULL;
    br_u32 served_virq = BR_IRQ_NO_HANDLER;

    if (br_irq_virq_of(pic_id, hwirq, &virq) != BR_OK) {
        /* Claimed but unbound: skip the ISR, still must eoi. */
        cpu->spurious_owned = sat_inc_u32(cpu->spurious_owned);
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_OWNED, hwirq, 0u);
        goto EOI;
    }

    d = br_irq_desc_of(virq);
    if (d == BR_NULL || d->isr == BR_NULL) {
        cpu->spurious_owned = sat_inc_u32(cpu->spurious_owned);
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_OWNED, hwirq, 0u);
        goto EOI;
    }

    /* The only place the software shadow may influence "whether to call the ISR": CAP_MASK == 0 and
     * the line is software-masked (depth > 0). With hardware masking, "masked yet still arriving"
     * is the spurious_owned bug signal. This branch must eoi, or the line stays Active forever. */
    if (((caps == BR_NULL) || ((caps->caps & BR_PIC_CAP_MASK) == 0u)) && d->depth > 0u) {
        cpu->spurious_nocap = sat_inc_u32(cpu->spurious_nocap);
        br_trace_emit(BR_TRACE_IRQ_SUPPRESSED, virq, 0u);
        goto EOI;
    }

#if BR_IRQ_STATS
    {
        br_irq_rt_t *rt = br_irq_rt_of(virq);
        if (rt != BR_NULL) {
            rt->count = sat_inc_u32(rt->count);      /* cumulative counter saturates */
        }
    }
#endif

    cpu->curr_virq = (br_u16)virq;
    cpu->isr_nest++;
    served_virq = virq;
    br_trace_emit(BR_TRACE_IRQ_ENTER, virq, (br_u64)hwirq);

    /* Dispatch decision: INLINE runs the ISR in place; BH masks the line and submits a work item
     * whose real handler runs in bh after eoi. ISR contract: minimal work, no blocking/malloc/lock
     * held. */
    if (br_irq_dispatch_is_bh(d) != BR_FALSE) {
        br_irq_dispatch_defer(d, virq);
    } else {
        d->isr(d->arg);
    }

    cpu->isr_nest--;
    cpu->curr_virq = BR_IRQ_NO_HANDLER;
    br_irq_storm_check(d);
    br_trace_emit(BR_TRACE_IRQ_EXIT, virq, 0u);

EOI:
    /* The single eoi point: everything claimed reaches here (normal, invalid virq, unregistered,
     * suppressed); "not claimed" returned above, so no path can forget eoi (INV-H). */
    ops->eoi(pic, hwirq);

    /* Exit: INV-C assertion + scheduler seam. */
    if (cpu->irq_depth != 0u) {
        /* INV-C (IR-6): no return from an ISR holding a lock. v1 does not nest, so depth must be 0
         * at exit; a non-zero value means a lock taken in this ISR was never released. */
#ifdef BR_DEBUG
        br_panic_bare("INV-C violated: irq_depth=%u leaked across IRQ exit (virq=%u)",
                      (br_u32)cpu->irq_depth, (br_u32)served_virq);
#else
        /* Without a panic, a leaked L2 lock stays in the per-core irq_depth and later threads run
         * with IRQs disabled, so self-heal (IR-15): force to zero, restore IRQs, trace. */
        const br_u32 leaked = cpu->irq_depth;
        cpu->irq_depth = 0u;
        /* v0.1 restores I only, and frame is unused (ERET restores from SPSR), so raw = 0.
         * P-IRQ-10: once Stage 2 uses the frame, restore the low 4 bits of frame->daif. */
        if (ops->cpu_unmask != BR_NULL) {
            ops->cpu_unmask(pic, 0u);
        }
        /* No dedicated INV-C event, so borrow the underflow item; b records which line's ISR
         * leaked. */
        br_trace_emit(BR_TRACE_IRQ_UNLOCK_UNDERFLOW, leaked, (br_u64)served_virq);
#endif
    }

    /* Drain bh work items after eoi (line not Active), before ERET (no worker/scheduler dependency)
     * and before the irq_exit hook. The BH budget cap bounds IRQ latency; leftovers
     * wait for the next IRQ exit (tick = 1/HZ). */
    if (s_hooks != BR_NULL && s_hooks->bh_drain != BR_NULL) {
        s_hooks->bh_drain(BR_IRQ_BH_BUDGET);
    }

    if (s_hooks != BR_NULL && s_hooks->irq_exit != BR_NULL) {
        s_hooks->irq_exit();
    }
}

/* ---- Cross-module observation (consumers: debug bridge / ramdump) ---- */

void br_irq_stats_get(br_u32 virq, br_irq_stat_t *out)
{
    if (out == BR_NULL) {
        return;
    }

    /* Zero first: unavailable virq (out of range/unbound) returns all zero. */
    out->count          = 0u;
    out->storm          = 0u;
    out->depth          = 0u;
    out->spurious_owned = 0u;
    out->spurious_nocap = 0u;
    out->drop           = 0u;
    out->flags          = 0u;
    out->prio           = 0u;
    out->trigger        = 0u;
    out->bh_deferred        = 0u;
    out->bh_dropped         = 0u;
    out->suppressed_dispatch = 0u;

    br_irq_desc_t *d = br_irq_desc_of(virq);
    if (d == BR_NULL || d->pic_id < 0) {
        return;                         /* out of range or unbound => all zero */
    }

    br_irq_rt_t *rt = br_irq_rt_of(virq);

    out->count = (BR_IRQ_STATS != 0 && rt != BR_NULL) ? rt->count : 0u;
    out->storm = (rt != BR_NULL) ? rt->storm : 0u;
    out->depth = d->depth;

    /* Spurious counters are CPU-local; v1 is single-core => effectively global. */
    out->spurious_owned = s_cpu.spurious_owned;
    out->spurious_nocap = s_cpu.spurious_nocap;

    /* bh counters are likewise CPU-local cumulative values, not per-line: any bound virq returns
     * the same snapshot, so bh evidence is readable repeatedly without the trace ring. */
    out->bh_deferred         = s_cpu.bh_deferred;
    out->bh_dropped          = s_cpu.bh_dropped;
    out->suppressed_dispatch = s_cpu.suppressed_dispatch;

    /* drop is meaningful only for SLOW domain members; direct lines are always 0. */
    out->drop = br_irq_domain_drop_of(d);

    out->flags   = d->flags;
    out->prio    = d->prio;
    out->trigger = d->trigger;
}
