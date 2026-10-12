/*
 * service/memleak — leak / heap corruption reporting service (implementation)
 *
 * Design source: `docs/5-debug/5-01-debug.md` §4 "ASan / memleak policy (layered, honest engineering)":
 *   - the cheap v1.x substitute = TLSF red zones (malloc header/footer guard) + freelist poisoning +
 *     stack canary;
 *   - the v2.0 target memleak = per-plugin arena accounting (allocation carries plugin ownership;
 *     on shutdown/dump produce a per-plugin leak report).
 *   This file implements the **intersection** of those two lines: core's cheap mechanism is already in
 *   place (violation counters come from core), while the v2 arena is only carried as far as an
 *   "ownership tag" in this prototype (see `br/core/br_mem.h` §③ accounting contract).
 *
 * ★ Honest statement (consistent with the `br_memleak.h` header, ADR-0003):
 *   An ownership tag ≠ a v2 arena — no budget cap, no enforced ownership, no OOM policy.
 *   This plugin can answer "which blocks were not returned / who tagged them / where they were
 *   allocated", but not "some plugin is over budget".
 *   WORKAROUND(br-wa-debug-001) registers exactly that gap and its repayment action (v2 arena).
 *
 * ★ Read-only boundary (ADR-0003 §2.6):
 *   This plugin **only consumes** core's heap observation contract — `br_heap_stats_get` /
 *   `br_heap_walk` / `br_heap_check` / `br_heap_usage` / `br_heap_owner_*`; it builds no pool, changes
 *   no heap, and allocates nothing on the report path. The consumer is a plugin ⇒ the observation
 *   surface must live in core's public header, not hidden inside core (same treatment as
 *   `br_irq_stats_get` in 3-02 §17.4).
 *
 * ★ Static budget (constraint of 5-01 §3: "the capture path uses only static buffers, touching no heap
 *   or scheduler"):
 *   Ownership aggregation uses a **fixed 16-slot** static array; once full, further owners merge into
 *   one `"(other)"` overflow slot (the summary's owners count includes that slot as one owner).
 *   Detail rows are **not cached during the report**: each owner re-walks `br_heap_walk` to keep a
 *   zero-row buffer (printing at most BR_MEMLEAK_MAX_ROWS_PER_OWNER rows each time). This is the
 *   ruling "trade time for static RAM" — the report is a debug path and a heap walk is not cheap, but
 *   a 4 KiB plugin budget cannot hold a block table of 16 owners × 8 details.
 *
 * Threading discipline: everything is **thread-only** (it prints and walks the heap).
 */
#include <br/debug/br_memleak.h>

#include <br/core/br_log.h>

/* =====================================================================
 * Ownership tag (consumer side of the ③ accounting contract)
 * ===================================================================== */

/*
 * This plugin's own ownership tag. v0.1 has no runtime plugin manager (br-wa-boot-001), so
 * `br_memleak_init()` is not guaranteed to have been called; we use lazy registration ("register on
 * first need + cache on success"), which both satisfies init semantics (idempotent) and lets
 * self-test/report work in combinations that never init explicitly.
 * A failed registration (heap not ready / tag table full) is not cached; the next call retries —
 * core's register is idempotent for duplicate names.
 */
static br_owner_t s_owner      = BR_OWNER_NONE;
static br_bool    s_owner_done = BR_FALSE;

static br_owner_t ml_ensure_owner(void)
{
    if (!s_owner_done) {
        s_owner = br_heap_owner_register("memleak");
        if (s_owner != BR_OWNER_NONE) {
            s_owner_done = BR_TRUE;
        }
    }
    return s_owner;
}

br_owner_t br_memleak_owner(void)
{
    return ml_ensure_owner();
}

/*
 * Stance of 5-01 §4: only the ownership tag (base accounting) is registered here.
 * ★ The header also mentions "trace event names", but memleak's brick.toml does **not** declare a
 *   `service/trace` dependency edge ⇒ do not create an undeclared call edge here (that is exactly
 *   what the single-source-of-truth declaration surface must stop). Event names wait until that
 *   dependency edge is declared, see "to be re-imported" in the README.
 */
int br_memleak_init(void)
{
    (void)ml_ensure_owner();
    return 0;
}

/* =====================================================================
 * Live block observation (three consumers of br_heap_walk)
 * ===================================================================== */

static int ml_count_cb(void *ctx, const br_heap_block_t *blk)
{
    (void)blk;
    (*(br_u32 *)ctx)++;
    return 0;
}

br_u32 br_memleak_live(void)
{
    br_u32 n = 0u;

    /* Heap not ready ⇒ br_heap_walk returns -ENODEV; semantically "no live blocks" */
    if (br_heap_walk(ml_count_cb, &n) < 0) {
        return 0u;
    }
    return n;
}

typedef struct {
    br_owner_t owner;
    br_u32     live;
} ml_owner_count_t;

static int ml_count_owner_cb(void *ctx, const br_heap_block_t *blk)
{
    ml_owner_count_t *c = (ml_owner_count_t *)ctx;

    if (blk->owner == c->owner) {
        c->live++;
    }
    return 0;
}

br_u32 br_memleak_live_by_owner(br_owner_t owner)
{
    ml_owner_count_t c = { owner, 0u };

    if (br_heap_walk(ml_count_owner_cb, &c) < 0) {
        return 0u;
    }
    return c.live;
}

/*
 * ★ This is the **current state**, not a cumulative total. The violations of 5-01 §4
 *   (red zone/magic/poisoning) live in core's block headers and free list, and every
 *   `br_heap_check()` call rescans the whole heap ⇒ "how many violations remain right now".
 *   Cumulative figures (redzone_hits / canary_hits / double_free) live in `br_heap_stats_t` and are
 *   taken separately, outside `br_memleak_report()`'s summary; this function deliberately does not
 *   add the two, so that "a corruption that was repaired" is not recorded as red forever — case
 *   TC-DBG-041 depends on this semantics (after restoring a red zone it must return to 0).
 */
br_u32 br_memleak_corruptions(void)
{
    return br_heap_check();
}

/* =====================================================================
 * Report: owner aggregation (static bound + "(other)" overflow slot)
 * ===================================================================== */

#define ML_OWNER_SLOTS   16u   /* per-owner bound: static buffer discipline of 5-01 §3 */
#define ML_OTHER_NAME    "(other)"

typedef struct {
    br_bool    used;
    br_owner_t owner;
    br_u32     live;
    br_size_t  bytes;
} ml_slot_t;

static ml_slot_t s_slots[ML_OWNER_SLOTS];
static br_u32    s_slot_count;      /* occupied slot count (<= ML_OWNER_SLOTS) */

static br_bool   s_other_used;      /* whether the overflow slot was used */
static br_u32    s_other_live;
static br_size_t s_other_bytes;

static br_u32    s_sum_live;        /* global live blocks/bytes of this walk */
static br_size_t s_sum_bytes;

static void ml_reset(void)
{
    for (br_u32 i = 0u; i < ML_OWNER_SLOTS; i++) {
        s_slots[i].used  = BR_FALSE;
        s_slots[i].owner = BR_OWNER_NONE;
        s_slots[i].live  = 0u;
        s_slots[i].bytes = 0u;
    }
    s_slot_count  = 0u;
    s_other_used  = BR_FALSE;
    s_other_live  = 0u;
    s_other_bytes = 0u;
    s_sum_live    = 0u;
    s_sum_bytes   = 0u;
}

/* Find an owner in the slot table; not registered ⇒ -1 (no distinction between "overflow" and
 * "never seen" — both merge into "(other)") */
static int ml_slot_find(br_owner_t owner)
{
    for (br_u32 i = 0u; i < s_slot_count; i++) {
        if (s_slots[i].owner == owner) {
            return (int)i;
        }
    }
    return -1;
}

/* Take (creating if needed) an owner slot; table full ⇒ BR_NULL (caller merges into "(other)") */
static ml_slot_t *ml_slot_take(br_owner_t owner)
{
    const int idx = ml_slot_find(owner);

    if (idx >= 0) {
        return &s_slots[idx];
    }
    if (s_slot_count < ML_OWNER_SLOTS) {
        ml_slot_t *slot = &s_slots[s_slot_count];
        slot->used  = BR_TRUE;
        slot->owner = owner;
        slot->live  = 0u;
        slot->bytes = 0u;
        s_slot_count++;
        return slot;
    }
    return BR_NULL;
}

static int ml_aggr_cb(void *ctx, const br_heap_block_t *blk)
{
    ml_slot_t *slot;

    (void)ctx;
    slot = ml_slot_take(blk->owner);
    if (slot != BR_NULL) {
        slot->live++;
        slot->bytes += blk->size;
    } else {
        s_other_used = BR_TRUE;
        s_other_live++;
        s_other_bytes += blk->size;
    }
    s_sum_live++;
    s_sum_bytes += blk->size;
    return 0;
}

/* summary's owners figure: number of owners with live blocks (including the "(other)" pseudo-owner) */
static br_u32 ml_owner_total(void)
{
    br_u32 n = 0u;

    for (br_u32 i = 0u; i < s_slot_count; i++) {
        if (s_slots[i].used && s_slots[i].live > 0u) {
            n++;
        }
    }
    if (s_other_used && s_other_live > 0u) {
        n++;
    }
    return n;
}

typedef struct {
    br_owner_t owner;
    br_bool    other;    /* matches owners "not in the slot table" */
    br_u32     rows;
} ml_detail_ctx_t;

static int ml_detail_cb(void *ctx, const br_heap_block_t *blk)
{
    ml_detail_ctx_t *c = (ml_detail_ctx_t *)ctx;
    br_bool          match;

    if (c->rows >= BR_MEMLEAK_MAX_ROWS_PER_OWNER) {
        return 0;   /* rows full: keep walking but print nothing (the excess only shows in the owner row count) */
    }
    if (c->other) {
        match = (ml_slot_find(blk->owner) < 0) ? BR_TRUE : BR_FALSE;
    } else {
        match = (blk->owner == c->owner) ? BR_TRUE : BR_FALSE;
    }
    if (!match) {
        return 0;
    }

    br_log_info("[LEAK]   blk seq=%u addr=0x%lx size=%lu caller=0x%lx",
                blk->seq, (br_uintptr_t)blk->addr, blk->size, blk->caller);
    c->rows++;
    return 0;
}

br_u32 br_memleak_report(void)
{
    br_heap_stats_t st;
    br_u32          owners;
    br_u32          corrupt;

    /* First gate: if the heap is not ready leave one line only, do not pretend there is accounting */
    if (br_heap_stats_get(&st) != 0) {
        br_log_info("[LEAK] heap not ready");
        return 0u;
    }

    ml_reset();
    if (br_heap_walk(ml_aggr_cb, BR_NULL) < 0) {
        br_log_info("[LEAK] heap not ready");
        return 0u;
    }

    owners  = ml_owner_total();
    corrupt = br_memleak_corruptions();

    br_log_info("[LEAK] summary live=%u bytes=%lu owners=%u corrupt=%u high_water=%lu",
                s_sum_live, s_sum_bytes, owners, corrupt, st.high_water);

    /* Owner summary line + at most BR_MEMLEAK_MAX_ROWS_PER_OWNER detail rows per owner */
    for (br_u32 i = 0u; i < s_slot_count; i++) {
        ml_detail_ctx_t c;

        if (!s_slots[i].used || s_slots[i].live == 0u) {
            continue;
        }
        br_log_info("[LEAK] owner=%s live=%u bytes=%lu",
                    br_heap_owner_name(s_slots[i].owner),
                    s_slots[i].live, s_slots[i].bytes);
        c.owner = s_slots[i].owner;
        c.other = BR_FALSE;
        c.rows  = 0u;
        (void)br_heap_walk(ml_detail_cb, &c);
    }

    if (s_other_used && s_other_live > 0u) {
        ml_detail_ctx_t c;

        br_log_info("[LEAK] owner=%s live=%u bytes=%lu",
                    ML_OTHER_NAME, s_other_live, s_other_bytes);
        c.owner = BR_OWNER_NONE;   /* the other branch matches by "not in the slot table"; owner is unused */
        c.other = BR_TRUE;
        c.rows  = 0u;
        (void)br_heap_walk(ml_detail_cb, &c);
    }

    return s_sum_live;
}

/* =====================================================================
 * Plugin lifecycle hooks (name = symbol_prefix(short) + phase; referenced by the generated
 * `build/gen/service/memleak/plugin_desc.c`; design 1-01 §9 / 3-05 §2)
 * Self-declared prototypes satisfy -Wmissing-prototypes (hook names are derived by the generator,
 * not in the public header or [[export]]).
 * ===================================================================== */
int memleak_early_init(void);
int memleak_init(void);
int memleak_start(void);

/* EARLY phase: no action (no heap/threads/interrupt disabling — although this plugin "watches the
 * heap", the heap is not claimed yet at this point). */
int memleak_early_init(void)
{
    return 0;
}

/* LATE phase (Service category ⇒ ② completion point): forward to the existing init. */
int memleak_init(void)
{
    return br_memleak_init();
}

/* START phase: this service has nothing to do after "interrupts available / threads creatable". */
int memleak_start(void)
{
    return 0;
}

