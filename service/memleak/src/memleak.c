/*
 * Leak / heap corruption reporting implementation. Implements the intersection of the cheap v1.x
 * substitute (core's red zones / poisoning / double-free counters) and the v2 arena, which
 * only reaches ownership tags: no budget cap, no enforced ownership, no OOM policy.
 * WORKAROUND(br-wa-debug-001) registers that gap and its repayment action (v2 arena).
 *
 * Read-only: consumes only core's heap observation contract (br_heap_stats_get / br_heap_walk /
 * br_heap_check / br_heap_usage / br_heap_owner_*) and allocates nothing on the report path.
 *
 * Static budget: ownership aggregation uses a fixed 16-slot array; once full, further owners merge
 * into one "(other)" overflow slot (counted as one owner). Detail rows are not cached: each owner
 * re-walks br_heap_walk and prints at most BR_MEMLEAK_MAX_ROWS_PER_OWNER rows -- trading time for
 * static RAM, since a 4 KiB plugin budget cannot hold 16 owners x 8 details.
 *
 * Threading: thread-only throughout (prints and walks the heap).
 */
#include <br/debug/br_memleak.h>

#include <br/core/br_log.h>

/*
 * This plugin's own ownership tag. br_memleak_init() is not guaranteed to have run, so registration
 * is lazy (register on first need, cache on success), which is also idempotent. Failed registration
 * (heap not ready / tag table full) is not cached; the next call retries.
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

/* Only the ownership tag is registered. memleak's brick.toml declares no service/trace dependency
 * edge, so no trace event names here (that would create an undeclared call edge). */
int br_memleak_init(void)
{
    (void)ml_ensure_owner();
    return 0;
}

static int ml_count_cb(void *ctx, const br_heap_block_t *blk)
{
    (void)blk;
    (*(br_u32 *)ctx)++;
    return 0;
}

br_u32 br_memleak_live(void)
{
    br_u32 n = 0u;

    /* Heap not ready (br_heap_walk -ENODEV) means no live blocks */
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

/* Current violation count, not cumulative: every br_heap_check() rescans the whole heap, so this is
 * "how many violations remain right now". Cumulative counters live in br_heap_stats_t and are
 * reported separately; the two are deliberately not added, so a repaired corruption returns to 0
 * (TC-DBG-041 depends on this). */
br_u32 br_memleak_corruptions(void)
{
    return br_heap_check();
}

/* Per-owner bound (static buffer discipline). */
#define ML_OWNER_SLOTS   16u
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

/* Not registered => -1; "overflow" and "never seen" both merge into "(other)". */
static int ml_slot_find(br_owner_t owner)
{
    for (br_u32 i = 0u; i < s_slot_count; i++) {
        if (s_slots[i].owner == owner) {
            return (int)i;
        }
    }
    return -1;
}

/* Take (creating if needed) an owner slot; table full => BR_NULL (caller merges into "(other)") */
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

/* summary's owners figure: owners with live blocks, including the "(other)" pseudo-owner */
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

    /* First gate: heap not ready => one line only, do not pretend there is accounting */
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

/* Plugin lifecycle hooks referenced by the generated plugin_desc.c. Self-declared prototypes
 * -Wmissing-prototypes (hook names come from the generator, not the public header). */
int memleak_early_init(void);
int memleak_init(void);
int memleak_start(void);

/* EARLY phase: no action; the heap is not claimed yet at this point. */
int memleak_early_init(void)
{
    return 0;
}

int memleak_init(void)
{
    return br_memleak_init();
}

int memleak_start(void)
{
    return 0;
}
