/* Cascaded IRQ domains: pool, window slicing, demux, parent-line ISR. v1 is single-level; shared
 * lines are a non-goal. FAST demux and sub handlers run in ISR context (INV-F); SLOW demux and sub
 * handlers run in bh, since the parent status register can only be read there. */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_trace.h>
#include <br/core/br_work.h>

#include "irq_internal.h"

/* Domain pool plus one pending bitmap word per domain: v1 supports only nwords == 1 (window <= 32),
 * the minimal static pool the bitmap pointer can point into. Widening past 32 needs a per-domain
 * word slice array; the _Static_assert below pins the cap. */
_Static_assert(BR_IRQ_DOMAIN_WINDOW <= 32u,
               "v0.1 domain bitmap supports a single word only (nwords == 1); widening the window needs the static pool shape changed too");

static struct br_irq_domain s_dom[BR_IRQ_DOMAIN_MAX];
static br_u32               s_dom_bitmap[BR_IRQ_DOMAIN_MAX];
static br_u32               s_dom_n;
static br_u32               s_dom_virq_next;   /* cursor of the contiguous domain window slice */

/* Parent-line ISR trampoline (arg = domain); file-static, not declared in irq_internal.h. */
static void br_irq_domain_parent_isr(void *arg);

/* SLOW domain demux work item body (arg = domain); runs in bh context. */
static void br_irq_domain_bh_demux(void *arg);

/* Sub-descriptor window slice [virq_base, virq_base + n_sub) from the core pool. */
static br_irq_desc_t *domain_sub_desc(const struct br_irq_domain *dom, br_u32 sub)
{
    return &dom->sub_desc[sub];
}

/* ---- Creation (EARLY/CORE phase, thread context) ---- */

br_irq_domain_t *br_irq_domain_create(const char *name, br_u32 parent_irq,
                                      br_u32 n_sub, br_u32 flags,
                                      const br_irq_domain_ops_t *ops, void *priv)
{
    /* ops->pending is the minimal dialect contract: without it demux cannot know who raised. */
    if (name == BR_NULL || ops == BR_NULL || ops->pending == BR_NULL) {
        return BR_NULL;
    }

    /* Only FAST(0) or SLOW(1); other bits are reserved. */
    if ((flags & ~(br_u32)BR_IRQ_DOMAIN_F_SLOW) != 0u) {
        return BR_NULL;
    }

    /* SLOW domains require bh; BR_WORKQ_DEPTH == 0 is the compile-time degradation point: reject
     * and trace rather than accept a domain that would never be demuxed. */
#if BR_WORKQ_DEPTH == 0
    if ((flags & (br_u32)BR_IRQ_DOMAIN_F_SLOW) != 0u) {
        br_trace_emit(BR_TRACE_IRQ_DOMAIN_NOBH, flags, 0u);
        return BR_NULL;
    }
#endif

    if (s_dom_n >= (br_u32)BR_IRQ_DOMAIN_MAX) {
        return BR_NULL;                     /* Pool full => NULL (the signature has no errno) */
    }
    if (n_sub == 0u || n_sub > (br_u32)BR_IRQ_DOMAIN_WINDOW) {
        return BR_NULL;
    }
    if (s_dom_virq_next + n_sub > (br_u32)BR_IRQ_DOMAIN_WINDOW) {
        return BR_NULL;                     /* the sub-interrupt window cannot supply a slice this long */
    }

    /* Parent line must be a bound, unoccupied PIC direct line. */
    br_irq_desc_t *parent = br_irq_desc_of(parent_irq);
    if (parent == BR_NULL) {
        return BR_NULL;
    }
    /* Nested domains are a v1 non-goal: parent mask/unmask would no longer be MMIO. */
    if (parent->dom_id >= 0) {
        return BR_NULL;
    }
    if (parent->pic_id < 0) {
        return BR_NULL;                     /* unbound: init-order violation, fail loudly */
    }
    if (parent->isr != BR_NULL) {
        return BR_NULL;                     /* INV-A: parent line has exactly one owner (already taken by another driver) */
    }

    const br_u32 dom_id = s_dom_n;
    const br_u32 virq_base = (br_u32)BR_IRQ_DOMAIN_BASE + s_dom_virq_next;

    struct br_irq_domain *dom = &s_dom[dom_id];
    dom->name       = name;
    dom->ops        = ops;
    dom->priv       = priv;
    dom->parent_virq = (br_u16)parent_irq;
    dom->virq_base  = (br_u16)virq_base;
    dom->n_sub      = (br_u16)n_sub;
    dom->flags      = (br_u16)flags;
    dom->id         = (br_u8)dom_id;
    /* (n_sub+31)/32; the _Static_assert guarantees 1, matching the one-word-per-domain bitmap pool.
     */
    dom->nwords     = (br_u16)((n_sub + 31u) / 32u);
    dom->busy       = 0u;
    dom->drop_count = 0u;
    dom->bitmap     = &s_dom_bitmap[dom_id];
    dom->bitmap[0]  = 0u;
    /* The descriptor pool is contiguous, so slicing = take the start pointer; never reach into
     * s_desc across TUs. */
    dom->sub_desc   = br_irq_desc_of(virq_base);
    if (dom->sub_desc == BR_NULL) {
        return BR_NULL;
    }

    /* Fill the sub-descriptor window; isr/arg stay empty for register_child. */
    for (br_u32 sub = 0u; sub < n_sub; sub++) {
        br_irq_desc_t *sd = domain_sub_desc(dom, sub);
        sd->isr     = BR_NULL;              /* two-phase: no owner yet */
        sd->arg     = BR_NULL;
        sd->hwirq   = sub;                  /* sub index within the domain */
        sd->pic_id  = parent->pic_id;       /* domain member: parent PIC (diagnostics/stats) */
        sd->dom_id  = (br_s8)dom_id;
        sd->sub     = (br_u16)sub;
        sd->flags   = 0u;
        sd->depth   = 0u;
        sd->prio    = 0u;
        sd->trigger = 0u;
    }

    /* Register the parent line with ISR = domain demux trampoline, via br_irq_register so the
     * "depth = 1, hardware masked" default has one implementation. attr = NULL => use the binding
     * table's static prio/trigger; on failure do not commit the domain. */
    {
        const int r = br_irq_register(parent_irq, br_irq_domain_parent_isr, dom, BR_NULL);
        if (r != BR_OK) {
            return BR_NULL;
        }
    }

    s_dom_n++;
    s_dom_virq_next += n_sub;

    return dom;
}

/* Demux main loop; context-independent -- FAST calls it in the parent ISR, SLOW from the bh work
 * item, which is where the "SLOW ops->pending() must run outside ISR context" contract lands. */

void br_irq_demux(br_irq_domain_t *dom)
{
    if (dom == BR_NULL || dom->ops == BR_NULL || dom->ops->pending == BR_NULL) {
        return;
    }

    /* The test uses an explicit exhausted flag, not round == BR_IRQ_DEMUX_MAX_ROUND: an empty last
     * round still increments round, which would report a false overflow. exhausted starts true
     * ("not finished"); only an empty read clears it. */
    br_bool exhausted = BR_TRUE;

    for (br_u32 round = 0u; round < (br_u32)BR_IRQ_DEMUX_MAX_ROUND; round++) {
        /* Status is a snapshot: re-read each round, as new bits may be set while handling. */
        dom->bitmap[0] = 0u;
        dom->ops->pending(dom->priv, dom->bitmap, (br_size_t)dom->nwords);

        if (dom->bitmap[0] == 0u) {
            exhausted = BR_FALSE;
            break;
        }

        const br_u32 bits = dom->bitmap[0];
        for (br_u32 sub = 0u; sub < (br_u32)dom->n_sub; sub++) {
            if ((bits & (1u << sub)) == 0u) {
                continue;
            }

            br_irq_desc_t *sd = domain_sub_desc(dom, sub);
            if (sd->isr != BR_NULL) {
                sd->isr(sd->arg);           /* sub handler (FAST = ISR) */
            } else {
                /* two-phase gap: no owner => mask at once to avoid a re-trigger storm */
                if (dom->ops->mask != BR_NULL) {
                    dom->ops->mask(dom->priv, sub);
                }
                br_trace_emit(BR_TRACE_IRQ_DOMAIN_ORPHAN, (br_u32)dom->id, (br_u64)sub);
            }

            /* Core acks after the sub handler returns, mirroring the parent line's single-eoi
             * discipline. */
            if (dom->ops->ack != BR_NULL) {
                dom->ops->ack(dom->priv, sub);
            }
        }
    }

    /* Bitmap summary: 0 on a clean exit; non-zero means rounds were exhausted with bits still set.
     */
    br_trace_emit(BR_TRACE_IRQ_DEMUX, (br_u32)dom->id, (br_u64)dom->bitmap[0]);
    if (exhausted == BR_TRUE) {
        /* Storm protection cap = BR_IRQ_DEMUX_MAX_ROUND; pending re-triggers after the parent eoi.
         */
        br_trace_emit(BR_TRACE_IRQ_DEMUX_OVERFLOW, (br_u32)dom->id, 0u);
    }
}

/* ---- Parent-line ISR trampoline + SLOW bh work item body ---- */

/* SLOW bh body: read the status bitmap in bh, dispatch sub handlers, ack, then clear busy and
 * release the parent line. busy must be cleared before the release: the line is masked during bh,
 * but after release a level line can re-assert and re-enter the parent ISR, which would then be
 * blocked by single-flight while the line is already released. */
static void br_irq_domain_bh_demux(void *arg)
{
    br_irq_domain_t *dom = (br_irq_domain_t *)arg;

    if (dom == BR_NULL) {
        return;
    }

    br_irq_demux(dom);

    dom->busy = 0u;
    br_irq_pgm_unmask(br_irq_desc_of((br_u32)dom->parent_virq));
}

static void br_irq_domain_parent_isr(void *arg)
{
    br_irq_domain_t *dom = (br_irq_domain_t *)arg;
    if (dom == BR_NULL) {
        return;
    }

    if ((dom->flags & (br_u16)BR_IRQ_DOMAIN_F_SLOW) != 0u) {
        /* SLOW parent ISR does three things: single-flight, mask the whole line, submit demux.
         * Single-flight guards against an occurrence already pending before the mask; the mask must
         * precede the submit, or the line re-asserts before bh reads the status register and floods
         * the static queue. */
        if (dom->busy != 0u) {
            return;                     /* one demux already queued/running */
        }
        dom->busy = 1u;
        br_irq_pgm_mask(br_irq_desc_of((br_u32)dom->parent_virq));

        if (br_work_submit(br_irq_domain_bh_demux, dom) != 0) {
            /* IR-10: the only deadlock entry of this scheme. The parent line is already masked, so
             * without an immediate rollback every sub-interrupt of this domain goes permanently
             * dead with no error. Rollback = clear busy, release the line, count and trace. */
            dom->busy = 0u;
            dom->drop_count++;
            br_irq_pgm_unmask(br_irq_desc_of((br_u32)dom->parent_virq));
            br_irq_bh_note_drop();
            br_trace_emit(BR_TRACE_IRQ_DOMAIN_DROP, (br_u32)dom->id, 0u);
            return;
        }

        /* Shares the deferral counter with per-line BH dispatch. */
        br_irq_bh_note_defer();
        br_trace_emit(BR_TRACE_IRQ_BH_DEFER, (br_u32)dom->parent_virq, (br_u64)dom->id);
        return;
    }

    br_irq_demux(dom);
}

/* drop_count of the domain owning a SLOW member (read-only, for br_irq_stats_get). */
br_u32 br_irq_domain_drop_of(const br_irq_desc_t *d)
{
    if (d == BR_NULL || d->dom_id < 0) {
        return 0u;                      /* direct line: no domain, so no domain-level drops */
    }
    const br_u32 id = (br_u32)d->dom_id;
    if (id >= (br_u32)BR_IRQ_DOMAIN_MAX) {
        return 0u;
    }
    return (br_u32)s_dom[id].drop_count;
}

/* ---- Child-interrupt API (thread-only; exclusive with the direct family) ---- */

/* Child API precondition: thread-only, dom/sub in bounds, owner present. */
static int child_api_guard(br_irq_domain_t *dom, br_u32 sub, br_irq_desc_t **out)
{
    if (br_irq_in_atomic() != BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    if (dom == BR_NULL || dom->ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (sub >= (br_u32)dom->n_sub) {
        return BR_ERR(EINVAL);
    }
    br_irq_desc_t *sd = domain_sub_desc(dom, sub);
    if (sd->isr == BR_NULL) {
        return BR_ERR(EINVAL);           /* unregistered sub-interrupt */
    }
    *out = sd;
    return BR_OK;
}

int br_irq_register_child(br_irq_domain_t *dom, br_u32 sub,
                          void (*isr)(void *), void *arg, const br_irq_attr_t *attr)
{
    if (br_irq_in_atomic() != BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    if (dom == BR_NULL || dom->ops == BR_NULL || isr == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (sub >= (br_u32)dom->n_sub) {
        return BR_ERR(EINVAL);
    }

    br_irq_desc_t *sd = domain_sub_desc(dom, sub);
    if (sd->isr != BR_NULL) {
        return BR_ERR(EBUSY);            /* INV-A */
    }

    const br_u32 child_virq = (br_u32)dom->virq_base + sub;

    br_u32 flags = 0u;
    /* Domain members have no binding-table prio/trigger; DEFAULT is left to the domain dialect. */
    br_u8 prio = (br_u8)BR_IRQ_PRIO_DEFAULT;
    br_u8 trigger = (br_u8)BR_IRQ_TRIG_DEFAULT;

    if (attr != BR_NULL) {
        flags = attr->flags;

        if ((flags & ~(br_u32)BR_IRQ_F_KNOWN_MASK) != 0u) {
            return BR_ERR(EINVAL);       /* unknown bits => reject */
        }
        if ((flags & BR_IRQ_F_SHARED) != 0u) {
            return BR_ERR(ENOTSUP);      /* shared line = v1 non-goal */
        }
        /* PERCPU's authority is the binding table, which sub-interrupts have no entry in, so the
         * bit has no source here: reject explicitly rather than silently ignore the request.
         * P-IRQ-12. */
        if ((flags & BR_IRQ_F_PERCPU) != 0u) {
            return BR_ERR(EINVAL);
        }
        /* A sub-interrupt's dispatch form has no extra semantics (context comes from the domain
         * type), so it is cleared and traced when set -- never silently ignored. */
        if ((flags & (BR_IRQ_F_DISPATCH_BH | BR_IRQ_F_DISPATCH_THREAD)) != 0u) {
            br_trace_emit(BR_TRACE_IRQ_DISPATCH_IGNORED, child_virq,
                          (br_u64)(flags & (br_u32)(BR_IRQ_F_DISPATCH_BH |
                                                    BR_IRQ_F_DISPATCH_THREAD)));
            flags &= ~(br_u32)(BR_IRQ_F_DISPATCH_BH | BR_IRQ_F_DISPATCH_THREAD);
        }
        if ((flags & BR_IRQ_F_STATS) != 0u && (BR_IRQ_STATS == 0)) {
            return BR_ERR(ENOTSUP);
        }

        /* Per-sub prio/trigger live on the domain_ops side; core only records the logical values.
         */
        if (attr->prio != (br_u8)BR_IRQ_PRIO_DEFAULT) {
            prio = attr->prio;
        }
        if (attr->trigger != (br_u8)BR_IRQ_TRIG_DEFAULT) {
            trigger = attr->trigger;
        }
    }

    /* Legal encoding range. Sub-interrupts have no binding-table entry, so an unspecified trigger
     * stays at BR_IRQ_TRIG_DEFAULT ("decided by the domain dialect"), not an illegal value => only
     * concrete encodings are range-checked. P-IRQ-13. */
    if (trigger != (br_u8)BR_IRQ_TRIG_DEFAULT && trigger > (br_u8)BR_IRQ_TRIG_EDGE_BOTH) {
        return BR_ERR(EINVAL);
    }

    /* Symmetric with the direct register path: register = depth 1 + hardware mask, under the same
     * critical section as enable_child/disable_child. */
    const br_irq_state_t st = br_irq_lock();
    sd->isr     = isr;
    sd->arg     = arg;
    sd->flags   = (br_u16)flags;
    sd->prio    = prio;
    sd->trigger = trigger;
    sd->depth   = 1u;
    if (dom->ops->mask != BR_NULL) {
        dom->ops->mask(dom->priv, sub);
    }
    br_irq_unlock(st);

    return BR_OK;
}

int br_irq_enable_child(br_irq_domain_t *dom, br_u32 sub)
{
    br_irq_desc_t *sd = BR_NULL;
    const int guard = child_api_guard(dom, sub, &sd);
    if (guard != BR_OK) {
        return guard;
    }

    /* only 1->0 unmasks */
    const br_irq_state_t st = br_irq_lock();
    if (sd->depth == 0u) {
        br_irq_unlock(st);
        return BR_OK;                       /* already enabled: idempotent success */
    }
    sd->depth--;
    if (sd->depth == 0u && dom->ops->unmask != BR_NULL) {
        dom->ops->unmask(dom->priv, sub);
    }
    br_irq_unlock(st);
    return BR_OK;
}

int br_irq_disable_child(br_irq_domain_t *dom, br_u32 sub)
{
    br_irq_desc_t *sd = BR_NULL;
    const int guard = child_api_guard(dom, sub, &sd);
    if (guard != BR_OK) {
        return guard;
    }

    const br_irq_state_t st = br_irq_lock();
    if (sd->depth == 0u && dom->ops->mask != BR_NULL) {
        dom->ops->mask(dom->priv, sub);     /* 0 -> 1: actually taken offline */
    }
    sd->depth++;
    br_irq_unlock(st);
    return BR_OK;
}
