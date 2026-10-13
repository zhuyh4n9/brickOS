/*
 * PIC contract between core and platform/ISA: core owns the number space, lifecycle and mask
 * semantics; the ISA layer implements the dialect. ack/eoi/mask/unmask/cpu_mask/cpu_unmask touch
 * registers only and are ISR-callable; bus-based masking belongs in a cascade domain.
 */
#ifndef BR_CORE_BR_PIC_H
#define BR_CORE_BR_PIC_H

#include <br/core/br_types.h>

/* PIC instance storage owned by platform (opaque; core accesses it only via ops) */
typedef struct br_pic br_pic_t;

/* `ack()` return value for "no interrupt to claim". GICv3 returns 1020-1023 (1023 = spurious);
 * the whole range must map here -- they are not legal INTIDs and EOI for a special INTID is
 * undefined behavior. */
#define BR_PIC_NO_IRQ   0xFFFFFFFFu

/* Max PIC instances (descriptor pic_id is int8_t; negatives reserved for domain members) */
#define BR_PIC_MAX      128

/* ---- capability bits ----
 * Missing capability: core degrades deterministically (trace + weaker behavior, or -ENOTSUP). */
#define BR_PIC_CAP_MASK        0x0001u  /* hardware per-line masking; missing => shadow masking */
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
    br_u32 (*ack)(br_pic_t *pic);                            /* none to claim => BR_PIC_NO_IRQ */
    void   (*eoi)(br_pic_t *pic, br_u32 hwirq);              /* Active -> Inactive (EOImode) */

    /* ---- per-line masking (callable in ISR: touches MMIO only) ---- */
    void (*mask)  (br_pic_t *pic, br_u32 hwirq);
    void (*unmask)(br_pic_t *pic, br_u32 hwirq);

    /* ---- status query (diagnostic, not hot path) ---- */
    int (*pending)(br_pic_t *pic, br_u32 hwirq, int *out);
    int (*active) (br_pic_t *pic, br_u32 hwirq, int *out);

    /* ---- configuration (thread-only) ---- */
    /* Logical priority (small = high); saturate toward lower priority when the hardware width
     * is short, never silently raise it. */
    int (*set_prio)   (br_pic_t *pic, br_u32 hwirq, br_u8 prio);     /* prio: small = high */
    int (*set_trigger)(br_pic_t *pic, br_u32 hwirq, br_u8 trigger);
    int (*trigger)    (br_pic_t *pic, br_u32 hwirq);                 /* software trigger (CAP_SW_TRIGGER only) */

    /* ---- CPU-side masking (dialect wrapper over PSTATE.I) ---- */
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

/* Register a PIC instance (EARLY phase, once per instance); returns `pic_id` (>= 0) or a negative
 * errno (-EINVAL bad argument, -ENOSPC pool full, -EBUSY duplicate). The first successfully
 * registered PIC becomes the CPU interface (sole interrupt entry, claims via ack()); later
 * instances attach only through cascade domains. */
int br_pic_register(br_pic_t *pic, const br_pic_ops_t *ops);

/* Copy the caps snapshot of a CPU interface PIC (platform keeps its own copy for caps
 * decisions). Returns -ENODEV when no PIC is registered. */
int br_pic_caps_of(br_u32 pic_id, br_pic_caps_t *out);

#endif /* BR_CORE_BR_PIC_H */
