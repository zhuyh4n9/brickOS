/*
 * Interrupt management (int framework) native API: Stage 1 mechanisms plus the bottom half (bh).
 * Covers IRQ numbering, PIC abstraction, lifecycle, fault path,
 * priority and preemption, three masking layers, trigger modes and cascade domains.
 * BR_IRQ_DISPATCH_THREAD (dedicated/shared thread), dynamic upgrade and SMP affinity are v2b:
 * setting THREAD does not error but is ignored, with a trace. Core contains no controller model
 * names -- they live in the ISA layer (br_pic.h).
 */
#ifndef BR_CORE_BR_IRQ_H
#define BR_CORE_BR_IRQ_H

#include <br/core/br_types.h>

/* ---- 1. compile-time constants (config-time only, no runtime negotiation) ---- */

/* virq space: 0..BR_IRQ_NR_PIC-1 direct PIC lines; BR_IRQ_IPI_BASE ..+N_IPI-1 IPI window
 * (reserved for v2b, unused in v1); BR_IRQ_DOMAIN_BASE .. MAX-1 cascade domain sub-IRQ windows
 * (contiguous slices per domain). */
#define BR_IRQ_MAX              64u
#define BR_IRQ_IPI_BASE         48u
#define BR_IRQ_NR_IPI            8u
#define BR_IRQ_DOMAIN_BASE      56u
#define BR_IRQ_DOMAIN_MAX        2u   /* domain pool capacity */
#define BR_IRQ_DOMAIN_WINDOW    (BR_IRQ_MAX - BR_IRQ_DOMAIN_BASE)

/* Invalid virq; public signatures use uint32 for it, never stored in internal fields */
#define BR_IRQ_INVALID          0xFFFFFFFFu

/* Per IRQ exit, drain at most this many deferred items. The drain runs with interrupts off, so
 * without a cap one interrupt could stretch latency arbitrarily; the rest waits for the next exit.
 * `hal/sched`'s BR_WORK_BH_BUDGET aliases this (one source of truth, ADR-0039). */
/* Whether a bottom half is compiled into this image at all. 1 = yes (the normal case: `hal/sched`
 * installs the hooks). A product that leaves `hal/sched` out defines this to 0, and SLOW cascade
 * domains are then rejected at creation instead of being accepted and never demuxed. It is a
 * property of the *image*, not of `hal/sched`'s header, precisely because `hal/int` must not
 * include `hal/sched` (ADR-0039). */
#ifndef BR_IRQ_HAVE_BH
#define BR_IRQ_HAVE_BH          1   /* WORKAROUND(br-wa-hal-001): image-level flag, not derived from the closure */
#endif

#define BR_IRQ_BH_BUDGET        4u

/* Per-demux round cap (domain-level storm protection) */
#define BR_IRQ_DEMUX_MAX_ROUND  8u

/* Interrupt storm protection: windowed criterion, frequency independent, ratio only */
#define BR_IRQ_STORM_LIMIT      64u
#define BR_IRQ_STORM_WINDOW     256u
#define BR_IRQ_STORM_GUARD      0     /* 1 = auto-mask on limit (default off: observable persistent error beats silent failure) */

/* Per-line counters switch; slots always exist, this only decides whether count is maintained */
#define BR_IRQ_STATS            1

/* ---- 1b. IRQ-exit hooks (ADR-0039: the HAL split) ----
 *
 * The IRQ exit has to hand control to the scheduler (stack switch / reschedule) and to the bottom
 * half (deferred work). Those mechanisms live in separate HAL bricks (`hal/sched`), and a HAL brick
 * must not depend upward, so the direction is inverted: **hal/sched installs these hooks**, the IRQ
 * core only calls them. Same shape as the policy ops table (`br_sched_ops_t`) and the platform
 * contract: the mechanism owner registers, the framework dispatches.
 *
 * Uninstalled (NULL table) is legal and means "boot, before hal/sched's early_init": the exit then
 * simply returns to the interrupted thread. `br_work_in_bh()` is false, so `br_irq_in_atomic()`
 * keeps its meaning.
 */
typedef struct br_irq_hooks {
    /* Called last on the IRQ exit path (after eoi + bh drain); may switch stacks and never returns
     * to this frame when it reschedules. */
    void (*irq_exit)(void);
    /* Deferred work: the ISR -> bh path (same contract as br_work_*). */
    int (*bh_submit)(void (*fn)(void *arg), void *arg);
    br_u32 (*bh_drain)(br_u32 max_items);
    br_bool (*in_bh)(void);
} br_irq_hooks_t;

/* Install (or, with NULL, clear) the hooks. Called once by `hal/sched`'s early_init; IRQs are still
 * disabled there, so no exit can observe a half-installed table. */
void br_irq_set_hooks(const br_irq_hooks_t *hooks);

/* ---- 2. priority / trigger / attributes ---- */

/* Logical priority: **smaller value = higher** (GIC/NVIC convention); platform interprets */
#define BR_IRQ_PRIO_HIGHEST   0x00u
#define BR_IRQ_PRIO_LOWEST    0xFFu
#define BR_IRQ_PRIO_DEFAULT   0x80u

/* Trigger encoding (platform interprets, core only passes through) */
#define BR_IRQ_TRIG_LEVEL_HIGH 0u
#define BR_IRQ_TRIG_LEVEL_LOW  1u
#define BR_IRQ_TRIG_EDGE_RISE  2u
#define BR_IRQ_TRIG_EDGE_FALL  3u
#define BR_IRQ_TRIG_EDGE_BOTH  4u
#define BR_IRQ_TRIG_DEFAULT    0xFFu   /* use the binding table's static value (board info, owned by platform) */

/* `br_irq_attr_t.flags` (append-only). Unknown bits set => register returns `-EINVAL`,
 * **deliberately not silently ignored** (a plugin built against a newer header must not quietly
 * lose the attributes it requested on an old core). */
#define BR_IRQ_F_SHARED          0x0001u  /* shared line = not a v1 target => -ENOTSUP */
#define BR_IRQ_F_PERCPU          0x0002u  /* per-core private (PPI); mismatch with hardware => -EINVAL */
#define BR_IRQ_F_DISPATCH_BH     0x0004u  /* dispatch this line to bh: the ISR is deferred to the
                                           * workqueue; hardware line masked until it returns */
#define BR_IRQ_F_DISPATCH_THREAD 0x0008u  /* Stage 2: dedicated/shared thread; unimplemented
                                           * => setting it does not error but is ignored + trace */
#define BR_IRQ_F_MIGRATABLE      0x0010u  /* v2b: allow runtime migration */
#define BR_IRQ_F_STATS           0x0020u  /* enable statistics on this line (needs BR_IRQ_STATS) */
#define BR_IRQ_F_NO_STORM_GUARD  0x0040u  /* exempt this line from storm protection (known-legal high-rate line) */
#define BR_IRQ_F_KNOWN_MASK      0x007Fu  /* v1 known-bit set */

typedef struct {
    br_u8  prio;      /* BR_IRQ_PRIO_*; beyond controller resolution => saturate to low priority + trace */
    br_u8  trigger;   /* BR_IRQ_TRIG_* */
    br_u32 flags;     /* BR_IRQ_F_* */
} br_irq_attr_t;

/* ---- 3. native API (9 entries; frozen signatures) ----
 * "Thread-only" is a context prohibition ("must not be called from an ISR"), not a requirement that
 * thread objects exist: M0's main context and plugin init phases are themselves thread contexts.
 * Direct line <-> domain member are mutually exclusive: calling a direct API on a domain member (or
 * vice versa) is always -EINVAL, reported at register time. */

/* Bind an ISR to a virq: validate the binding table -> fill the descriptor -> set prio/trigger
 * (via pic_ops, interrupts off) -> **do not enable** (depth = 1, hardware already masked). */
int br_irq_register(br_u32 irq, void (*isr)(void *), void *arg,
                    const br_irq_attr_t *attr);

/* Only when depth reaches 0 does pic_ops.unmask run; already released => idempotent 0. */
int br_irq_enable(br_u32 irq);

/* depth++; on 0->1 call pic_ops.mask (order: **L2 first, then L1**). */
int br_irq_disable(br_u32 irq);

/* ---- L2: CPU-local masking (ISR-safe, one of the four whitelist entries) ---- */

/* `br_irq_state_t` bit layout **goes into golden**; unused bits stay 0. */
typedef br_u32 br_irq_state_t;

#define BR_IRQ_ST_I_MASK       0x00000001u  /* [0]     PSTATE.I on entry */
#define BR_IRQ_ST_F_MASK       0x00000002u  /* [1]     PSTATE.F on entry (not managed in v1) */
#define BR_IRQ_ST_A_MASK       0x00000004u  /* [2]     PSTATE.A on entry (not managed in v1) */
#define BR_IRQ_ST_D_MASK       0x00000008u  /* [3]     PSTATE.D on entry (not managed in v1) */
#define BR_IRQ_ST_DEPTH_SHIFT  4
#define BR_IRQ_ST_DEPTH_MASK   0x0003FFF0u  /* [17:4]  irq_depth on entry (14 bits, debug check) */

/* Save DAIF -> disable local IRQ -> irq_depth++ -> return the token. */
br_irq_state_t br_irq_lock(void);

/* irq_depth--; restore PSTATE.I only when it reaches zero **and** the I bit was 0 on entry.
 * Underflow guard: at depth 0 **never** `--` (it would wrap and disable interrupts forever). */
void br_irq_unlock(br_irq_state_t st);

/* ---- 4. cascaded interrupt domains ----
 * v1 is single-level cascade and the parent line must be a direct PIC line. FAST demuxes in ISR
 * context; SLOW demuxes on the core workqueue, with the parent masked before submission and
 * released after demux completes. Only when no workqueue exists does
 * `br_irq_domain_create(..., F_SLOW, ...)` return NULL, so SLOW is always available in v0.2.0. */

typedef struct br_irq_domain br_irq_domain_t;

typedef struct br_irq_domain_ops {
    /* Pending query: core provides the bitmap buffer (capacity = ceil(n_sub/32) words) and the
     * driver fills the currently pending bits. A sub-IRQ that occurs after domain creation but
     * before its handler is registered is **masked** by core. */
    void (*pending)(void *priv, br_u32 *bits, br_size_t nwords);
    void (*mask)  (void *priv, br_u32 sub);   /* mask sub-IRQ */
    void (*unmask)(void *priv, br_u32 sub);
    void (*ack)   (void *priv, br_u32 sub);   /* core calls this after the sub handler returns */
} br_irq_domain_ops_t;

#define BR_IRQ_DOMAIN_F_FAST 0x0u   /* status register is memory-mapped => demux + sub handler in ISR context */
#define BR_IRQ_DOMAIN_F_SLOW 0x1u   /* status read needs a bus transaction => demux in bh. The sub
                                     * handler runs under the bh contract: IRQs off, no blocking */

/* Allocate a domain + sub-descriptor window + bitmap from the domain pool and register the parent
 * line's ISR = domain demux. Returns NULL (pool full / parent invalid or not direct /
 * n_sub out of range / ops->pending missing / SLOW requested but bh unavailable). */
br_irq_domain_t *br_irq_domain_create(const char *name, br_u32 parent_irq,
                                      br_u32 n_sub, br_u32 flags,
                                      const br_irq_domain_ops_t *ops, void *priv);

/* Fill the sub descriptor; on success sub depth = 1 (masked, like br_irq_register). */
int br_irq_register_child(br_irq_domain_t *dom, br_u32 sub,
                          void (*isr)(void *), void *arg, const br_irq_attr_t *attr);

int br_irq_enable_child (br_irq_domain_t *dom, br_u32 sub);
int br_irq_disable_child(br_irq_domain_t *dom, br_u32 sub);

/* ---- 5. platform side: virq <-> hwirq binding table (platform data, in .rodata) ---- */

/* Binding table entry static attributes */
#define BR_IRQB_F_PERCPU       0x01u   /* per-core private (PPI) */
#define BR_IRQB_F_ACTIVE_LOW   0x02u   /* active low (board-level polarity info) */
#define BR_IRQB_F_EXCLUSIVE    0x04u   /* exclusive (shared lines are a v1 non-target) */

typedef struct {
    br_u16 virq;      /* global virtual number (index semantics; must be < BR_IRQ_MAX) */
    br_u8  pic_id;    /* which PIC instance it hangs on (EARLY registration order) */
    br_u8  flags;     /* BR_IRQB_F_* */
    br_u32 hwirq;     /* hardware number within the PIC instance (INTID / IRQ number / pin) */
    br_s8  dom_id;    /* -1 = direct PIC line; >= 0 = cascade domain member */
    br_u8  prio;      /* static default priority (used when attr.prio == 0xFF) */
    br_u8  trigger;   /* static default trigger (used when attr.trigger == 0xFF) */
    br_u8  dispatch;  /* static dispatch form (Stage 2; Stage 1 accepts INLINE only) */
    br_u8  rsv[3];    /* append-only reserve (D14) */
} br_irq_binding_t;   /* 16 B */

#define BR_IRQ_DISPATCH_INLINE 0u
#define BR_IRQ_DISPATCH_BH     1u
#define BR_IRQ_DISPATCH_THREAD 2u

/* Platform side submits the binding table once (EARLY phase, exactly once; repeat => -EBUSY).
 * Core builds the hwirq->virq resolution table here: v0.1 uses a sorted table + binary search
 * (4 B per binding), not a direct-index table, which GICv3's 1024 INTIDs would make 2 KB. */
int br_irq_bindings_set(const br_irq_binding_t *tbl, br_size_t n);

/* ---- 6. cross-module observation contract (consumers: debug bridge / ramdump plugin) ---- */

/* Diagnostic snapshot (not a hot path, no golden layout promise); three bh fields appended. */
typedef struct {
    br_u32 count;          /* entry count (BR_IRQ_STATS; always 0 when disabled) */
    br_u16 storm;          /* this line's count in the current window (read rt->storm directly, no separate peak) */
    br_u16 depth;          /* current masking depth */
    br_u32 spurious_owned; /* "arrived despite hardware masking" - a real bug signal */
    br_u32 spurious_nocap; /* "expected drop when masking is unavailable" - degraded behavior */
    br_u32 drop;           /* SLOW domain submission failures (domain members) */
    br_u16 flags;          /* descriptor flags snapshot (includes the DISPATCH_BH bit) */
    br_u8  prio, trigger;
    /* ---- bottom half (bh), three fields: per-core, v1 single-core => global ---- */
    br_u32 bh_deferred;        /* BH form: successful deferrals to the bottom half */
    br_u32 bh_dropped;         /* BH form: queue full => rolled-back unsubmitted count (IR-10) */
    br_u32 suppressed_dispatch;/* already disabled before dispatch => handler suppressed count */
} br_irq_stat_t;

/* `spurious_owned`/`spurious_nocap` are CPU-local, so this returns **this core's** counters (v1
 * single-core => global). If unavailable (virq out of range / no binding) => out is zeroed. */
void br_irq_stats_get(br_u32 virq, br_irq_stat_t *out);

/* Execution context queries (the runtime-observable face of the three execution forms). They are
 * public because a domain dialect (plugin-side code) must prove its status register is not read
 * inside an ISR -- the reason the SLOW domain exists. */
br_bool br_irq_in_isr(void);        /* inside an ISR body (interrupt context) */
br_bool br_irq_in_atomic(void);     /* ISR **or** bh - the prohibition range of thread-only APIs */

/* ---- 7. core-internal symbols (asm stub calls and M0 stand-ins; **not exported**) ---- */

/*
 * CPU-local interrupt state initialization (BSS zero-init => no runtime constructor): point
 * TPIDR_EL1 at this core's `br_irq_cpu_t` and set all descriptors' pic_id/dom_id to -1. Called by
 * br_core_main() in boot-chain step (1). It must run before platform submits the PIC binding table
 * (`br_irq_bindings_set`), which would otherwise be wiped.
 */
void br_irq_cpu_init(void);

/* Globally enable interrupts (release PSTATE.I only; F/A/D unchanged). Called by the plugin manager
 * after the LATE phase and before the START phase. */
void br_irq_cpu_enable(void);

/* Interrupt entry (called by the asm stub): ack -> dispatch -> ISR -> single exit eoi. */
struct br_exc_frame;
void br_irq_enter(struct br_exc_frame *f);

/*
 * Current L2 critical-section nesting depth (core-internal). On a thread's first entry the
 * scheduling framework checks it: depth > 0 means someone yielded inside a critical section, so
 * leave the interrupt state **untouched**; depth == 0 means the thread must run with interrupts
 * released (required when the preempt scheduler switches stacks at IRQ exit, where PSTATE.I == 1).
 */
br_u32 br_irq_lock_depth(void);

#endif /* BR_CORE_BR_IRQ_H */
