/*
 * brickOS prototype v0.1.0 — cascaded IRQ domains: domain pool / window slicing / FAST demux / parent ISR
 *
 * Authoritative design: `docs/3-os-core/3-02-int.md` §9
 *   §9.1  Model and bounds: one physical line carries N sub-sources; v1 **single-level cascade**; shared line = non-goal
 *   §9.2  Domain pool + sub-descriptor window + bitmap all come from core static pools (not the heap);
 *         child registration is **two-phase** (create domain -> register_child); with no owner, demux masks
 *   §9.3  FAST demux pseudocode: poll until empty + **explicit exhausted flag** (not round == MAX_ROUND,
 *         else "last round read empty" falsely reports overflow); core acks **after the sub handler returns**
 *   §9.4  SLOW domains: reading state needs bus transactions, so the parent ISR only "masks the whole line + single-flight + submits bh";
 *         demux runs in the **bottom half**, then clears busy + releases the parent line; **`br_work_submit` failure is
 *         this scheme's only deadlock entry and must be rolled back** (IR-10).
 *         ★ ADR-0011: bh now exists (core workqueue) ⇒ SLOW domains are **usable** (previously returned NULL).
 *   §9.6  Domain events: IRQ_DEMUX / IRQ_DEMUX_OVERFLOW / IRQ_DOMAIN_ORPHAN / IRQ_DOMAIN_DROP /
 *         IRQ_DOMAIN_NOBH (the latter now appears only when bh is unavailable)
 *
 * Context (INV-F): FAST domain demux and sub handlers all run in the ISR (pure register access);
 * SLOW domain demux and sub handlers all run in **bh** (ADR-0011) — the parent line's status registers
 * can only be read there, which is exactly why SLOW domains exist.
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_trace.h>
#include <br/core/br_work.h>

#include "irq_internal.h"

/*
 * §9.2: domain pool + one pending bitmap word per domain.
 * v1 supports only nwords == 1 (window <= 32), so "one uint32 per domain" is enough — the minimal
 * static pool that the `uint32_t *bitmap` pointer can point into. The _Static_assert pins the window
 * cap; widening past 32 requires replacing this with a per-domain word slice array.
 */
_Static_assert(BR_IRQ_DOMAIN_WINDOW <= 32u,
               "v0.1 domain bitmap supports a single word only (nwords == 1); widening the window needs the static pool shape changed too");

static struct br_irq_domain s_dom[BR_IRQ_DOMAIN_MAX];
static br_u32               s_dom_bitmap[BR_IRQ_DOMAIN_MAX];
static br_u32               s_dom_n;
static br_u32               s_dom_virq_next;   /* cursor of the domain window slice (§3.2: contiguous slice per domain) */

/* Shared trampoline for the parent-line ISR (§9.3/§9.4): arg = domain pointer. static — used only
 * in this TU via function pointer; not even declared in irq_internal.h (more hidden than hidden). */
static void br_irq_domain_parent_isr(void *arg);

/* SLOW domain demux work item body (arg = domain pointer): runs in **bh** context (ADR-0011). */
static void br_irq_domain_bh_demux(void *arg);

/* Sub-descriptor window: slice [virq_base, virq_base + n_sub) from the core descriptor pool (§3.2/§9.2) */
static br_irq_desc_t *domain_sub_desc(const struct br_irq_domain *dom, br_u32 sub)
{
    return &dom->sub_desc[sub];
}

/* =====================================================================
 * Creation (§9.2; EARLY/CORE phase, thread context)
 * ===================================================================== */

br_irq_domain_t *br_irq_domain_create(const char *name, br_u32 parent_irq,
                                      br_u32 n_sub, br_u32 flags,
                                      const br_irq_domain_ops_t *ops, void *priv)
{
    /* ops->pending is the domain dialect's minimal contract: without it demux cannot know who raised (§9.2) */
    if (name == BR_NULL || ops == BR_NULL || ops->pending == BR_NULL) {
        return BR_NULL;
    }

    /* flags may only be FAST(0) or SLOW(1); other bits are reserved (append-only) */
    if ((flags & ~(br_u32)BR_IRQ_DOMAIN_F_SLOW) != 0u) {
        return BR_NULL;
    }

    /* ★ ADR-0011: SLOW domains require bh (bottom half) — the core workqueue always exists
     *   (`BR_WORKQ_DEPTH > 0`), so this family is **usable**.
     *   The guard below ("bh compiled out => reject + trace") is kept as a **compile-time** branch:
     *   it is the executable form of the staging boundary — if the workqueue is ever trimmed to an
     *   optional piece (depth 0), this is the correct degradation point (not a silent
     *   "accept but never demux" failure, §9.4/§1.1.1). */
#if BR_WORKQ_DEPTH == 0
    if ((flags & (br_u32)BR_IRQ_DOMAIN_F_SLOW) != 0u) {
        br_trace_emit(BR_TRACE_IRQ_DOMAIN_NOBH, flags, 0u);
        return BR_NULL;
    }
#endif

    if (s_dom_n >= (br_u32)BR_IRQ_DOMAIN_MAX) {
        return BR_NULL;                     /* §9.2: pool full => NULL (the signature can only express errors this way) */
    }
    if (n_sub == 0u || n_sub > (br_u32)BR_IRQ_DOMAIN_WINDOW) {
        return BR_NULL;
    }
    if (s_dom_virq_next + n_sub > (br_u32)BR_IRQ_DOMAIN_WINDOW) {
        return BR_NULL;                     /* the sub-interrupt window cannot supply a slice this long */
    }

    /* ---- Parent-line validation (§9.2/§9.4): must be a PIC **direct line**, bound and unoccupied ---- */
    br_irq_desc_t *parent = br_irq_desc_of(parent_irq);
    if (parent == BR_NULL) {
        return BR_NULL;
    }
    /* Nested domains (parent line itself a domain member) are a v1 non-goal (§9.5): parent mask/unmask
     * would no longer be MMIO, invalidating the mask precondition of §9.4. */
    if (parent->dom_id >= 0) {
        return BR_NULL;
    }
    if (parent->pic_id < 0) {
        return BR_NULL;                     /* unbound: init order violation (§14.3), fail loudly */
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
    /* (n_sub+31)/32; _Static_assert guarantees BR_IRQ_DOMAIN_WINDOW <= 32 => always 1, matching the
     * "one uint32 per domain" static bitmap pool. Both must change together if the window is widened. */
    dom->nwords     = (br_u16)((n_sub + 31u) / 32u);
    dom->busy       = 0u;
    dom->drop_count = 0u;
    dom->bitmap     = &s_dom_bitmap[dom_id];
    dom->bitmap[0]  = 0u;
    /* The descriptor pool is a contiguous array (§3.3), so window slicing = take the start pointer;
     * never reach into s_desc across TUs */
    dom->sub_desc   = br_irq_desc_of(virq_base);
    if (dom->sub_desc == BR_NULL) {
        return BR_NULL;
    }

    /* ---- Fill the sub-descriptor window (identity fields; isr/arg stay empty for register_child) ---- */
    for (br_u32 sub = 0u; sub < n_sub; sub++) {
        br_irq_desc_t *sd = domain_sub_desc(dom, sub);
        sd->isr     = BR_NULL;              /* two-phase: no owner yet (§9.2) */
        sd->arg     = BR_NULL;
        sd->hwirq   = sub;                  /* sub index within the domain */
        sd->pic_id  = parent->pic_id;       /* domain member: remember the parent PIC (for diagnostics/stats) */
        sd->dom_id  = (br_s8)dom_id;
        sd->sub     = (br_u16)sub;
        sd->flags   = 0u;
        sd->depth   = 0u;
        sd->prio    = 0u;
        sd->trigger = 0u;
    }

    /*
     * ---- Register the parent line: ISR = domain demux trampoline (§9.2/§9.3) ----
     * Go through br_irq_register() instead of hand-rolling: the safe default
     * "depth=1 after register + hardware already masked" (§6.2) then has one implementation.
     * attr = NULL => use the binding table's static prio/trigger.
     * Failure (e.g. the PIC rejects prio/trigger) => do not commit the domain, return NULL.
     */
    {
        const int r = br_irq_register(parent_irq, br_irq_domain_parent_isr, dom, BR_NULL);
        if (r != BR_OK) {
            return BR_NULL;
        }
    }

    /* Commit: only here does the domain claim its id and window slice */
    s_dom_n++;
    s_dom_virq_next += n_sub;

    return dom;
}

/* =====================================================================
 * Domain demux main loop (§9.3; FAST = ISR context / SLOW = bh context, ADR-0011)
 *
 * The loop itself is **context-independent** — which is exactly where the contract "SLOW domain
 * ops->pending() must read status registers outside ISR context" lands: what changes is not the loop
 * but the **caller**.
 * ===================================================================== */

void br_irq_demux(br_irq_domain_t *dom)
{
    if (dom == BR_NULL || dom->ops == BR_NULL || dom->ops->pending == BR_NULL) {
        return;
    }

    /*
     * ★ The test uses an **explicit exhausted flag**, not `round == BR_IRQ_DEMUX_MAX_ROUND`:
     *   if the last round happens to read an empty bitmap, the loop increments round before exiting,
     *   leaving round == MAX_ROUND with an empty bitmap => false overflow (the §9.3 pseudocode pitfall).
     *   Semantics: exhausted starts at 1 = "assume not finished"; only an empty read clears it.
     */
    br_bool exhausted = BR_TRUE;

    for (br_u32 round = 0u; round < (br_u32)BR_IRQ_DEMUX_MAX_ROUND; round++) {
        /* The status register is a snapshot: new sub-interrupts may set bits while handling => re-read each round (§9.3) */
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
                sd->isr(sd->arg);           /* sub handler contract (FAST=ISR, INV-F) */
            } else {
                /* two-phase gap (§9.2): no owner => mask at once, leaving no re-trigger storm hole */
                if (dom->ops->mask != BR_NULL) {
                    dom->ops->mask(dom->priv, sub);
                }
                br_trace_emit(BR_TRACE_IRQ_DOMAIN_ORPHAN, (br_u32)dom->id, (br_u64)sub);
            }

            /* §9.3: core acks **after the sub handler returns** (isomorphic to the parent line's
             * "eoi only at the single exit": central management => drivers cannot forget to ack) */
            if (dom->ops->ack != BR_NULL) {
                dom->ops->ack(dom->priv, sub);
            }
        }
    }

    /* Bitmap summary: clean exit == 0; rounds exhausted with bits still set => non-zero (direct overflow evidence) */
    br_trace_emit(BR_TRACE_IRQ_DEMUX, (br_u32)dom->id, (br_u64)dom->bitmap[0]);
    if (exhausted == BR_TRUE) {
        /* Domain-level storm protection: cap = BR_IRQ_DEMUX_MAX_ROUND; remaining pending re-triggers
         * after the parent line eoi — level-type status registers naturally lose nothing (§9.3). */
        br_trace_emit(BR_TRACE_IRQ_DEMUX_OVERFLOW, (br_u32)dom->id, 0u);
    }
}

/* =====================================================================
 * Parent-line ISR trampoline (§9.3 FAST direct / §9.4 SLOW to bh) + SLOW's bh work item body
 * ===================================================================== */

/*
 * SLOW domain bh work item body (ADR-0011): read the status bitmap in the **bottom half** => per-sub
 * handler => ack, then clear single-flight + release the parent line.
 *
 * Order is correctness:
 *   1. Must busy be cleared **before releasing the parent line**? — No. The parent line is masked during
 *      bh, so re-entry is impossible; after release a level line may immediately re-assert and enter the
 *      parent ISR — by then busy must already be 0, else the new round is blocked by single-flight while
 *      the line is released, so the event waits for the next assert.
 *      => This implementation: **clear busy first, then release** (same order as the §9.4 pseudocode).
 *   2. demux itself follows the §9.3 poll + exhausted semantics (until empty or rounds exhausted).
 */
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
        /*
         * ---- §9.4: the parent ISR does only three things: single-flight, mask the whole line, submit demux ----
         * Why single-flight: although the parent line is masked immediately, the occurrence already pending
         * before the mask may still come in (level-type + edge-sensitive controllers); without it, two demux
         * runs would be queued for the same state.
         * Why mask must come **before** submit: otherwise the line re-asserts before bh reads the status
         * register and the queue is flooded by the same domain (depth is a static budget).
         */
        if (dom->busy != 0u) {
            return;                     /* one demux already queued/running */
        }
        dom->busy = 1u;
        br_irq_pgm_mask(br_irq_desc_of((br_u32)dom->parent_virq));

        if (br_work_submit(br_irq_domain_bh_demux, dom) != 0) {
            /*
             * ★ IR-10: **this scheme's only deadlock entry**. The parent line is already masked; without
             *   an immediate rollback every sub-interrupt of this domain goes permanently and silently dead
             *   with no error at all.
             *   Rollback = clear busy + release parent line + count + trace (§9.6 IRQ_DOMAIN_DROP).
             *   A level-type status register re-asserts immediately after release => no event lost;
             *   edge/read-clear types lose this one (the honest clause of §6.3).
             */
            dom->busy = 0u;
            dom->drop_count++;
            br_irq_pgm_unmask(br_irq_desc_of((br_u32)dom->parent_virq));
            br_irq_bh_note_drop();
            br_trace_emit(BR_TRACE_IRQ_DOMAIN_DROP, (br_u32)dom->id, 0u);
            return;
        }

        /* Deferral counting and per-line BH dispatch **share one source of truth** (see br_irq_bh_note_defer). */
        br_irq_bh_note_defer();
        br_trace_emit(BR_TRACE_IRQ_BH_DEFER, (br_u32)dom->parent_virq, (br_u64)dom->id);
        return;
    }

    br_irq_demux(dom);
}

/* drop_count of the domain owning a SLOW domain member (read-only accessor for br_irq_stats_get; §9.4). */
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

/* =====================================================================
 * Child-interrupt API (§14.1; the domain side, **mutually exclusive** with the direct family; all thread-only)
 * ===================================================================== */

/* Common precondition of the child API: thread-only (rejects ISR and bh) + dom/sub bounds + owner present */
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
    /* Domain members have no binding-table static prio/trigger (§3.2: sub-interrupts are not physical
     * lines) => the default is "unspecified", left to the domain dialect (domain_ops) to encode. */
    br_u8 prio = (br_u8)BR_IRQ_PRIO_DEFAULT;
    br_u8 trigger = (br_u8)BR_IRQ_TRIG_DEFAULT;

    if (attr != BR_NULL) {
        flags = attr->flags;

        if ((flags & ~(br_u32)BR_IRQ_F_KNOWN_MASK) != 0u) {
            return BR_ERR(EINVAL);       /* unknown bits => reject (§3.4) */
        }
        if ((flags & BR_IRQ_F_SHARED) != 0u) {
            return BR_ERR(ENOTSUP);      /* shared line = v1 non-goal */
        }
        /*
         * PERCPU has no source for domain members: that bit's authority is the binding table (§3.4), and
         * sub-interrupts have no binding entry. => reject explicitly (never silently ignore a requested
         * attribute — exactly what §3.4 forbids). Recorded as P-IRQ-12 (implementation ruling).
         */
        if ((flags & BR_IRQ_F_PERCPU) != 0u) {
            return BR_ERR(EINVAL);
        }
        /* Per-sub dispatch form: **context is decided by the domain type** (FAST => ISR / SLOW => bh,
         * ADR-0011); `DISPATCH_BH/_THREAD` on a sub-interrupt has no extra semantics => ignore + trace
         * when set, cleared on store (never silent: silence would turn "I did ask for bh" into an
         * invisible lie). */
        if ((flags & (BR_IRQ_F_DISPATCH_BH | BR_IRQ_F_DISPATCH_THREAD)) != 0u) {
            br_trace_emit(BR_TRACE_IRQ_DISPATCH_IGNORED, child_virq,
                          (br_u64)(flags & (br_u32)(BR_IRQ_F_DISPATCH_BH |
                                                    BR_IRQ_F_DISPATCH_THREAD)));
            flags &= ~(br_u32)(BR_IRQ_F_DISPATCH_BH | BR_IRQ_F_DISPATCH_THREAD);
        }
        if ((flags & BR_IRQ_F_STATS) != 0u && (BR_IRQ_STATS == 0)) {
            return BR_ERR(ENOTSUP);
        }

        /* Domain members skip the PIC caps check for prio/trigger: per-sub configuration lives entirely
         * on the domain_ops (mask/unmask/ack) side (§9.2); core only records logical values. */
        if (attr->prio != (br_u8)BR_IRQ_PRIO_DEFAULT) {
            prio = attr->prio;
        }
        if (attr->trigger != (br_u8)BR_IRQ_TRIG_DEFAULT) {
            trigger = attr->trigger;
        }
    }

    /*
     * Legal encoding range (§8.1).
     * ★ One necessary difference from direct lines: sub-interrupts have **no binding-table entry** (§3.2),
     *   so with attr unspecified the trigger stays at `BR_IRQ_TRIG_DEFAULT` — meaning "decided by the domain
     *   dialect" (per-sub config lives entirely on the domain_ops side, core only records logical values),
     *   not an illegal value. => range-check only concrete encodings given explicitly. Recorded as P-IRQ-13.
     */
    if (trigger != (br_u8)BR_IRQ_TRIG_DEFAULT && trigger > (br_u8)BR_IRQ_TRIG_EDGE_BOTH) {
        return BR_ERR(EINVAL);
    }

    /* Symmetric with the direct register path (§6.2): register = depth 1 + hardware mask.
     * The parent line is itself masked before enable; still write depth/mask under the L2 critical
     * section to stay atomicity-consistent with enable_child/disable_child. */
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

    /* depth semantics identical to the direct family (§6.2): only 1->0 really unmasks */
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
