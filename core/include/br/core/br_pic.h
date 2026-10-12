/*
 * brickOS prototype v0.1.0 — PIC (interrupt controller) contract: the core ↔ platform fill-in surface
 *
 * Authoritative design: `docs/3-os-core/3-02-int.md` §4.1 (`br_pic_ops`) / §4.2 (capability bits
 * and degradation) / §4.1.1 (GICv3 EOImode duty) / §14.3 (platform-side contract, **not a native surface**).
 *
 * Three-layer ownership (3-02 §1.3, this prototype's placement):
 *   semantics and mechanism  core (consumer of this header: number space/lifecycle/mask semantics)
 *   controller dialect       this header is implemented by the ISA layer (GICv3 = platform/qemu-aarch64/src/gicv3.c)
 *   data and specialization  provided by platform (PIC instance/base address/binding table/priority width)
 *
 * ⚠ This is not the native API surface (3-02 §14.3 last paragraph): the native API is "the coding
 * object of all plugin authors", whereas `br_pic_ops` is faced only by platform/ISA authors. Mixing
 * it into the native surface would distort CA-5's surface statistics.
 *
 * Hot-path discipline (3-02 §4.1/§14.3):
 *   ack / eoi / mask / unmask / cpu_mask / cpu_unmask **must touch registers only** (callable inside ISR);
 *   any masking implementation that "goes over a bus" must not live here — such controllers use a
 *   cascade domain (§9) so bus operations happen in demux/thread context.
 */
#ifndef BR_CORE_BR_PIC_H
#define BR_CORE_BR_PIC_H

#include <br/core/br_types.h>

/* PIC instance storage owned by platform (opaque; core accesses it only via ops) */
typedef struct br_pic br_pic_t;

/*
 * `ack()`'s "no interrupt to claim" return value (3-02 §14.6).
 * ★ Dialect duty: GICv3 `ICC_IAR1_EL1` returns 1020–1023 (1023 = spurious); these are
 *   **not legal INTIDs** and the **whole range** must be mapped to this value — otherwise
 *   core would look them up in the binding table as legal hwirq, miss, and then write an
 *   EOI, while writing EOI for a special INTID is undefined/deprecated behavior
 *   (the "dialect duty" of 3-02 §5.3).
 */
#define BR_PIC_NO_IRQ   0xFFFFFFFFu

/* Max PIC instances (3-02 §3.2: pic_id in the descriptor is int8_t, negatives reserved for domain members) */
#define BR_PIC_MAX      128

/* ---------------- capability bits (3-02 §4.2) ----------------
 * When missing, core behavior is **deterministic degradation** (never silently changes semantics):
 * either degrade to "weaker but definite" behavior (with a trace), or `-ENOTSUP`. */
#define BR_PIC_CAP_MASK        0x0001u  /* hardware per-line masking; missing ⇒ software-only shadow masking */
#define BR_PIC_CAP_LATCH       0x0002u  /* still latches pending while masked */
#define BR_PIC_CAP_READBACK    0x0004u  /* config/status registers readable back */
#define BR_PIC_CAP_PRIO        0x0008u  /* priority supported */
#define BR_PIC_CAP_NEST        0x0010u  /* priority nesting supported (v1 never nests) */
#define BR_PIC_CAP_AFFINITY    0x0020u  /* routable to a given core (v2b) */
#define BR_PIC_CAP_TRIG_EDGE   0x0040u  /* edge trigger supported */
#define BR_PIC_CAP_TRIG_BOTH   0x0080u  /* both edges supported */
#define BR_PIC_CAP_SW_TRIGGER  0x0100u  /* software trigger supported (SGI/self-test channel) */
#define BR_PIC_CAP_PER_CPU     0x0200u  /* per-core private interrupts (PPI) */
#define BR_PIC_CAP_RESYNC      0x0400u  /* save/restore supported (PM, v2) */

typedef struct {
    br_u8  prio_bits;   /* effective priority bits (0 = priority unsupported) */
    br_u32 nr_irq;      /* hwirq upper bound of this instance */
    br_u32 caps;        /* BR_PIC_CAP_* bitmap */
} br_pic_caps_t;

typedef struct br_pic_ops {
    const char *name;   /* "gicv3" / "gicv2" / "plic" / "nvic" / "soft" */

    /* ---- lifecycle (EARLY phase, interrupts globally disabled) ---- */
    int (*init)    (br_pic_t *pic);                          /* controller reset/init (interrupts off) */
    int (*caps_get)(br_pic_t *pic, br_pic_caps_t *out);      /* capability report (IR-3) */

    /* ---- claim and end (hot path) ---- */
    br_u32 (*ack)(br_pic_t *pic);                            /* no interrupt to claim ⇒ BR_PIC_NO_IRQ */
    void   (*eoi)(br_pic_t *pic, br_u32 hwirq);              /* Active → Inactive (EOImode duty converges here) */

    /* ---- per-line masking (callable in ISR: touches MMIO only) ---- */
    void (*mask)  (br_pic_t *pic, br_u32 hwirq);
    void (*unmask)(br_pic_t *pic, br_u32 hwirq);

    /* ---- status query (diagnostic, not hot path) ---- */
    int (*pending)(br_pic_t *pic, br_u32 hwirq, int *out);
    int (*active) (br_pic_t *pic, br_u32 hwirq, int *out);

    /* ---- configuration (thread-only) ---- */
    int (*set_prio)   (br_pic_t *pic, br_u32 hwirq, br_u8 prio);     /* prio = logical priority (small = high);
                                                                      * ★ quantizing logical→hardware encoding is a
                                                                      *   dialect duty (3-02 §7.1): saturate toward
                                                                      *   **lower priority** when the width is short,
                                                                      *   never silently raise priority */
    int (*set_trigger)(br_pic_t *pic, br_u32 hwirq, br_u8 trigger);
    int (*trigger)    (br_pic_t *pic, br_u32 hwirq);                 /* software trigger (CAP_SW_TRIGGER only) */

    /* ---- CPU-side masking (dialect wrapper over PSTATE.I; 3-02 §6.4's L2 lands here) ---- */
    br_u32 (*cpu_mask)  (br_pic_t *pic);                             /* disable local IRQ, return prior state */
    void   (*cpu_unmask)(br_pic_t *pic, br_u32 state);               /* restore I bit to prior state */

    /* ---- priority masking/nesting (CAP_NEST only; nobody calls it in v1) ---- */
    void (*set_running_prio)(br_pic_t *pic, br_u8 prio);

    /* ---- affinity (CAP_AFFINITY only; v2b) ---- */
    int (*set_affinity)(br_pic_t *pic, br_u32 hwirq, br_u32 cpu_mask);

    /* ---- PM state sync (CAP_RESYNC only; v2) ---- */
    int (*save)   (br_pic_t *pic, void *buf, br_size_t n);
    int (*restore)(br_pic_t *pic, const void *buf, br_size_t n);
} br_pic_ops_t;

/*
 * Register a PIC instance (EARLY phase; once per instance).
 * Returns `pic_id` (>= 0) or a negative errno (`-EINVAL` bad argument / `-ENOSPC` instance
 * pool full / `-EBUSY` duplicate registration of the same instance).
 *
 * ★ IR-3 corollary (3-02 §4.2): **each CPU has exactly one "CPU interface controller"** —
 *   it claims interrupts via `ack()` and is the sole interrupt entry. core records the
 *   **first successfully registered PIC** as the CPU interface (no PIC ready ⇒ no interrupts
 *   available ⇒ init order step 1, §14.3). Instances registered later can only attach via
 *   cascade domains (core introduces no "which controller is firing" test).
 */
int br_pic_register(br_pic_t *pic, const br_pic_ops_t *ops);

/*
 * Copy out the caps snapshot of a CPU interface PIC (platform writes its own copy for
 * caps-related decisions). Returns -ENODEV when no PIC is registered.
 */
int br_pic_caps_of(br_u32 pic_id, br_pic_caps_t *out);

#endif /* BR_CORE_BR_PIC_H */
