/*
 * brickOS prototype v0.1.0 — PIC registry + binding table + hwirq→virq index
 *
 * Authoritative design: `docs/3-os-core/3-02-int.md`
 *   §4.1 / §4.2  PIC ops table filling and capability bits (IR-3); corollary
 *                "exactly one CPU interface controller per CPU"
 *   §14.3        platform-side contract and **one-way init chain** (1 register PIC →
 *                2 caps → 3 binding table → 4-7 drivers)
 *   §14.2        hot path hwirq→virq: **sorted table + binary search** (4 B/binding),
 *                not a virq_of[nr_irq] direct table
 *   §3.3/§3.4    binding table entries (platform .rodata) and attr.flags semantics
 *   §3.2         width invariants: public signature br_u32, internal fields int8/uint16
 *
 * This file only does "register + resolve table"; it names no controller model (§1.3 rule).
 * The descriptor pool belongs to irq_core.c; here it is accessed via br_irq_desc_of()
 * (irq_internal.h).
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_pic.h>
#include <br/core/br_trace.h>

#include "irq_internal.h"

/* =====================================================================
 * PIC instance pool (§4.1: core holds only pointers; instance storage belongs to platform)
 * ===================================================================== */

static struct {
    br_pic_t           *pic;
    const br_pic_ops_t *ops;
    br_pic_caps_t       caps;
} s_pics[BR_PIC_MAX];

static br_u32  s_pic_n;
/* ★ IR-3 corollary (§4.2): ack() takes no argument ⇒ exactly one "CPU interface
 * controller" per CPU. The first successfully registered PIC becomes it and **never
 * changes** (other instances can only attach via cascade domains). */
static br_u32  s_cpu_pic = BR_IRQ_INVALID;
static br_bool s_bindings_set;

/*
 * hwirq→virq sorted index (§14.2 "sorted table + binary search" form).
 * Sort key = (pic_id, hwirq); pic_id is in the key because multiple instances coexist
 * (§4.2 multi-controller attach rules).
 *
 * P-IRQ-4 (implementation decision): entry = 8 B, not the 4 B written in design §14.2 —
 * because the v0.1 entry packs pic_id and pad together (2+1+1+4 = 8). That 4 B figure
 * describes the minimal "store only (virq, hwirq)" form, which requires "one table per
 * instance" or a sort key without pic_id. v0.1 chooses a single table with a composite
 * (pic_id, hwirq) key: entries double, but at ≤ BR_IRQ_MAX bindings that is at most
 * 512 B and it avoids per-PIC static table pool management.
 */
static struct {
    br_u16 virq;     /* virq written out on hit */
    br_u8  pic_id;   /* ★ composite key part 1 */
    br_u8  pad;      /* explicit alignment (no padding ambiguity) */
    br_u32 hwirq;    /* ★ composite key part 2 */
} s_index[BR_IRQ_MAX];

static br_u32 s_index_n;

/* =====================================================================
 * Helpers (no libc: own string compare)
 * ===================================================================== */

static br_bool str_eq(const char *a, const char *b)
{
    if (a == BR_NULL || b == BR_NULL) {
        return BR_FALSE;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

/* Insert in ascending (pic_id, hwirq) order (n is tiny, insertion sort suffices; §14.2 only says "sorted") */
static void index_insert(br_u32 pic_id, br_u32 hwirq, br_u32 virq)
{
    br_u32 pos = s_index_n;

    while (pos > 0u) {
        const br_u32 pp = (br_u32)s_index[pos - 1u].pic_id;
        const br_u32 ph = s_index[pos - 1u].hwirq;
        if (pp < pic_id || (pp == pic_id && ph <= hwirq)) {
            break;
        }
        s_index[pos] = s_index[pos - 1u];
        pos--;
    }

    s_index[pos].virq   = (br_u16)virq;
    s_index[pos].pic_id = (br_u8)pic_id;
    s_index[pos].pad    = 0u;
    s_index[pos].hwirq  = hwirq;
    s_index_n++;
}

/* =====================================================================
 * Pool accessors (irq_internal.h; out of range/unregistered ⇒ NULL)
 * ===================================================================== */

br_pic_t *br_irq_pic(br_u32 pic_id)
{
    return (pic_id < s_pic_n) ? s_pics[pic_id].pic : BR_NULL;
}

const br_pic_ops_t *br_irq_pic_ops(br_u32 pic_id)
{
    return (pic_id < s_pic_n) ? s_pics[pic_id].ops : BR_NULL;
}

const br_pic_caps_t *br_irq_pic_caps(br_u32 pic_id)
{
    return (pic_id < s_pic_n) ? &s_pics[pic_id].caps : BR_NULL;
}

br_u32 br_irq_pic_count(void)
{
    return s_pic_n;
}

br_u32 br_irq_cpu_pic_id(void)
{
    return s_cpu_pic;
}

/* =====================================================================
 * Registration (§14.3 init chain steps 1-2; EARLY phase, interrupts globally disabled)
 * ===================================================================== */

int br_pic_register(br_pic_t *pic, const br_pic_ops_t *ops)
{
    /* ---- required slots (§4.1 ops table): any missing one means an incomplete dialect, must fail loudly ---- */
    if (pic == BR_NULL || ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (ops->name == BR_NULL || ops->ack == BR_NULL || ops->eoi == BR_NULL ||
        ops->caps_get == BR_NULL || ops->cpu_mask == BR_NULL ||
        ops->cpu_unmask == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    if (s_pic_n >= (br_u32)BR_PIC_MAX) {
        return BR_ERR(ENOSPC);
    }

    /* Duplicate registration: same instance pointer, or same dialect name (§4.1: "once per instance") */
    for (br_u32 i = 0; i < s_pic_n; i++) {
        if (s_pics[i].pic == pic || str_eq(s_pics[i].ops->name, ops->name) == BR_TRUE) {
            return BR_ERR(EBUSY);
        }
    }

    /* init/caps_get may fail ⇒ EARLY-phase failure = boot failure (§14.3 error handling duty):
     * **propagate** the negative errno here, never "silently return 0 while unconfigured". */
    if (ops->init != BR_NULL) {
        const int r = ops->init(pic);
        if (r != BR_OK) {
            return r;
        }
    }

    br_pic_caps_t caps;
    caps.prio_bits = 0u;
    caps.nr_irq    = 0u;
    caps.caps      = 0u;
    {
        const int r = ops->caps_get(pic, &caps);
        if (r != BR_OK) {
            return r;
        }
    }
    /* "Reasonable" nr_irq criterion: at least one line. A 0-line controller would only make
     * br_irq_bindings_set's hwirq < caps.nr_irq check unsatisfiable ⇒ report early. */
    if (caps.nr_irq == 0u) {
        return BR_ERR(EINVAL);
    }

    /* mask/unmask are required only when CAP_MASK may be set (§4.2: no hardware masking
     * ⇒ degrade to pure software shadow masking, where these slots are meaningless).
     * ⇒ This check must come after caps_get. */
    if ((caps.caps & BR_PIC_CAP_MASK) != 0u &&
        (ops->mask == BR_NULL || ops->unmask == BR_NULL)) {
        return BR_ERR(EINVAL);
    }

    const br_u32 pic_id = s_pic_n;
    s_pics[pic_id].pic  = pic;
    s_pics[pic_id].ops  = ops;
    s_pics[pic_id].caps = caps;
    s_pic_n++;

    /* ★ First successfully registered PIC = CPU interface (IR-3 corollary); unchanged after. */
    if (s_cpu_pic == BR_IRQ_INVALID) {
        s_cpu_pic = pic_id;
    }

    return (int)pic_id;
}

int br_pic_caps_of(br_u32 pic_id, br_pic_caps_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (pic_id >= s_pic_n) {
        return BR_ERR(ENODEV);
    }
    *out = s_pics[pic_id].caps;
    return BR_OK;
}

/* =====================================================================
 * Binding table (§3.3; submitted once by platform, exactly once in EARLY phase)
 * ===================================================================== */

int br_irq_bindings_set(const br_irq_binding_t *tbl, br_size_t n)
{
    if (s_bindings_set == BR_TRUE) {
        return BR_ERR(EBUSY);                    /* exactly once (§14.3) */
    }
    if (tbl == BR_NULL || n == 0u || n > (br_size_t)BR_IRQ_MAX) {
        return BR_ERR(EINVAL);
    }

    const br_u32 cnt = (br_u32)n;

    /* ---- pass 1: full validation (write state only if all pass, avoiding a "half-applied table") ---- */
    for (br_u32 i = 0u; i < cnt; i++) {
        const br_irq_binding_t *b = &tbl[i];

        /* Public signature is br_u32, internal virq is br_u16 ⇒ the entry must validate the legal range (§3.2 width invariants) */
        if ((br_u32)b->virq >= BR_IRQ_MAX) {
            return BR_ERR(EINVAL);
        }
        /*
         * P-IRQ-1 (implementation decision): the v0.1 binding table **covers direct lines only**.
         * Domain-member descriptors are filled by br_irq_domain_create() window slicing
         * (§3.2: child interrupts are "not physical lines", semantically domain members)
         * ⇒ a binding entry with dom_id >= 0 is schema misuse and must be rejected.
         */
        if (b->dom_id != (br_s8)-1) {
            return BR_ERR(EINVAL);
        }
        /* §14.3: init-order violations must **fail loudly** (PIC unregistered ⇒ the referenced
         * pic_id is meaningless); never silently accept a binding that can never resolve. */
        if ((br_u32)b->pic_id >= s_pic_n) {
            return BR_ERR(ENODEV);
        }
        if (b->hwirq >= s_pics[b->pic_id].caps.nr_irq) {
            return BR_ERR(EINVAL);
        }
        /* Duplicate: same virq (INV-E/INV-A), or same (pic_id, hwirq) bound twice (INV-A:
         * exactly one owner per physical line) */
        for (br_u32 j = 0u; j < i; j++) {
            if (tbl[j].virq == b->virq) {
                return BR_ERR(EINVAL);
            }
            if (tbl[j].pic_id == b->pic_id && tbl[j].hwirq == b->hwirq) {
                return BR_ERR(EINVAL);
            }
        }
    }

    /* ---- pass 2: commit (fill descriptors + hardware default mask + build sorted index) ---- */
    for (br_u32 i = 0u; i < cnt; i++) {
        const br_irq_binding_t *b = &tbl[i];
        br_irq_desc_t *d = br_irq_desc_of((br_u32)b->virq);
        if (d == BR_NULL) {
            return BR_ERR(EINVAL);               /* already excluded by pass 1; pure defense */
        }

        /* Identity and binding cache (§3.3): isr/arg left NULL (unregistered), depth/sub left 0,
         * dom_id stays -1 (direct; initialized to -1 by br_irq_cpu_init — see P-IRQ-5).
         *
         * ★ flags special case (§3.4): PERCPU is authoritative from the **binding table**
         *   ("usually not filled by hand"). The descriptor has no separate "static binding
         *   attribute" bitfield, so the static PERCPU is folded into the runtime flags bit here;
         *   br_irq_register then cross-checks it against the attr request.
         *   The remaining flags bits stay 0 (awaiting register). */
        d->hwirq  = b->hwirq;
        d->pic_id = (br_s8)b->pic_id;
        d->prio   = b->prio;
        d->trigger = b->trigger;
        /* Binding entries allow direct lines only (P-IRQ-1 check above) ⇒ normalize the internal
         * domain index to -1, so the br_irq_register/desc "direct" check does not depend on
         * whether br_irq_cpu_init ran first. */
        d->dom_id = (br_s8)-1;
        d->flags  = ((b->flags & (br_u8)BR_IRQB_F_PERCPU) != 0u)
                        ? (br_u16)BR_IRQ_F_PERCPU : (br_u16)0u;

        /* Safe default (§14.3 step 7, "lines before step 7 receive no interrupts"): mask on bind.
         * Controllers without CAP_MASK are skipped by br_irq_pgm_mask itself (§4.3 exemption). */
        br_irq_pgm_mask(d);

        /*
         * Dispatch form (§12.2: the default carrier is the **binding table**, platform data in flash).
         *   INLINE ⇒ normal (hot path calls the ISR directly);
         *   BH     ⇒ ★ ADR-0011: folds into the runtime flags DISPATCH_BH bit as the **truth**;
         *            that line's ISR is deferred to the bottom half (hardware line masked while running);
         *   THREAD / others ⇒ not implemented yet (threaded IRQ is Stage 2) ⇒ accept, ignore, trace (§3.4).
         */
        if (b->dispatch == (br_u8)BR_IRQ_DISPATCH_BH) {
            d->flags |= (br_u16)BR_IRQ_F_DISPATCH_BH;
        } else if (b->dispatch != (br_u8)BR_IRQ_DISPATCH_INLINE) {
            br_trace_emit(BR_TRACE_IRQ_DISPATCH_IGNORED,
                          (br_u32)b->virq, (br_u64)b->dispatch);
        }

        index_insert((br_u32)b->pic_id, b->hwirq, (br_u32)b->virq);
    }

    s_bindings_set = BR_TRUE;
    return BR_OK;
}

/* =====================================================================
 * Hot path: hwirq → virq (§5.2 entry stub lookup; read-only, ISR-safe)
 * ===================================================================== */

int br_irq_virq_of(br_u32 pic_id, br_u32 hwirq, br_u32 *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /* Lower-bound binary search (lower_bound) on s_index sorted by (pic_id, hwirq) */
    br_u32 lo = 0u;
    br_u32 hi = s_index_n;
    while (lo < hi) {
        const br_u32 mid = lo + ((hi - lo) >> 1);
        const br_u32 mp  = (br_u32)s_index[mid].pic_id;
        const br_u32 mh  = s_index[mid].hwirq;
        if (mp < pic_id || (mp == pic_id && mh < hwirq)) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }

    if (lo < s_index_n && (br_u32)s_index[lo].pic_id == pic_id &&
        s_index[lo].hwirq == hwirq) {
        *out = (br_u32)s_index[lo].virq;
        return BR_OK;
    }

    /* Miss ⇒ §5.2 spurious slot (with hardware masking ⇒ spurious_owned, a real bug signal) */
    return BR_ERR(ENODEV);
}
