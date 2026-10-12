/*
 * brickOS prototype v0.1.0 — IRQ core: descriptor pool / CPU-local / lifecycle / masking / storm
 *
 * Authoritative design: `docs/3-os-core/3-02-int.md`
 *   §3.3 / §14.4  descriptor pool and runtime counters (two parallel arrays, same virq index)
 *   §5.2 / §5.3   lifecycle: ack → ISR → **single eoi point** ("claimed ⇒ must eoi, not claimed ⇒ never eoi")
 *   §6.1–§6.4     three mask layers; L1 depth nesting; L2 lock/unlock + underflow guard + INV-C exit assertion
 *   §7.1 / IR-7   priority semantics (smaller = higher; saturate toward **low priority** when bits are short + trace)
 *   §8.3          windowed storm criterion (isr_seq time base; frequency-independent, ratio only)
 *   §4.2 / §4.3   capability negotiation and deterministic degradation; CAP_MASK=0 exemption "shadow is the last gate"
 *   §11.2         scheduler seam br_sched_irq_epilogue (Stage 1 = empty implementation)
 *   §14.1         runtime enforcement of thread-only (design side: static scan + conformance)
 *
 * ★ ADR-0011 (this prototype) **lands the first Stage 2 item early: bottom half (bh)**:
 *   §11.1 path one (ISR → deferred work), §12.4 (BH form: mask the same line while handling),
 *   §11.4.1 (semantics of `br_irq_disable` under BH: suppress without cancel, no replay), IR-10 (must roll back on full queue).
 *   Execution point = **IRQ exit (after eoi, before ERET)**, carried by the core workqueue (br_work.h).
 *   Still **not done**: `BR_IRQ_DISPATCH_THREAD` (threaded IRQ) and §12.3 dynamic upgrade — set bit ignored + trace.
 *
 * This file names no controller model (§1.3); all hardware actions go through br_pic_ops (br_pic.h).
 */
#include <br/core/br_error.h>
#include <br/core/br_exc.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_pic.h>
/* `br_sched_irq_epilogue()` is declared and implemented on the scheduler side (br_sched.h / core/src/sched/sched_core.c):
 * this file only **calls** it on the IRQ exit path (the call site is unchanged). */
#include <br/core/br_sched.h>
#include <br/core/br_trace.h>
/* Bottom half (ADR-0011): the IRQ exit drains bh work items — this is how "minimal ISR work" is realized. */
#include <br/core/br_work.h>

#include "irq_internal.h"

/* =====================================================================
 * Static pools (§3.3: all BSS, no runtime construction; RAM = 2 KB + 512 B @ 64 lines)
 * ===================================================================== */

static br_irq_desc_t s_desc[BR_IRQ_MAX];   /* config and identity (layout is golden) */
static br_irq_rt_t   s_rt[BR_IRQ_MAX];     /* runtime counters (storm cursor always present) */
static br_irq_cpu_t  s_cpu;                /* this core's IRQ state (TPIDR_EL1 points here) */

/* Convention for cumulative counters (§14.4): **saturate, never wrap** — wrapping makes
 * "counter went down" read as "problem gone"; saturation costs one compare. Cursor-like
 * values (rt->storm) are exempt from this rule. */
static br_u32 sat_inc_u32(br_u32 v)
{
    return (v == 0xFFFFFFFFu) ? v : (v + 1u);
}

/* Saturating br_u32 → br_u16 narrowing (only for small window values like storm/epoch; see br_irq_storm_check) */
static br_u16 sat_u16(br_u32 v)
{
    return (v > 0xFFFFu) ? (br_u16)0xFFFFu : (br_u16)v;
}

/* =====================================================================
 * CPU-local (§14.4; TPIDR_EL1 is core-exclusive, 3-01 §2.2)
 * ===================================================================== */

br_irq_cpu_t *br_irq_cpu(void)
{
    return &s_cpu;
}

void br_irq_cpu_init(void)
{
    /*
     * ★ Call order (header br_irq.h §7): after BSS zeroing, before platform submits the binding table.
     * In the design this step belongs to core.init — v0.2.0 calls it from the core entry `br_core_main()`
     * at boot chain stage ① (see ADR-0008); before that cut it is called directly from Platform Entry asm.
     * ★ **Hard constraint**: this function sets pic_id/dom_id of **all** descriptors to -1 (P-IRQ-5), so it
     *   must run **before** platform's `br_irq_bindings_set()` — reversing the order wipes the bindings.
     *
     * P-IRQ-5 (implementation decision, must be stated): "BSS is zero ⇒ no other construction" holds for
     * br_irq_cpu_t but **not for the descriptor pool** — 0 is a **legal encoding** for both pic_id and
     * dom_id (PIC 0 / domain 0), not "invalid". Without explicit init, an unbound descriptor has
     * dom_id == 0 ⇒ br_irq_register mistakes "unbound" for "domain member" and returns -EINVAL, while the
     * -ENODEV required by §14.3 (a wrong init order must be reported correctly) is never hit.
     * ⇒ set both index fields to the internal invalid value -1 (§3.2: "use -1/out-of-range for invalid").
     */
    for (br_u32 v = 0u; v < BR_IRQ_MAX; v++) {
        s_desc[v].pic_id = (br_s8)-1;
        s_desc[v].dom_id = (br_s8)-1;
    }

    /* The frozen header irq_internal.h notes in_handler is always 0xFFFF in v1 (BSS zero is 0) —
     * v1 has no fault handler chain (§10.5 registration API is v2); this only keeps diagnostics honest. */
    s_cpu.in_handler = BR_IRQ_NO_HANDLER;

    /* Attach this core's CPU-local to TPIDR_EL1 (3-02 §14.4 "init order") */
    __asm__ volatile("msr tpidr_el1, %0" :: "r"(&s_cpu) : "memory");
}

void br_irq_cpu_enable(void)
{
    /*
     * Global IRQ enable: unmasks PSTATE.I only (§14.3 chain end "after all plugin init"; F/A/D unchanged).
     * Called by the plugin manager after the LATE phase, before the START phase (see phase driver in plugin_mgr.c).
     */
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

/* =====================================================================
 * Descriptor / runtime table access (§3.3; out-of-range and BR_IRQ_INVALID both give NULL)
 * ===================================================================== */

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

br_bool br_irq_in_isr(void)
{
    return (s_cpu.isr_nest > 0u) ? BR_TRUE : BR_FALSE;
}

/*
 * ★ ADR-0011: the thread-only enforcement test **tightens** from "in ISR" to "in atomic context".
 *   Why it must tighten: bh runs on the IRQ exit (`s_cpu.isr_nest` is already 0 ⇒ `br_irq_in_isr()` is false),
 *   but it is still on the **IRQ-disabled exception exit** — registering/enabling IRQs or blocking there
 *   would keep the exception frame from ever returning.
 *   They stay separate (rather than counting bh as `br_irq_in_isr`): tests must mechanically distinguish
 *   "the handler truly does not run in ISR context" (§9.4/§12.1 criterion), which is why bh exists.
 */
br_bool br_irq_in_atomic(void)
{
    if (br_irq_in_isr() != 0) {
        return BR_TRUE;
    }
    return (br_work_in_bh() != BR_FALSE) ? BR_TRUE : BR_FALSE;
}

br_u32 br_irq_lock_depth(void)
{
    return s_cpu.irq_depth;
}

/* =====================================================================
 * L2: br_irq_lock / br_irq_unlock (§6.4; ISR-safe — no locking, no tracing, no clock reads)
 * ===================================================================== */

/* br_irq_state_t bit layout is golden (§6.4): bit0..3 = I/F/A/D before entry, [17:4] = depth copy */
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

/* **Rebuild** the dialect raw from st's 4 mask bits (the underflow guard still honors st's I bit, §6.4).
 * ★ This holds only for dialects where raw DAIF bit positions = PSTATE bit positions (aarch64);
 *   st's 4 bits are by definition the BR_DAIF_* mapping (see encode_state), so the rebuild is self-consistent. */
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
        raw = ops->cpu_mask(pic);          /* L2 proper = dialect wrapper over PSTATE.I (§6.1) */
    } else {
        /*
         * P-IRQ-6 (implementation decision): before the PIC is registered (EARLY phase, before §14.3 step 1)
         * there is no dialect to call and IRQs cannot really be disabled. That window is by design the
         * "global IRQ disabled" phase, so the synthesized state sets **all mask bits to 1**: truthful (IRQs
         * really are off) and it keeps the later unlock from wrongly enabling IRQs (had I=0 been synthesized,
         * unlock would try to "restore unmasked", inverting the semantics).
         */
        raw = BR_DAIF_I | BR_DAIF_F | BR_DAIF_A | BR_DAIF_D;
    }

    const br_irq_state_t st = encode_state(raw, cpu->irq_depth);

    /*
     * Outermost (0→1) records raw: unlock uses it for "exact pre-entry state" restore (§6.4).
     *
     * ★ P-IRQ-14 (implementation decision / interface reading): the frozen header irq_internal.h comments
     *   this field as "encoded as br_irq_state_t", but br_pic_ops.cpu_unmask() takes **dialect raw**
     *   (aarch64 = PSTATE.DAIF bits), and v1's restore just hands it back to the dialect as-is
     *   ⇒ storing raw is what makes "restore = exact replay" (storing the st encoding would need another
     *   decode, one more error source). The field width (u32) fits either, so the frozen layout is
     *   unchanged; this implementation governs that comment's reading.
     */
    if (cpu->irq_depth == 0u) {
        cpu->saved_daif = raw;
    }
    cpu->irq_depth++;

    return st;
}

void br_irq_unlock(br_irq_state_t st)
{
    br_irq_cpu_t *cpu = &s_cpu;

    /* ★ The underflow guard must come first (§6.4): if depth is already 0 and we decrement again, uint32
     * wraps to 0xFFFFFFFF ⇒ it never reaches 0 again ⇒ unlock never restores ⇒ this core's IRQs stay
     * **off** with no error reported. */
    if (cpu->irq_depth == 0u) {
        /* Trace: the event family has no separate "underflow" item (frozen br_trace.h), so use st's depth copy as a —
         * it distinguishes a "stale unlock (copy non-zero)" from a "pure surplus unlock (copy 0)" (§6.4). */
        const br_u32 extra = (br_u32)((st & BR_IRQ_ST_DEPTH_MASK) >> BR_IRQ_ST_DEPTH_SHIFT);
        br_trace_emit(BR_TRACE_IRQ_UNLOCK_UNDERFLOW, extra, 0u);
        restore_irq_raw(raw_from_state(st));    /* still honors st's I bit, leaves no dangling state */
        return;
    }

    cpu->irq_depth--;
    if (cpu->irq_depth != 0u) {
        return;                                 /* ★ inner unlock never enables IRQs (TC-IRQ-004) */
    }

    /* v1 manages I only (§6.4: F/A/D left as-is); restore exactly from the raw captured at outermost lock */
    restore_irq_raw(cpu->saved_daif);
}

/* =====================================================================
 * Per-line masking (L1; §6.2 / §4.3)
 * ===================================================================== */

/*
 * ★ Valid for **direct lines** only: a domain member's pic_id names the parent PIC and its hwirq is the
 *   in-domain child number; calling this on it would treat the child number as a parent PIC hwirq
 *   ⇒ **masking the wrong physical line** (§14.1). So domain members are explicitly rejected here;
 *   masking domain child IRQs goes through br_irq_domain_ops.mask (irq_domain.c).
 */
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
    /* §4.3 exemption: no hardware mask capability ⇒ the pure software shadow is the last gate, do nothing here */
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

/* =====================================================================
 * §7.1 / IR-7: capability degradation and priority quantization
 * ===================================================================== */

/* Can this controller express the trigger? (§4.2: never silently rewrite a missing capability; prefer -ENOTSUP) */
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

/*
 * Priority quantization tracing (§7.1 "report in both directions"; only downward quantization is possible here).
 *
 * ★ P-IRQ-3 (implementation decision): the `step = 1 << (8 - prio_bits)` here assumes **MSB-aligned
 *   hardware encoding** (GICv3: 3 valid bits ⇒ reachable values {0x00,0x20,…,0xE0}), so "round up to a
 *   multiple of step" = saturating logical priority **toward lower priority** (never raises it).
 *   LSB-aligned dialects (NVIC: contiguous 0..2^n-1) cannot use this formula — their core-side
 *   quantization/encoding belongs to the dialect, the core only sees the "small = high" logical order
 *   (design §7.1: logical→hardware encoding is a dialect duty). v0.1 only supports GICv3 (MSB-aligned),
 *   hence this decision.
 */
static void trace_prio_quantized(br_u32 virq, br_u8 prio, const br_pic_caps_t *caps)
{
    /* Precondition: the caller invokes this only when CAP_PRIO is set (the no-CAP_PRIO degradation trace happens in register) */
    if (caps->prio_bits < 1u || caps->prio_bits > 7u) {
        return;     /* bits 0/≥8: step undefined or always 1 ⇒ no quantization */
    }

    const br_u32 step = 1u << (8u - (br_u32)caps->prio_bits);
    if (((br_u32)prio & (step - 1u)) == 0u) {
        return;     /* already on a realizable step */
    }

    /*
     * Round toward **larger value = lower priority** (to a multiple of step), never round down (that would raise it).
     * Note enc is carried in br_u32 and is **not narrowed to u8**: for prio near 0xFF (e.g. prio=0xFF,
     * step=2) the rounded result is 256, outside the 8-bit range — exactly showing that "the requested
     * step above the lowest priority is still not expressible"; record 256 faithfully, rather than wrapping
     * to 0 (0 reads as "highest priority", a silent priority raise).
     */
    const br_u32 enc = ((br_u32)prio + step - 1u) & ~(step - 1u);

    /* b = (encoded << 8) | logical (§br_trace.h event semantics) */
    br_trace_emit(BR_TRACE_IRQ_PRIO_QUANTIZED, virq,
                  ((br_u64)enc << 8) | (br_u64)prio);
}

/* =====================================================================
 * native API: register / enable / disable (§14.1; all thread-only)
 * ===================================================================== */

/*
 * Runtime enforcement of thread-only (§14.1).
 * In the design this ban is enforced by "static scan + conformance matrix" (ISR whitelist, CA-3);
 * the prototype adds **one extra** runtime guard here: a call inside ISR/bh is rejected with -EINVAL.
 * Rationale: the per-line mask configuration path may hold a core internal lock, and taking it inside an
 * ISR self-deadlocks (§5.3); while bh sits on the IRQ-disabled exception exit, where register/enable would
 * make the exception frame's return path unpredictable.
 * ★ ADR-0011: the test is `br_irq_in_atomic()` (ISR **or** bh), no longer only ISR.
 */
static int direct_api_guard(br_u32 irq, br_irq_desc_t **out)
{
    if (br_irq_in_atomic() != BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    br_irq_desc_t *d = br_irq_desc_of(irq);
    /* out-of-range / domain member (the two families are exclusive, §14.1) / not registered all give -EINVAL */
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
    /* Families are exclusive (§14.1): calling direct register on a domain member must be rejected at
     * **registration time**, else later the sub number would be used as a hwirq to mask an unrelated physical line. */
    if (d->dom_id >= 0) {
        return BR_ERR(EINVAL);
    }
    /* INV-A: each physical line has exactly one owner */
    if (d->isr != BR_NULL) {
        return BR_ERR(EBUSY);
    }
    /* §14.3: unbound ⇒ -ENODEV (a wrong init order must be reported correctly, not "register succeeds but never fires") */
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

    /* Static fact from the binding table (§3.4: PERCPU is "usually filled by derivation from the binding table") */
    const br_bool bound_percpu = ((d->flags & (br_u16)BR_IRQ_F_PERCPU) != 0u);

    br_u32 flags = 0u;
    br_u8  prio = d->prio;            /* binding table static default (used when attr.prio == DEFAULT) */
    br_u8  trigger = d->trigger;      /* binding table static default (used when attr.trigger == DEFAULT) */
    br_bool trigger_requested = BR_FALSE;

    if (attr != BR_NULL) {
        flags = attr->flags;

        /* ★ §3.4: unknown bit set ⇒ -EINVAL (deliberately not silently ignored: otherwise a plugin built
         * against a newer header would quietly lose its requested attribute on an old core, the hardest
         * compatibility bug to find). */
        if ((flags & ~(br_u32)BR_IRQ_F_KNOWN_MASK) != 0u) {
            return BR_ERR(EINVAL);
        }
        /* Shared lines are out of scope for v1 (§3.4) */
        if ((flags & BR_IRQ_F_SHARED) != 0u) {
            return BR_ERR(ENOTSUP);
        }
        /* §3.4: hand-filled PERCPU contradicting hardware (the binding table) ⇒ -EINVAL (prevents "balancing a PPI as an SPI");
         * the reverse (binding says PPI but the driver left it unset) is legal — the bit is derived from the binding table and added
         * to the descriptor. */
        if ((flags & BR_IRQ_F_PERCPU) != 0u && bound_percpu == BR_FALSE) {
            return BR_ERR(EINVAL);
        }
        /*
         * Dispatch form (§12.1).
         *   DISPATCH_BH  —— ★ ADR-0011: **really effective** (no longer "set bit ignored"): that line's ISR is
         *                    deferred to the bottom half and the bit **stays** in the descriptor flags as the runtime truth.
         *   DISPATCH_THREAD —— still unimplemented (threaded IRQ is Stage 2, §12.1/§11.4):
         *                    accepted but ignored + traced, and cleared from the stored flags — a descriptor must not
         *                    claim a form that has no effect in this version (§3.4).
         */
        if ((flags & BR_IRQ_F_DISPATCH_THREAD) != 0u) {
            br_trace_emit(BR_TRACE_IRQ_DISPATCH_IGNORED, irq,
                          (br_u64)BR_IRQ_F_DISPATCH_THREAD);
            flags &= ~(br_u32)BR_IRQ_F_DISPATCH_THREAD;
        }
        if ((flags & BR_IRQ_F_STATS) != 0u && (BR_IRQ_STATS == 0)) {
            return BR_ERR(ENOTSUP);       /* §3.4: bit set while stats are compiled out ⇒ -ENOTSUP */
        }

        /* `prio`/`trigger` are platform's to interpret (§7.1/§8.1): DEFAULT ⇒ use the binding static value */
        if (attr->prio != (br_u8)BR_IRQ_PRIO_DEFAULT) {
            prio = attr->prio;
        }
        if (attr->trigger != (br_u8)BR_IRQ_TRIG_DEFAULT) {
            trigger = attr->trigger;
            trigger_requested = BR_TRUE;
        }
    }

    /* Legal range of the trigger encoding (§8.1) */
    if (trigger > (br_u8)BR_IRQ_TRIG_EDGE_BOTH) {
        return BR_ERR(EINVAL);
    }

    /*
     * ---- Capability degradation (§4.2: never silently change semantics; either "weaker but deterministic"
     *      + trace, or -ENOTSUP) ----
     *
     * ★ The caps check for trigger targets only an **explicit driver request** (attr.trigger != DEFAULT):
     *   §4.2 says "attr.trigger may only be DEFAULT; a non-DEFAULT request → -ENOTSUP".
     *   The binding table's static trigger is platform's declaration about its own hardware (§8.1 "board info")
     *   and is trusted data — applying the caps check to it would make a level-only controller unable to even
     *   register because the binding table wrote LEVEL_LOW, conflating "platform data" with "driver request".
     */
    if (trigger_requested == BR_TRUE &&
        (caps->caps & BR_PIC_CAP_TRIG_EDGE) == 0u) {
        return BR_ERR(ENOTSUP);           /* non-DEFAULT request but the controller has no programmable trigger ⇒ reject */
    }
    if (trigger_requested == BR_TRUE && trigger == (br_u8)BR_IRQ_TRIG_EDGE_BOTH &&
        (caps->caps & BR_PIC_CAP_TRIG_BOTH) == 0u) {
        return BR_ERR(ENOTSUP);
    }

    const br_bool caps_prio = ((caps->caps & BR_PIC_CAP_PRIO) != 0u) ? BR_TRUE : BR_FALSE;
    if (caps_prio == BR_FALSE) {
        /*
         * §4.2: controller does not support priority ⇒ attr.prio is ignored but still recorded in the
         * descriptor + traced (not silent); with caps_prio==FALSE, set_prio is not called below.
         * ★ P-IRQ-7 (implementation decision): the frozen event family has no separate "prio unsupported"
         *   item ⇒ reuse PRIO_QUANTIZED with encoded recorded as 0 (hardware does not express this priority).
         *   Consumers should read "encoded == 0 and logical != 0" as a **missing capability**, not quantization.
         */
        br_trace_emit(BR_TRACE_IRQ_PRIO_QUANTIZED, irq, ((br_u64)0u << 8) | (br_u64)prio);
    }

    /* ---- Hardware configuration must be inside the L2 critical section (§14.1: "must not be interrupted by
     *      this line while configuring pic_ops.set_*"; order follows §6.5 "L2 before L1") ---- */
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
        return r;       /* hardware config failed ⇒ propagate the negative errno, do not register (no "half-registered" state) */
    }

    /* ---- §7.1 / IR-7: priority quantization trace (report on hit, in both directions) ---- */
    if (caps_prio == BR_TRUE) {
        trace_prio_quantized(irq, prio, caps);
    }

    /* ---- Commit the registration last (§14.1; isr is set only now, so the failure paths above leave no owner) ----
     * hwirq/pic_id were filled by br_irq_bindings_set (§3.3 binding cache) and are not rewritten here.
     *
     * ★ ADR-0011: flags must be **OR-ed**, not overwritten — the binding table (bindings_set) may already
     *   have written `BR_IRQ_F_DISPATCH_BH` (a statically declared dispatch form, §12.2 default carrier) into
     *   the descriptor, and attr is a separate source. Overwriting would silently drop a line where
     *   "the binding table says BH but attr is empty" back to INLINE. */
    const br_u16 binding_flags =
        (br_u16)(d->flags & (br_u16)(BR_IRQ_F_PERCPU | BR_IRQ_F_DISPATCH_BH));

    d->isr     = isr;
    d->arg     = arg;
    d->flags   = (br_u16)((br_u16)flags | binding_flags |
                          (bound_percpu == BR_TRUE ? (br_u16)BR_IRQ_F_PERCPU : (br_u16)0u));
    d->prio    = prio;
    d->trigger = trigger;
    d->sub     = 0u;                 /* direct line: the domain child number is always 0 */
    d->depth   = 1u;                 /* §6.2 initial value: registered = masked */

    /* §6.2: after register the hardware must be masked, else enable sees depth==0, returns 0 immediately,
     * and the hardware is never unmasked (classic regression point). */
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
        return BR_OK;                /* already unmasked: idempotent success (§6.2, friendly to driver cleanup) */
    }
    d->depth--;
    if (d->depth == 0u) {
        br_irq_pgm_unmask(d);        /* 1 → 0: only the outermost actually touches hardware */
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

    const br_irq_state_t st = br_irq_lock();   /* §6.5: L2 first to block this core, then change hardware */
    if (d->depth == 0u) {
        br_irq_pgm_mask(d);                    /* 0 → 1: really taken down */
    }
    d->depth++;
    br_irq_unlock(st);
    return BR_OK;
}

/* =====================================================================
 * §8.3: windowed storm detection (safety feature, always on — not behind a diagnostic switch)
 * ===================================================================== */

void br_irq_storm_check(br_irq_desc_t *d)
{
    if (d == BR_NULL) {
        return;
    }

    br_irq_cpu_t *cpu = &s_cpu;

    /* Escape hatch (§3.4/§8.3): known and legitimate high-rate lines can be exempted */
    if ((d->flags & (br_u16)BR_IRQ_F_NO_STORM_GUARD) != 0u) {
        return;
    }

    /* Descriptor pointer → virq (callers can only pass in-pool descriptors; see the §14.2 hidden contract) */
    const br_u32 virq = (br_u32)(d - &s_desc[0]);
    if (virq >= BR_IRQ_MAX) {
        return;
    }
    br_irq_rt_t *rt = &s_rt[virq];

    /*
     * ★ Window time base: br_irq_enter already does one saturating +1 on cpu->isr_seq at entry (see P-IRQ-8);
     *   this function **only samples**, it does not increment.
     *   P-IRQ-8 (implementation decision): the design §8.3 pseudocode writes `cpu->isr_seq++` in **two** places
     *   (once at entry, once in the per-line window block); doing both adds 2 per interrupt and squeezes the
     *   256 window into an effective window of ~128 interrupts — drifting the criterion from design §8.3's
     *   "64 of the last 256 interrupts (25%)" to ~50%. Hence: **the single increment point is the top of
     *   br_irq_enter** (the one point of truth for "any line entered"), and this function keeps "check" semantics.
     *
     * ★ epoch is uint16 (frozen layout §3.3) and isr_seq is uint32 ⇒ the comparison must be done in 16-bit
     *   modular arithmetic: P-IRQ-9 (implementation decision): `(br_u16)(isr_seq - epoch)` gives "interrupts
     *   since the last window start (mod 2^16)"; the window is only 256 ≪ 32768 ⇒ unambiguous. A direct
     *   32-bit compare (isr_seq - epoch) would treat epoch as an "absolutely small number" and once isr_seq
     *   passes ~65792 the condition is always true ⇒ storm is reset to 1 every time ⇒ **the detector dies
     *   permanently** (exactly the "always failed" class §8.3 warns about).
     */
    const br_u16 now = sat_u16(cpu->isr_seq);
    if ((br_u16)(now - rt->epoch) > (br_u16)BR_IRQ_STORM_WINDOW) {
        rt->epoch = now;                 /* window expired: restart counting */
        rt->storm = 1u;
    } else {
        rt->storm++;                     /* the N-th within the window */
    }

    /* Report only on "the one crossing the threshold" (rt->storm == LIMIT + 1), not on every over-limit hit */
    if (rt->storm == (br_u16)(BR_IRQ_STORM_LIMIT + 1u)) {
        cpu->storm_total = sat_inc_u32(cpu->storm_total);
        br_trace_emit(BR_TRACE_IRQ_STORM, virq, (br_u64)rt->storm);
#if BR_IRQ_STORM_GUARD
        /* Off by default (§8.3/IR-9): auto-mask turns a "livelock" into "this interrupt silently stops working",
         * which is harder to find. "An observable persistent error beats a silent loss of function" ⇒ by default
         * only count + trace. */
        br_irq_pgm_mask(d);
#endif
    }
}

/* =====================================================================
 * Bottom half (bh): deferral, suppression and rollback of BR_IRQ_F_DISPATCH_BH (ADR-0011)
 *
 * Design basis: `3-02` §11.1 path one (ISR → deferred work = work queue), §12.4 (skeleton of "mask the
 * same line while handling"), §11.4.1 (disable semantics under BH), IR-10 (a failed submit must roll back).
 *
 * Execution point choice (must be stated, since it is the whole mechanism): **after eoi, before ERET**.
 *   (a) already eoi'd ⇒ however long the handler runs, this line never stays Active (a raised running
 *       priority ⇒ system-wide IRQ starvation, §4.1.1/§5.3);
 *   (b) still in exception context ⇒ no worker thread needed, independent of whether the scheduler is registered;
 *   (c) **before** `br_sched_irq_epilogue()` ⇒ threads woken in bh are visible to the preemption decision of
 *       the same exit (preempt-class schedulers).
 * Cost (recorded honestly): bh runs with IRQs **disabled** ⇒ work items must be short and must not block
 * (see the br_work.h contract).
 * ===================================================================== */

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

/* Descriptor → virq (in-pool index; defensive bounds check — out-of-range can only come from a core bug). */
static br_u32 desc_virq(const br_irq_desc_t *d)
{
    const br_u32 v = (br_u32)(d - &s_desc[0]);

    return (v < BR_IRQ_MAX) ? v : BR_IRQ_INVALID;
}

/*
 * bh work item body: run this line's ISR in bh context, then unmask the hardware line.
 *
 * The §11.4.1 **suppression check** lives here: for the entry "queued but not yet started", re-check the
 * mask state before dispatch — if disabled ⇒ **skip the handler** (the item is still consumed off the
 * queue, not occupying queue depth) + count + trace; **no catch-up/replay** (after enable, historical
 * work is not secretly run again).
 * With the current implementation ("drain happens on the same IRQ exit") this window is usually
 * unobservable (no thread can interleave); but the check stays — it is where §11.4.1 tightens §6.3's
 * promise from "the ISR is no longer called" to "no new call is **started**", and it is the only correct
 * behavior when the queue budget is exceeded and work items linger across interrupts.
 */
static void br_irq_bh_run(void *arg)
{
    br_irq_desc_t *d = (br_irq_desc_t *)arg;

    if (d == BR_NULL || d->isr == BR_NULL) {
        return;
    }

    const br_u32 virq = desc_virq(d);

    if (d->depth > 0u) {
        /* Suppressed: do not call the handler and do **not** unmask the line (disable means "stay masked"). */
        s_cpu.suppressed_dispatch = sat_inc_u32(s_cpu.suppressed_dispatch);
        br_trace_emit(BR_TRACE_IRQ_DISPATCH_SUPPRESSED, virq, 0u);
        return;
    }

    d->isr(d->arg);
    br_irq_pgm_unmask(d);          /* bh window ended ⇒ unmask (a level line re-asserts for another round) */
}

/*
 * The "dispatch decision" at the ISR slot: BH ⇒ mask the same line + submit a work item (the INLINE call
 * site does not come through here).
 *
 * Why mask: the §12.4/§9.4 skeleton is "mask the same line while handling" — otherwise a level line would
 * re-assert before bh even runs, flooding the static queue from the same line. mask is a hardware action
 * (MMIO) and satisfies the precondition "can be done in an ISR" (same origin as masking the parent line
 * in a SLOW domain).
 */
static void br_irq_dispatch_defer(br_irq_desc_t *d, br_u32 virq)
{
    br_irq_pgm_mask(d);

    if (br_work_submit(br_irq_bh_run, d) != 0) {
        /*
         * ★ Same IR-10 discipline: **a failed submit must be rolled back**. The action already taken here is
         *   "masked the same line"; without rollback the line silently stops working forever (with no error
         *   at all) — the hardest class of failure to find. Rollback = unmask (only if logically still
         *   unmasked; if it was disabled it must stay masked). A level line re-asserts immediately after
         *   unmask ⇒ no event lost; an edge line loses this one (§6.3 honesty clause: when the queue is
         *   full, prefer observable loss over a silent deadlock).
         */
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

/* =====================================================================
 * IRQ entry (§5.2: ack → dispatch → ISR → **single exit eoi**; called by the asm stub)
 * ===================================================================== */

void br_irq_enter(br_exc_frame_t *frame)
{
    /*
     * v0.1 does not use the context in the exception frame to **switch stacks**: the ISR always returns to
     * the interrupted context. ★ The precise statement after ADR-0011: the IRQ exit **does** switch stacks
     * (the preempt scheduler's preemption decision is in `br_sched_irq_epilogue()`), but that switch point
     * is a **C call boundary** (`br_sched_switch_to` saves callee-saved + sp), so the preempted thread's
     * context still sits complete on this function's stack frame — hence vectors.S saving caller-saved only
     * still holds (see "v2 obligations" under P-IRQ-ASM in the vectors.S header; this implementation needs
     * no change to that asm, because the stack switch does not happen in the few asm lines before ERET).
     */
    (void)frame;

    br_irq_cpu_t *cpu = &s_cpu;

    /* Window time base (§8.3): saturating +1 per IRQ entry — the only increment point (see P-IRQ-8 in br_irq_storm_check) */
    cpu->isr_seq = sat_inc_u32(cpu->isr_seq);

    const br_u32 pic_id = br_irq_cpu_pic_id();
    br_pic_t *pic = br_irq_pic(pic_id);
    const br_pic_ops_t *ops = br_irq_pic_ops(pic_id);
    const br_pic_caps_t *caps = br_irq_pic_caps(pic_id);

    if (pic == BR_NULL || ops == BR_NULL || ops->ack == BR_NULL) {
        /* No CPU-interface PIC: nothing to claim (init order violated, §14.3) ⇒ never eoi */
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_NOCAP, BR_PIC_NO_IRQ, 0u);
        return;
    }

    const br_u32 hwirq = ops->ack(pic);      /* Pending → Active (one IAR read suffices, §4.1.1) */
    if (hwirq == BR_PIC_NO_IRQ) {
        /* ★ Not claimed ⇒ never eoi (a stray write breaks other lines' state machine, §5.3 criterion table row 1) */
        cpu->spurious_nocap = sat_inc_u32(cpu->spurious_nocap);
        br_trace_emit(BR_TRACE_IRQ_SPURIOUS_NOCAP, hwirq, 0u);
        return;
    }

    br_u32 virq = 0u;
    br_irq_desc_t *d = BR_NULL;
    br_u32 served_virq = BR_IRQ_NO_HANDLER;

    if (br_irq_virq_of(pic_id, hwirq, &virq) != BR_OK) {
        /* Claimed but unbound (§5.3 criterion table row 3) ⇒ skip the ISR, still must eoi */
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

    /*
     * ★ §4.3 exemption — the only place where the shadow may influence "whether to call the ISR":
     *   only when the controller has **no hardware mask capability** (CAP_MASK=0) and the line is in the
     *   software-masked state (depth>0). With hardware mask capability, "masked yet still arriving" is the
     *   real bug signal (spurious_owned). This branch **must eoi** (already ack'd; otherwise the line stays
     *   Active forever ⇒ raised running priority ⇒ system-wide IRQ starvation).
     */
    if (((caps == BR_NULL) || ((caps->caps & BR_PIC_CAP_MASK) == 0u)) && d->depth > 0u) {
        cpu->spurious_nocap = sat_inc_u32(cpu->spurious_nocap);
        br_trace_emit(BR_TRACE_IRQ_SUPPRESSED, virq, 0u);
        goto EOI;
    }

#if BR_IRQ_STATS
    {
        br_irq_rt_t *rt = br_irq_rt_of(virq);
        if (rt != BR_NULL) {
            rt->count = sat_inc_u32(rt->count);      /* §14.4: cumulative counter saturates */
        }
    }
#endif

    cpu->curr_virq = (br_u16)virq;
    cpu->isr_nest++;
    served_virq = virq;
    br_trace_emit(BR_TRACE_IRQ_ENTER, virq, (br_u64)hwirq);

    /*
     * Dispatch decision (§12.1). The ISR contract is unchanged: minimal work + no blocking/malloc/returning with a lock held (§1.2).
     *   INLINE ⇒ run in place (default; all platform binding lines are this);
     *   BH     ⇒ mask the same line + submit a work item; the real handler runs in bh **after eoi**.
     */
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
    /* ★ The single eoi point: everything "claimed" reaches here (normal / invalid virq / unregistered / suppressed);
     *   "not claimed" already returned above in two places ⇒ structurally free of "driver forgot eoi" (§5.3/INV-H). */
    ops->eoi(pic, hwirq);

    /* ---- OUT (§5.2 exit): INV-C assertion + scheduler seam ---- */
    if (cpu->irq_depth != 0u) {
        /*
         * INV-C (§6.4): runtime enforcement of "no return from an ISR holding a lock". v1 does not nest
         * (IR-6) ⇒ at ISR entry it must be 0 (IRQs are masked while a lock is held), so a non-zero exit can
         * only come from "took a lock in this ISR and did not release it".
         */
#ifdef BR_DEBUG
        br_panic_bare("INV-C violated: irq_depth=%u leaked across IRQ exit (virq=%u)",
                      (br_u32)cpu->irq_depth, (br_u32)served_virq);
#else
        /*
         * release self-heal (§6.4 IR-15 revision): a leaked L2 lock stays in the **per-core** irq_depth and,
         * without a panic, would silently run all later threads with IRQs disabled ⇒ the right action is
         * "fix and report": force to zero + restore IRQs + trace.
         */
        const br_u32 leaked = cpu->irq_depth;
        cpu->irq_depth = 0u;
        /* v0.1 restores I only: raw=0 ⇒ unmask IRQs (cpu_unmask touches only the I bit, §6.4/§4.1).
         * ★ P-IRQ-10 (implementation decision): the design says "restore per entry DAIF" and the frame's daif
         *   exists for that; but v0.1 marks frame unused (ERET restores context from SPSR), so raw=0 here.
         *   When Stage 2 really uses the frame, switch to the low 4 bits of frame->daif (layout is ready). */
        if (ops->cpu_unmask != BR_NULL) {
            ops->cpu_unmask(pic, 0u);
        }
        /* The event family has no separate INV-C item (frozen br_trace.h) ⇒ borrow the "underflow" depth-violation
         * event; b records "which line's ISR leaked" (curr_virq was reset per §5.2, hence the local copy). */
        br_trace_emit(BR_TRACE_IRQ_UNLOCK_UNDERFLOW, leaked, (br_u64)served_virq);
#endif
    }

    /*
     * ---- Bottom half (ADR-0011): drain and run bh work items ----
     * The position is the whole design: after eoi (not holding Active), before ERET (no worker/scheduler
     * dependency), before `br_sched_irq_epilogue()` (threads woken in bh are visible to the preemption
     * decision of the same exit). Single budget `BR_WORK_BH_BUDGET`: without a cap, "one interrupt can
     * stretch IRQ latency arbitrarily". Leftover items wait for the next IRQ exit (tick = `1/HZ`,
     * default 5 ms ⇒ a computable upper bound on drain).
     */
    br_work_drain(BR_WORK_BH_BUDGET);

    br_sched_irq_epilogue();
}

/* =====================================================================
 * Cross-module observation contract (§14.2; consumers = debug bridge / ramdump plugins, hence not hidden)
 * ===================================================================== */

void br_irq_stats_get(br_u32 virq, br_irq_stat_t *out)
{
    if (out == BR_NULL) {
        return;
    }

    /* Zero first: if unavailable (virq out of range / unbound) return all zero (§14.4 br_irq_stat_t semantics) */
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
        return;                         /* out of range or unbound ⇒ all zero */
    }

    br_irq_rt_t *rt = br_irq_rt_of(virq);

    out->count = (BR_IRQ_STATS != 0 && rt != BR_NULL) ? rt->count : 0u;
    out->storm = (rt != BR_NULL) ? rt->storm : 0u;
    out->depth = d->depth;

    /* §4.2: the two spurious counters are CPU-local (§14.4); v1 is single-core ⇒ effectively global */
    out->spurious_owned = s_cpu.spurious_owned;
    out->spurious_nocap = s_cpu.spurious_nocap;

    /* The three bottom-half counters (ADR-0011): also CPU-local, v1 single-core ⇒ effectively global.
     * ★ They are **this core's cumulative values** (not "this line's"), read the same way as spurious_* —
     *   querying any bound virq returns the same snapshot. Thus bh evidence need not rely on the trace ring
     *   (not lost, repeatedly readable). */
    out->bh_deferred         = s_cpu.bh_deferred;
    out->bh_dropped          = s_cpu.bh_dropped;
    out->suppressed_dispatch = s_cpu.suppressed_dispatch;

    /* §9.4: drop is meaningful only for SLOW domain members ⇒ read it from the domain object (cross-TU
     * read-only accessor). Direct lines are always 0. */
    out->drop = br_irq_domain_drop_of(d);

    out->flags   = d->flags;
    out->prio    = d->prio;
    out->trigger = d->trigger;
}
