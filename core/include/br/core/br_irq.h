/*
 * brickOS prototype v0.1.0 - interrupt management (int framework) native API
 *
 * Authoritative design: `docs/3-os-core/3-02-int.md` (mechanism / data structures /
 * invariants / staging); symbol surface and freeze plan:
 * `docs/3-os-core/3-01-core-api-list.md` §8/§8.1/§15.
 *
 * Staging (3-02 §1.1): this header implements the **Stage 1** (pre-scheduler, M0)
 * mechanisms, and under ADR-0011 (this prototype) **lands the first Stage 2 item
 * early** - the **bottom half (bh)**:
 *   1) IRQ number management and abstraction (§3)  2) PIC abstraction and capability
 *      fallback (§4, see br_pic.h)
 *   3) lifecycle (Pending/Active/Inactive, §5, implementation in src/irq/irq_core.c)
 *   4) fault path (synchronous exception slots / classification / extable / panic, §10,
 *      see br_fault.h; the **`br_fault_handler_register` registration API is v2**, not
 *      implemented here)
 *   5) priority and preemption (§7)                6) three masking/enable layers (§6)
 *   7) trigger modes (§8)                          8) cascaded interrupt domains (§9)
 *
 * * **bh has landed (ADR-0011)**: `BR_IRQ_F_DISPATCH_BH` is no longer "set and ignore";
 *   it really defers that line's ISR to the bottom half (workqueue,
 *   `br/core/br_work.h`); the SLOW cascade domain (whose precondition is bh) is
 *   therefore usable. **Still missing**: `BR_IRQ_DISPATCH_THREAD` (dedicated/shared
 *   thread), §12 dynamic upgrade and §13 affinity and load balancing (SMP, v2b)
 *   => setting THREAD **does not error but is ignored**, leaving a trace (§3.4); that
 *   comment is unchanged. v1 coop/preempt both have **no threaded IRQ**.
 *   * Global constraint (3-02 §1.3): **core contains no controller model names**. No
 *   wording like "gic"/"pl011" appears in this header - model differences live entirely
 *   in the ISA layer (the br_pic.h fill-in tables).
 */
#ifndef BR_CORE_BR_IRQ_H
#define BR_CORE_BR_IRQ_H

#include <br/core/br_types.h>

/* =====================================================================
 * 1. Compile-time constants (3-02 §14.6: all trimmed by manifest/platform
 * config, no runtime negotiation)
 * ===================================================================== */

/* virq space layout (3-02 §3.2):
 *   0 .. BR_IRQ_NR_PIC-1         direct PIC lines (§3.2)
 *   BR_IRQ_IPI_BASE .. +N_IPI-1  IPI window (reserved for v2b; unused in v1)
 *   BR_IRQ_DOMAIN_BASE .. MAX-1  cascade domain sub-IRQ windows (contiguous
 *                                slices per domain)
 */
#define BR_IRQ_MAX              64u
#define BR_IRQ_IPI_BASE         48u
#define BR_IRQ_NR_IPI            8u
#define BR_IRQ_DOMAIN_BASE      56u
#define BR_IRQ_DOMAIN_MAX        2u   /* domain pool capacity */
#define BR_IRQ_DOMAIN_WINDOW    (BR_IRQ_MAX - BR_IRQ_DOMAIN_BASE)

/* Invalid virq (public signatures use uint32 to express "invalid"; **never
 * written into internal fields**) */
#define BR_IRQ_INVALID          0xFFFFFFFFu

/* Per-demux round cap (domain-level storm protection, 3-02 §9.3) */
#define BR_IRQ_DEMUX_MAX_ROUND  8u

/* Interrupt storm protection (3-02 §8.3: windowed criterion, frequency
 * independent, ratio only) */
#define BR_IRQ_STORM_LIMIT      64u
#define BR_IRQ_STORM_WINDOW     256u
#define BR_IRQ_STORM_GUARD      0     /* 1 = auto-mask on limit (default off: observable persistent error beats silent failure) */

/* Per-line counters switch (3-02 §14.6: slots always exist, the switch only
 * decides whether count is maintained - no RAM change) */
#define BR_IRQ_STATS            1

/* =====================================================================
 * 2. Priority / trigger / attributes (3-02 §7.1 / §8.1 / §3.4)
 * ===================================================================== */

/* Logical priority: **smaller value = higher** (GIC/NVIC convention);
 * interpretation belongs to platform */
#define BR_IRQ_PRIO_HIGHEST   0x00u
#define BR_IRQ_PRIO_LOWEST    0xFFu
#define BR_IRQ_PRIO_DEFAULT   0x80u

/* Trigger encoding (3-02 §8.1; platform interprets, core only passes through) */
#define BR_IRQ_TRIG_LEVEL_HIGH 0u
#define BR_IRQ_TRIG_LEVEL_LOW  1u
#define BR_IRQ_TRIG_EDGE_RISE  2u
#define BR_IRQ_TRIG_EDGE_FALL  3u
#define BR_IRQ_TRIG_EDGE_BOTH  4u
#define BR_IRQ_TRIG_DEFAULT    0xFFu   /* use the binding table's static value (board info, owned by platform) */

/* `br_irq_attr_t.flags` (3-02 §3.4; append-only, D14)
 * * Unknown bits set => register returns `-EINVAL` (**deliberately not silently
 *   ignored**: silent ignore would let a plugin built against a new header quietly
 *   lose the attributes it requested on an old core). */
#define BR_IRQ_F_SHARED          0x0001u  /* shared line = not a v1 target => -ENOTSUP */
#define BR_IRQ_F_PERCPU          0x0002u  /* per-core private (PPI); mismatch with hardware => -EINVAL */
#define BR_IRQ_F_DISPATCH_BH     0x0004u  /* dispatch this line to bh (bottom half). * Since
                                           * ADR-0011 this **really takes effect**: the ISR does
                                           * not run in interrupt context but is deferred to the
                                           * workqueue (bh); the hardware line stays masked while
                                           * it is handled and is released when the handler
                                           * returns (§12.4/§9.4 skeleton) */
#define BR_IRQ_F_DISPATCH_THREAD 0x0008u  /* Stage 2: dispatch to a dedicated/shared thread;
                                           * **still unimplemented** => setting it does not error
                                           * but is ignored + trace (§3.4) */
#define BR_IRQ_F_MIGRATABLE      0x0010u  /* v2b: allow runtime migration */
#define BR_IRQ_F_STATS           0x0020u  /* enable statistics on this line (needs BR_IRQ_STATS) */
#define BR_IRQ_F_NO_STORM_GUARD  0x0040u  /* exempt this line from storm protection (known-legal high-rate line) */
#define BR_IRQ_F_KNOWN_MASK      0x007Fu  /* v1 known-bit set */

typedef struct {
    br_u8  prio;      /* BR_IRQ_PRIO_*; beyond controller resolution => saturate to low priority + trace */
    br_u8  trigger;   /* BR_IRQ_TRIG_* */
    br_u32 flags;     /* BR_IRQ_F_* */
} br_irq_attr_t;

/* =====================================================================
 * 3. native API (9 entries; 3-01 §8/§8.1 frozen signatures, 3-02 §14.1 semantics)
 * =====================================================================
 * Exact meaning of thread-only (3-02 §14.1 final note): it is a **context
 * prohibition** ("must not be called from an ISR"), **not a requirement that
 * thread objects already exist** - M0's main context and plugin init phases are
 * themselves thread contexts.
 *
 * Direct line <-> domain member are **mutually exclusive** (3-02 §14.1): calling
 * a direct API on a domain member (or vice versa) is always `-EINVAL`, and it is
 * reported at `register` time (not only when disable fails).
 */

/* Bind an ISR to a virq: validate the binding table -> fill the descriptor ->
 * set prio/trigger (via pic_ops, interrupts off) -> **do not enable**
 * (depth = 1, hardware already masked). */
int br_irq_register(br_u32 irq, void (*isr)(void *), void *arg,
                    const br_irq_attr_t *attr);

/* Only when depth reaches 0 does pic_ops.unmask run; already released => idempotent 0. */
int br_irq_enable(br_u32 irq);

/* depth++; on 0->1 call pic_ops.mask (order: **L2 first, then L1**, 3-02 §6.5). */
int br_irq_disable(br_u32 irq);

/* ---- L2: CPU-local masking (ISR-safe, one of the four whitelist entries, CA-3) ---- */

/* `br_irq_state_t` bit layout **goes into golden** (3-02 §6.4); unused bits stay 0. */
typedef br_u32 br_irq_state_t;

#define BR_IRQ_ST_I_MASK       0x00000001u  /* [0]     PSTATE.I on entry */
#define BR_IRQ_ST_F_MASK       0x00000002u  /* [1]     PSTATE.F on entry (not managed in v1) */
#define BR_IRQ_ST_A_MASK       0x00000004u  /* [2]     PSTATE.A on entry (not managed in v1) */
#define BR_IRQ_ST_D_MASK       0x00000008u  /* [3]     PSTATE.D on entry (not managed in v1) */
#define BR_IRQ_ST_DEPTH_SHIFT  4
#define BR_IRQ_ST_DEPTH_MASK   0x0003FFF0u  /* [17:4]  irq_depth on entry (14 bits, debug check) */

/* Save DAIF -> disable local IRQ -> irq_depth++ -> return the token. */
br_irq_state_t br_irq_lock(void);

/* irq_depth--; restore PSTATE.I only when it reaches zero **and** the I bit was
 * 0 on entry.
 * * Underflow guard (3-02 §6.4): when depth is already 0, **never** `--`
 *   (it would wrap and disable interrupts forever). */
void br_irq_unlock(br_irq_state_t st);

/* =====================================================================
 * 4. Cascaded interrupt domains (3-01 §8.1 frozen signatures; 3-02 §9)
 * =====================================================================
 * v1 is single-level cascade; nested domains and shared lines are non-targets.
 * **The parent line must be a direct PIC line** (precondition for the SLOW domain
 * masking the parent inside the ISR, 3-02 §9.4).
 * * After ADR-0011 **both FAST and SLOW are usable**: a SLOW domain's demux runs
 *   on the core workqueue (`br/core/br_work.h`), the parent line is masked before
 *   submission and released after demux completes (§9.4). Only when "bh is
 *   unavailable" (core provides no workqueue) does
 *   `br_irq_domain_create(..., F_SLOW, ...)` return NULL - in the v0.2.0 core bh
 *   always exists, so SLOW is always available.
 */

typedef struct br_irq_domain br_irq_domain_t;

typedef struct br_irq_domain_ops {
    /* Pending query: core provides the bitmap buffer (capacity = ceil(n_sub/32)
     * words); the driver fills the currently pending bits.
     * * Two-phase registration (3-02 §9.2): if such a sub-IRQ occurs after domain
     *   creation but before its handler is registered, core **masks that sub-IRQ**
     *   (no "repeatedly firing with no owner" storm hole). */
    void (*pending)(void *priv, br_u32 *bits, br_size_t nwords);
    void (*mask)  (void *priv, br_u32 sub);   /* mask sub-IRQ */
    void (*unmask)(void *priv, br_u32 sub);
    void (*ack)   (void *priv, br_u32 sub);   /* * core calls this **after** the sub handler returns */
} br_irq_domain_ops_t;

#define BR_IRQ_DOMAIN_F_FAST 0x0u   /* status register is memory-mapped => demux + sub handler in ISR context */
#define BR_IRQ_DOMAIN_F_SLOW 0x1u   /* status read needs a bus transaction => demux in bh (bottom half, available since ADR-0011).
                                     * * The sub handler's context = the **bh contract** (stricter than the
                                     * designed "thread context": interrupts off, must not block). See the
                                     * br_work.h file header. */

/* Allocate a domain + sub-descriptor window + bitmap from the domain pool;
 * register the parent line's ISR = domain demux.
 * Returns NULL (pool full / parent line invalid or not direct / n_sub out of range
 * or window too small / ops->pending missing / SLOW requested but bh unavailable). */
br_irq_domain_t *br_irq_domain_create(const char *name, br_u32 parent_irq,
                                      br_u32 n_sub, br_u32 flags,
                                      const br_irq_domain_ops_t *ops, void *priv);

/* Fill the sub descriptor; on success the sub depth = 1 (masked, symmetric with
 * br_irq_register). */
int br_irq_register_child(br_irq_domain_t *dom, br_u32 sub,
                          void (*isr)(void *), void *arg, const br_irq_attr_t *attr);

int br_irq_enable_child (br_irq_domain_t *dom, br_u32 sub);
int br_irq_disable_child(br_irq_domain_t *dom, br_u32 sub);

/* =====================================================================
 * 5. Platform side: virq <-> hwirq binding table (3-02 §3.3; platform data,
 * compiled into .rodata)
 * ===================================================================== */

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

/* Platform side submits the binding table once (EARLY phase, exactly once;
 * repeat submission => -EBUSY).
 * core builds the hwirq->virq resolution table here (§14.2: v0.1 uses a "sorted
 * table + binary search", 4 B per binding - not a direct-index
 * `uint16_t virq_of[nr_irq]` table: GICv3's 1024 INTIDs would need 2 KB). */
int br_irq_bindings_set(const br_irq_binding_t *tbl, br_size_t n);

/* =====================================================================
 * 6. Cross-module observation contract (cannot be hidden: consumers are the debug
 * bridge / ramdump plugin, 3-02 §1.4)
 * ===================================================================== */

/* Diagnostic snapshot (3-02 §14.4; not a hot path, carries no golden layout promise)
 * * ADR-0011 appends three bh fields (append-only; the struct explicitly does not
 *   go into golden). */
typedef struct {
    br_u32 count;          /* entry count (BR_IRQ_STATS; always 0 when disabled) */
    br_u16 storm;          /* this line's count in the current window (read rt->storm directly, no separate peak) */
    br_u16 depth;          /* current masking depth */
    br_u32 spurious_owned; /* "arrived despite hardware masking" - a real bug signal (§4.2) */
    br_u32 spurious_nocap; /* "expected drop when masking is unavailable" - degraded behavior (§4.2) */
    br_u32 drop;           /* SLOW domain submission failures (domain members, §9.4) */
    br_u16 flags;          /* descriptor flags snapshot (* includes the DISPATCH_BH bit that really works since ADR-0011) */
    br_u8  prio, trigger;
    /* ---- bottom half (bh), three fields: per-core accumulation, v1 is single-core => global (ADR-0011 §3) ---- */
    br_u32 bh_deferred;        /* BH form: successful deferrals to the bottom half */
    br_u32 bh_dropped;         /* BH form: queue full => rolled-back unsubmitted count (IR-10) */
    br_u32 suppressed_dispatch;/* §11.4.1: already disabled before dispatch => handler suppressed count */
} br_irq_stat_t;

/* * `spurious_owned`/`spurious_nocap` are CPU-local (3-02 §14.4), so this call
 *   returns **this core's** counters (v1 single-core => global). If unavailable
 *   (virq out of range / no binding) => out is zeroed. */
void br_irq_stats_get(br_u32 virq, br_irq_stat_t *out);

/*
 * Execution context queries (ADR-0011; the runtime-observable face of the three
 * forms in §12.1).
 *
 * Why this is in a public header rather than a core-private one: a **domain dialect**
 * must be able to prove "my status register is not read inside an ISR" (the whole
 * reason the `SLOW` domain exists, §9.4), and that is code written on the plugin
 * (platform/driver) side - which cannot reach core-private headers. Same for
 * `br_work_in_bh()` (in `br_work.h`).
 */
br_bool br_irq_in_isr(void);        /* inside an ISR body (interrupt context) */
br_bool br_irq_in_atomic(void);     /* ISR **or** bh - the prohibition range of thread-only APIs */

/* =====================================================================
 * 7. core-internal symbols (3-02 §1.4/§14.2: asm stub calls and M0 stand-ins;
 * **not exported**)
 * ===================================================================== */

/*
 * CPU-local interrupt state initialization (BSS zero-init => no runtime
 * constructor; this points TPIDR_EL1 at this core's `br_irq_cpu_t` and sets the
 * descriptor pool's pic_id/dom_id to -1 - see P-IRQ-5).
 *
 * On the design side this step belongs to **core.init**, called by core's entry
 * `br_core_main()` in boot-chain step (1) (after BSS zeroing, before any C
 * subsystem).
 * * **Hard constraint**: it must run before platform submits the PIC binding table
 *   (`br_irq_bindings_set`) - this function sets pic_id/dom_id of **all**
 *   descriptors to -1, so running after the binding table would wipe the bindings
 *   already in place. See step (1) of `core/src/main.c` and ADR-0008.
 */
void br_irq_cpu_init(void);

/*
 * Globally enable interrupts (release PSTATE.I only; F/A/D unchanged).
 * Design side = the step "after all plugin init" (main doc §6.2 / 3-02 §14.3 after
 * step 7), called by the plugin manager after the LATE phase and before the START
 * phase (see the phase driver in plugin_mgr.c).
 */
void br_irq_cpu_enable(void);

/* Interrupt entry (called by the asm stub, 3-02 §5.2): ack -> dispatch -> ISR -> single exit eoi. */
struct br_exc_frame;
void br_irq_enter(struct br_exc_frame *f);

/*
 * Current L2 critical-section nesting depth (ADR-0011 ruling P-1). A **core-internal
 * symbol** (likewise not in golden): when a new thread first enters, the scheduling
 * framework must decide "was I switched in from inside an L2 critical section" - if
 * so (depth > 0) someone yielded the CPU inside a critical section (an existing
 * prohibition), so leave the interrupt state **untouched**; if not (depth == 0) the
 * thread must run in the "interrupts released" form - required when the preempt
 * scheduler switches stacks at **IRQ exit** (where PSTATE.I == 1), otherwise the new
 * thread would run on with interrupts disabled.
 */
br_u32 br_irq_lock_depth(void);

#endif /* BR_CORE_BR_IRQ_H */
