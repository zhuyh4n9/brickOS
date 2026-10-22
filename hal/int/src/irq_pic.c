/* PIC registry, binding table and hwirq -> virq index; names no controller model. Descriptors live
 * in irq_core.c and are reached via br_irq_desc_of(). The binding table is platform .rodata,
 * submitted exactly once in the EARLY phase with interrupts globally disabled. */
#include <br/core/br_error.h>
#include <br/hal/br_irq.h>
#include <br/hal/br_pic.h>
#include <br/core/br_trace.h>

#include "irq_internal.h"

/* ---- PIC instance pool: core holds only pointers, storage belongs to platform ---- */

static struct {
    br_pic_t           *pic;
    const br_pic_ops_t *ops;
    br_pic_caps_t       caps;
} s_pics[BR_PIC_MAX];

static br_u32  s_pic_n;
/* IR-3 corollary: ack() takes no argument => exactly one CPU-interface controller per CPU. The
 * first successfully registered PIC becomes it and never changes. */
static br_u32  s_cpu_pic = BR_IRQ_INVALID;
static br_bool s_bindings_set;

/* hwirq -> virq sorted index, key = (pic_id, hwirq): pic_id is in the key because multiple
 * instances coexist. P-IRQ-4: entries are 8 B (2+1+1+4), not the 4 B minimal form, trading size for
 * a single table that avoids per-PIC static pool management. */
static struct {
    br_u16 virq;     /* virq written out on hit */
    br_u8  pic_id;   /* composite key part 1 */
    br_u8  pad;      /* explicit alignment */
    br_u32 hwirq;    /* composite key part 2 */
} s_index[BR_IRQ_MAX];

static br_u32 s_index_n;

/* ---- Helpers (no libc) ---- */

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

/* Insert in ascending (pic_id, hwirq) order; n is tiny, insertion sort suffices. */
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

/* ---- Pool accessors; out of range or unregistered gives NULL ---- */

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

/* ---- Registration (EARLY phase, interrupts globally disabled) ---- */

int br_pic_register(br_pic_t *pic, const br_pic_ops_t *ops)
{
    /* Required ops slots: a missing one means an incomplete dialect. */
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

    /* Duplicate registration: same instance pointer or same dialect name. */
    for (br_u32 i = 0; i < s_pic_n; i++) {
        if (s_pics[i].pic == pic || str_eq(s_pics[i].ops->name, ops->name) == BR_TRUE) {
            return BR_ERR(EBUSY);
        }
    }

    /* init/caps_get may fail: an EARLY-phase failure is a boot failure; propagate the errno. */
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
    /* nr_irq must be >= 1, else the hwirq < caps.nr_irq check in bindings_set is unsatisfiable. */
    if (caps.nr_irq == 0u) {
        return BR_ERR(EINVAL);
    }

    /* mask/unmask are required only when CAP_MASK may be set; without hardware masking these slots
     * are meaningless. Must be checked after caps_get. */
    if ((caps.caps & BR_PIC_CAP_MASK) != 0u &&
        (ops->mask == BR_NULL || ops->unmask == BR_NULL)) {
        return BR_ERR(EINVAL);
    }

    const br_u32 pic_id = s_pic_n;
    s_pics[pic_id].pic  = pic;
    s_pics[pic_id].ops  = ops;
    s_pics[pic_id].caps = caps;
    s_pic_n++;

    /* First registered PIC = CPU interface; unchanged after. */
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

/* ---- Binding table (submitted exactly once, EARLY phase) ---- */

int br_irq_bindings_set(const br_irq_binding_t *tbl, br_size_t n)
{
    if (s_bindings_set == BR_TRUE) {
        return BR_ERR(EBUSY);                    /* exactly once */
    }
    if (tbl == BR_NULL || n == 0u || n > (br_size_t)BR_IRQ_MAX) {
        return BR_ERR(EINVAL);
    }

    const br_u32 cnt = (br_u32)n;

    /* Pass 1: full validation; no state is written unless all entries pass. */
    for (br_u32 i = 0u; i < cnt; i++) {
        const br_irq_binding_t *b = &tbl[i];

        /* Public virq is br_u32, internal is br_u16 => validate the legal range. */
        if ((br_u32)b->virq >= BR_IRQ_MAX) {
            return BR_ERR(EINVAL);
        }
        /* P-IRQ-1: the binding table covers direct lines only. Domain-member descriptors are filled
         * by br_irq_domain_create(), so a binding entry with dom_id >= 0 is schema misuse. */
        if (b->dom_id != (br_s8)-1) {
            return BR_ERR(EINVAL);
        }
        /* Init-order violations must fail loudly: an unregistered pic_id can never resolve. */
        if ((br_u32)b->pic_id >= s_pic_n) {
            return BR_ERR(ENODEV);
        }
        if (b->hwirq >= s_pics[b->pic_id].caps.nr_irq) {
            return BR_ERR(EINVAL);
        }
        /* Duplicate virq (INV-E) or duplicate (pic_id, hwirq) (INV-A): one owner per physical line.
         */
        for (br_u32 j = 0u; j < i; j++) {
            if (tbl[j].virq == b->virq) {
                return BR_ERR(EINVAL);
            }
            if (tbl[j].pic_id == b->pic_id && tbl[j].hwirq == b->hwirq) {
                return BR_ERR(EINVAL);
            }
        }
    }

    /* Pass 2: commit (fill descriptors, default-mask hardware, build the sorted index). */
    for (br_u32 i = 0u; i < cnt; i++) {
        const br_irq_binding_t *b = &tbl[i];
        br_irq_desc_t *d = br_irq_desc_of((br_u32)b->virq);
        if (d == BR_NULL) {
            return BR_ERR(EINVAL);               /* already excluded by pass 1; pure defense */
        }

        /* Identity and binding cache: isr/arg stay NULL, depth/sub 0, dom_id -1 (direct, set by
         * br_irq_cpu_init, P-IRQ-5). PERCPU is authoritative from the binding table and is folded
         * into the runtime flags bit here; the remaining flags stay 0 until br_irq_register
         * cross-checks the attr request. */
        d->hwirq  = b->hwirq;
        d->pic_id = (br_s8)b->pic_id;
        d->prio   = b->prio;
        d->trigger = b->trigger;
        /* Binding entries are direct-only => normalize dom_id to -1 regardless of br_irq_cpu_init
         * order. */
        d->dom_id = (br_s8)-1;
        d->flags  = ((b->flags & (br_u8)BR_IRQB_F_PERCPU) != 0u)
                        ? (br_u16)BR_IRQ_F_PERCPU : (br_u16)0u;

        /* Safe default: mask on bind. Controllers without CAP_MASK are skipped by br_irq_pgm_mask.
         */
        br_irq_pgm_mask(d);

        /* Dispatch form comes from the binding table (platform flash): INLINE = normal hot path; BH
         * folds into the runtime DISPATCH_BH bit and defers that line's ISR to the bottom half
         * (line masked while running); other forms are accepted, ignored and traced. */
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

/* ---- Hot path: hwirq -> virq (read-only, ISR-safe) ---- */

int br_irq_virq_of(br_u32 pic_id, br_u32 hwirq, br_u32 *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /* lower_bound on s_index, sorted by (pic_id, hwirq) */
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

    /* Miss => spurious slot (spurious_owned with hardware masking). */
    return BR_ERR(ENODEV);
}
