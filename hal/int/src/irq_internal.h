/*
 * IRQ subsystem internals (core-private, not exported). Descriptors and runtime counters are two
 * parallel arrays indexed by virq; sizes are pinned by _Static_assert (desc 32 B, rt 8 B).
 */
#ifndef BR_IRQ_INTERNAL_H
#define BR_IRQ_INTERNAL_H

#include <br/hal/br_fault.h>   /* br_extable_t (the table bounds below) */
#include <br/hal/br_irq.h>
#include <br/hal/br_pic.h>
#include <br/core/br_trace.h>

/* ---- Descriptor: index = virq, hot path O(1) ---- */
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
    br_u16   storm;          /* [0x1E] reserved 2 B rsv slot */
} br_irq_desc_t;

_Static_assert(sizeof(br_irq_desc_t) == 32u, "descriptor must be 32 B (3-02 §3.3 size derivation)");

/* Runtime counter table, parallel to descriptors (same index) */
typedef struct br_irq_rt {
    br_u32 count;   /* entry count (maintained only when BR_IRQ_STATS=1) */
    br_u16 storm;   /* times this line was serviced within the window (always on; safety feature) */
    br_u16 epoch;   /* isr_seq snapshot used to reset storm */
} br_irq_rt_t;

_Static_assert(sizeof(br_irq_rt_t) == 8u, "runtime table entry must be 8 B");

/* ---- CPU-local: TPIDR_EL1 points here, BSS zero-init ---- */
typedef struct br_irq_cpu {
    void   *current;          /* current thread (Stage 1 = NULL: no thread) */
    br_u32  irq_depth;        /* br_irq_lock nesting depth (enforcement target of INV-C) */
    br_u32  saved_daif;       /* DAIF saved by outermost lock (encoded as br_irq_state_t) */
    br_u32  isr_nest;         /* ISR nesting (v1 always <= 1, IR-6) */
    br_u32  in_fault;         /* fault nesting (>=2 = double fault) */
    br_u16  curr_virq;        /* virq of current ISR (fault attribution: "which ISR faulted") */
    br_u16  in_handler;       /* index of running fault handler (v2; v1 always 0xFFFF) */
    br_u32  isr_seq;          /* global ISR sequence (any line entry +1; storm window time base) */
    br_u32  storm_total;      /* storm events on this core (each threshold crossing +1) */
    br_u32  spurious_owned;   /* had hardware mask capability yet still arrived (real bug signal) */
    br_u32  spurious_nocap;   /* expected drops without mask capability (degraded behavior) */
    br_u32  bh_deferred;      /* BH form: successful deferrals to bottom half */
    br_u32  bh_dropped;       /* BH form: queue full => rolled-back uncommitted count (IR-10) */
    br_u32  suppressed_dispatch; /* already disabled before dispatch => handler suppressed count */
    br_u32  prev_fault_ec;    /* EC of previous fault */
    br_u64  prev_fault_elr;   /* ELR of previous fault */
} br_irq_cpu_t;

#define BR_IRQ_NO_HANDLER  0xFFFFu

/* ---- Cascaded domain ---- */
struct br_irq_domain {
    const char                *name;
    const br_irq_domain_ops_t *ops;
    void                      *priv;
    br_u16 parent_virq;    /* virq of parent physical line (must be a PIC direct line) */
    br_u16 virq_base;      /* start of child interrupt virq window */
    br_u16 n_sub;
    br_u16 flags;          /* BR_IRQ_DOMAIN_F_FAST / _SLOW */
    br_irq_desc_t            *sub_desc;  /* [n_sub] points into descriptor pool window */
    br_u32                   *bitmap;    /* [nwords] pending buffer (provided by core) */
    br_u16 nwords;
    br_u8  id;             /* domain index (domain pool) */
    br_u8  rsv;
    br_u16 busy;           /* SLOW domain: demux single-flight */
    br_u16 drop_count;     /* SLOW domain: deferred/dropped count for failed commits */
};

/* ---- Core-internal interfaces (shared between .c files, not exported) ---- */

/* This core's CPU-local; TPIDR_EL1 points to it after br_irq_cpu_init. */
br_irq_cpu_t *br_irq_cpu(void);

/* virq -> descriptor; out of range or BR_IRQ_INVALID gives NULL. */
br_irq_desc_t *br_irq_desc_of(br_u32 virq);

/* Runtime counter table entry */
br_irq_rt_t *br_irq_rt_of(br_u32 virq);

/* ---- PIC registry (irq_pic.c) ---- */
br_pic_t                *br_irq_pic(br_u32 pic_id);
const br_pic_ops_t      *br_irq_pic_ops(br_u32 pic_id);
const br_pic_caps_t     *br_irq_pic_caps(br_u32 pic_id);
br_u32                   br_irq_pic_count(void);
br_u32                   br_irq_cpu_pic_id(void);   /* first registered = CPU interface (IR-3 corollary) */

/* hwirq -> virq (hot path, binary search); 0 + *out on hit, -ENODEV on miss. */
int br_irq_virq_of(br_u32 pic_id, br_u32 hwirq, br_u32 *out);

/* ---- masking (irq_core.c) ---- */
void br_irq_pgm_mask  (br_irq_desc_t *d);
void br_irq_pgm_unmask(br_irq_desc_t *d);

/* ---- storm detection (irq_core.c) ---- */
void br_irq_storm_check(br_irq_desc_t *d);

/* ---- bottom half (irq_core.c) ---- */
/* Whether this line's ISR defers to bh (BR_IRQ_F_DISPATCH_BH); both line families. */
br_bool br_irq_dispatch_is_bh(const br_irq_desc_t *d);

/* Defer/drop counters (CPU-local); shared by per-line dispatch and SLOW domain demux. */
void br_irq_bh_note_defer(void);
void br_irq_bh_note_drop(void);

/* ---- domain (irq_domain.c) ---- */
/* Domain demux main loop: same code for both contexts -- FAST calls it in the parent line ISR, SLOW
 * from the bh work item. SLOW ops->pending() must stay a bus transaction, never read in the ISR. */
void br_irq_demux(br_irq_domain_t *dom);

/* Commit-failure count of the domain owning a SLOW member (direct line => 0); cross-TU read-only
 * accessor for br_irq_stats_get's br_irq_stat_t.drop. */
br_u32 br_irq_domain_drop_of(const br_irq_desc_t *d);

/* ---- exception table bounds (linker-provided) ----
 * Collected by the platform linker script (`__br_extable_start`/`__br_extable_stop` around
 * `KEEP(*(.br_extable))`). Declared here, not in irq_fault.c (ADR-0038 / R1). */
extern const br_extable_t __br_extable_start[];
extern const br_extable_t __br_extable_stop[];

/* Bh submit through the installed hook table (`irq_core.c`); -EAGAIN when nothing is installed.
 * The whole interrupt brick goes through this, never through `hal/sched` directly (ADR-0039). */
int br_irq_bh_submit(void (*fn)(void *arg), void *arg);

#endif /* BR_IRQ_INTERNAL_H */
