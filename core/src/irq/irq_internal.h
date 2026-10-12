/*
 * brickOS prototype v0.1.0 — IRQ subsystem internal truth (core private; **not exported**, CA-10)
 *
 * Key data structures and core-internal functions from 3-02 §3.3 / §14.4.
 * Layout constraints come from the design (`_Static_assert` pins them):
 *   br_irq_desc_t  = 32 B (fields ordered by decreasing width, no padding; §3.3 size derivation)
 *   br_irq_rt_t    =  8 B
 *   br_irq_binding_t = 16 B (in br_irq.h, platform data)
 *
 * ★ Descriptors and runtime counters are **two parallel arrays with the same virq index** (§3.3):
 *   the former = "configuration and identity" (layout in golden, frozen long term),
 *   the latter = "runtime counters" (can grow/shrink). Adding counters this way cannot
 *   break golden; and **storm detection is a safety feature** (§8.3), so it cannot sit
 *   behind a diagnostic switch ⇒ both pools always exist.
 */
#ifndef BR_IRQ_INTERNAL_H
#define BR_IRQ_INTERNAL_H

#include <br/core/br_irq.h>
#include <br/core/br_pic.h>
#include <br/core/br_trace.h>

/* =====================================================================
 * Descriptor (3-02 §3.3; index = virq, hot path O(1), no lookup table)
 * ===================================================================== */
typedef struct br_irq_desc {
    void   (*isr)(void *);   /* [0x00] ISR entry (thread-only register write) */
    void    *arg;            /* [0x08] */
    br_u32   hwirq;          /* [0x10] cached binding-table hwirq (saves a lookup on hot path) */
    br_u16   flags;          /* [0x14] BR_IRQ_F_* runtime attributes */
    br_u16   depth;          /* [0x16] br_irq_disable nesting count (IR-5; = 1 after register) */
    br_u8    prio;           /* [0x18] logical priority (lower value = higher) */
    br_u8    trigger;        /* [0x19] BR_IRQ_TRIG_* */
    br_s8    pic_id;         /* [0x1A] direct: PIC instance; domain member: parent PIC */
    br_s8    dom_id;         /* [0x1B] -1 = direct; >= 0 = domain id */
    br_u16   sub;            /* [0x1C] sub-IRQ number when dom_id >= 0 */
    br_u16   storm;          /* [0x1E] reserved (§3.3 rsv slot): interleaved 2 B slot */
} br_irq_desc_t;

_Static_assert(sizeof(br_irq_desc_t) == 32u, "descriptor must be 32 B (3-02 §3.3 size derivation)");

/* Runtime counter table (3-02 §3.3/§14.4; parallel to descriptors, same index) */
typedef struct br_irq_rt {
    br_u32 count;   /* entry count (maintained only when BR_IRQ_STATS=1) */
    br_u16 storm;   /* times this line was serviced within the window (§8.3; always on — safety feature) */
    br_u16 epoch;   /* isr_seq snapshot used to reset storm */
} br_irq_rt_t;

_Static_assert(sizeof(br_irq_rt_t) == 8u, "runtime table entry must be 8 B");

/* =====================================================================
 * CPU-local (3-02 §14.4; TPIDR_EL1 points here, BSS zero-init ⇒ no runtime construction)
 * ===================================================================== */
typedef struct br_irq_cpu {
    void   *current;          /* current thread (Stage 1 = NULL: no thread) */
    br_u32  irq_depth;        /* br_irq_lock nesting depth (enforcement target of INV-C) */
    br_u32  saved_daif;       /* DAIF saved by outermost lock (encoded as br_irq_state_t) */
    br_u32  isr_nest;         /* ISR nesting (v1 always <= 1, IR-6) */
    br_u32  in_fault;         /* fault nesting (>=2 = double fault, §10.6) */
    br_u16  curr_virq;        /* virq of current ISR (fault attribution: "which ISR faulted") */
    br_u16  in_handler;       /* index of running fault handler (v2; v1 always 0xFFFF) */
    br_u32  isr_seq;          /* global ISR sequence (any line entry +1; storm window time base, §8.3) */
    br_u32  storm_total;      /* storm events on this core (each threshold crossing +1) */
    br_u32  spurious_owned;   /* had hardware mask capability yet still arrived (real bug signal) */
    br_u32  spurious_nocap;   /* expected drops without mask capability (degraded behavior) */
    br_u32  bh_deferred;      /* BH form: successful deferrals to bottom half (ADR-0011) */
    br_u32  bh_dropped;       /* BH form: queue full ⇒ rolled-back uncommitted count (IR-10) */
    br_u32  suppressed_dispatch; /* §11.4.1: already disabled before dispatch ⇒ handler suppressed count */
    br_u32  prev_fault_ec;    /* EC of previous fault (for §10.6 printing) */
    br_u64  prev_fault_elr;   /* ELR of previous fault */
} br_irq_cpu_t;

#define BR_IRQ_NO_HANDLER  0xFFFFu

/* =====================================================================
 * Cascaded domain (3-02 §9.2)
 * ===================================================================== */
struct br_irq_domain {
    const char                *name;
    const br_irq_domain_ops_t *ops;
    void                      *priv;
    br_u16 parent_virq;    /* virq of parent physical line (★ must be a PIC direct line) */
    br_u16 virq_base;      /* start of child interrupt virq window */
    br_u16 n_sub;
    br_u16 flags;          /* BR_IRQ_DOMAIN_F_FAST / _SLOW */
    br_irq_desc_t            *sub_desc;  /* [n_sub] points into descriptor pool window */
    br_u32                   *bitmap;    /* [nwords] pending buffer (provided by core) */
    br_u16 nwords;
    br_u8  id;             /* domain index (domain pool) */
    br_u8  rsv;
    br_u16 busy;           /* SLOW domain: demux single-flight (§9.4; reachable since ADR-0011) */
    br_u16 drop_count;     /* SLOW domain: deferred/dropped count for failed commits */
};

/* =====================================================================
 * core internal interfaces (shared between .c files; all **not exported**)
 * ===================================================================== */

/* This core's CPU-local (static BSS instance; TPIDR_EL1 points to it after br_irq_cpu_init) */
br_irq_cpu_t *br_irq_cpu(void);

/* virq → descriptor (**with bounds check**: out of range/BR_IRQ_INVALID ⇒ NULL) */
br_irq_desc_t *br_irq_desc_of(br_u32 virq);

/* Runtime counter table entry */
br_irq_rt_t *br_irq_rt_of(br_u32 virq);

/* ---- PIC registry (irq_pic.c) ---- */
br_pic_t                *br_irq_pic(br_u32 pic_id);
const br_pic_ops_t      *br_irq_pic_ops(br_u32 pic_id);
const br_pic_caps_t     *br_irq_pic_caps(br_u32 pic_id);
br_u32                   br_irq_pic_count(void);
br_u32                   br_irq_cpu_pic_id(void);   /* first registered = CPU interface (IR-3 corollary) */

/* hwirq → virq (hot path; v1 = ordered index table + binary search, §14.2). Hit returns 0 and writes *out. */
int br_irq_virq_of(br_u32 pic_id, br_u32 hwirq, br_u32 *out);

/* ---- masking (irq_core.c) ---- */
void br_irq_pgm_mask  (br_irq_desc_t *d);
void br_irq_pgm_unmask(br_irq_desc_t *d);

/* ---- storm detection (irq_core.c; windowed criterion of §8.3) ---- */
void br_irq_storm_check(br_irq_desc_t *d);

/* ---- bottom half (irq_core.c; §11.1/§12.4/ADR-0011) ---- */
/* Whether this line's ISR is configured to defer to bh (BR_IRQ_F_DISPATCH_BH). Direct lines and domain members both. */
br_bool br_irq_dispatch_is_bh(const br_irq_desc_t *d);

/* Defer/drop counters (CPU-local). **One source of truth shared by both deferral points**:
 * per-line dispatch (irq_core.c) and SLOW domain demux (irq_domain.c) —— otherwise
 * "how many deferrals happened" would have two sources. */
void br_irq_bh_note_defer(void);
void br_irq_bh_note_drop(void);

/* ---- domain (irq_domain.c) ---- */
/* Domain demux main loop (§9.3). **The same code serves both contexts**:
 *   FAST domain = called directly in the parent line ISR; SLOW domain = called by the bh work item (ADR-0011).
 * The context difference is not in the loop, only in the **caller** (this is also where the contract
 * "SLOW domain ops->pending() must be a bus transaction and must never be read in the ISR" lands:
 * the loop itself runs in either context). */
void br_irq_demux(br_irq_domain_t *dom);

/* "Commit failure count" of the domain owning a SLOW domain member (§9.4 drop_count; direct line ⇒ 0).
 * Cross-TU read-only accessor: the domain object is irq_domain.c's static pool, and irq_core.c's
 * `br_irq_stats_get()` reads `br_irq_stat_t.drop` through it. */
br_u32 br_irq_domain_drop_of(const br_irq_desc_t *d);

#endif /* BR_IRQ_INTERNAL_H */
