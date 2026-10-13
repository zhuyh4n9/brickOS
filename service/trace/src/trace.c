/*
 * Trace observation implementation. The 16 B event ring and the ISR emit point live in core
 * (br/core/br_trace.h br_trace_emit); this plugin is the only consumer: drain, decode id->name,
 * count, render lines.
 *
 * Name table: a slot stores only the caller-supplied pointer, never a copy, so the contract is
 * "tag lifetime = image" (callers pass literals or static arrays); a stack/heap temporary would
 * dangle and nothing enforces the premise in this prototype. Identity is pointer equality (the same
 * literal in one translation unit is merged, so repeated markers are idempotent); equal content
 * behind another pointer is undecidable => -ENOSPC. An address collision is not an error:
 * probe linearly forward from hash % SLOTS for an empty slot (wrapping, at most the whole table);
 * failing on the first collision would let registration order decide who can register.
 *
 * ISR safety: every function in this file is thread-only (drain advances the consumer cursor);
 * ISR/fault paths may only call core's br_trace_emit.
 */
#include <br/debug/br_trace_svc.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>

#include "trace_internal.h"

/* FNV-1a 32-bit: an event carries only a 32-bit hash; names stay in this table for online rendering
 * or offline decode with metadata. */
#define TRC_FNV_OFFSET   2166136261u
#define TRC_FNV_PRIME    16777619u

/* Per-drain batch (fixed static footprint; 16B × 16 = 256B) */
#define TRC_DRAIN_BATCH  16u

/* "Seen" count table: 64-entry linear table; overflow past full goes into the s_other bucket (shown as a trailing summary line) */
#define TRC_SEEN_SLOTS   64u

/* Dynamic name table: stores only pointers in slots, owns no strings (premise in the file header) */
static const char *s_names[BR_TRACE_SVC_ID_SLOTS];

/* Per-id accumulated "seen" counts (linear search; observation and test criteria) */
static br_u32 s_seen_id[TRC_SEEN_SLOTS];
static br_u32 s_seen_cnt[TRC_SEEN_SLOTS];
static br_u32 s_seen_n;      /* number of distinct registered ids (= summary's ids) */
static br_u32 s_other;       /* entries folded into the "other" bucket after the count table filled */

/* Total entries this plugin has taken; also serves as the consumption sequence number in [TRACE] lines */
static br_u32 s_drained;

/* Drain batch buffer (single consumer in thread context, no locking needed) */
static br_trace_evt_t s_batch[TRC_DRAIN_BATCH];

/* This plugin's own event names (static arrays ⇒ pointer identity, init is repeatable) */
static const char TRC_NAME_REPORT[] = "trace.report";
static const char TRC_NAME_RESET[]  = "trace.reset";

/* FNV-1a 32-bit; byte-wise, so cast to br_u8 first (avoids char signedness affecting the result) */
static br_u32 trc_fnv1a32(const char *s)
{
    br_u32 h = TRC_FNV_OFFSET;

    for (const char *p = s; *p != '\0'; p++) {
        h ^= (br_u32)(br_u8)*p;
        h *= TRC_FNV_PRIME;
    }
    return h;
}

static br_u32 trc_slot_of(const char *name)
{
    return trc_fnv1a32(name) % BR_TRACE_SVC_ID_SLOTS;
}

static br_bool trc_streq(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (*a == *b);   /* equal only if both stopped at '\0' */
}

/*
 * Acquire a slot: probe linearly forward from hash % SLOTS (wrapping, at most SLOTS steps). Empty
 * slot => claim it (pointer only); same pointer => idempotent; same content but a different
 * pointer => -ENOSPC, identity is undecidable; no empty slot after a full scan => -ENOSPC.
 * A collision is not an error: failing on the first one would let registration order decide who can
 * register. Returns the slot index (0..SLOTS-1) or a negative errno.
 */
static int trc_acquire_slot(const char *name)
{
    if (name == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    const br_u32 start = trc_slot_of(name);

    for (br_u32 step = 0u; step < BR_TRACE_SVC_ID_SLOTS; step++) {
        const br_u32 slot = (start + step) % BR_TRACE_SVC_ID_SLOTS;

        if (s_names[slot] == BR_NULL) {
            s_names[slot] = name;          /* store pointer only, no copy */
            return (int)slot;
        }
        if (s_names[slot] == name) {       /* same pointer ⇒ idempotent */
            return (int)slot;
        }
        if (trc_streq(s_names[slot], name)) {
            return BR_ERR(ENOSPC);      /* same name (content) but different pointer */
        }
    }
    return BR_ERR(ENOSPC);              /* table full */
}

/* Slot index → event id (the single conversion point, shared by marker/register) */
static int trc_slot_to_id(int slot)
{
    return (slot < 0) ? slot : (int)(BR_TRACE_SVC_ID_BASE + (br_u32)slot);
}

int br_trace_svc_marker(const char *tag)
{
    const int slot = trc_acquire_slot(tag);

    if (slot < 0) {
        return slot;
    }

    const int id = trc_slot_to_id(slot);

    /* a = 32-bit hash of tag (key for offline/online decode), b = current clock (us) */
    br_trace_emit((br_u32)id, trc_fnv1a32(tag), (br_u64)br_clock_now());
    return id;
}

int br_trace_svc_register(const char *name)
{
    return trc_slot_to_id(trc_acquire_slot(name));
}

const char *br_trace_svc_name(br_u32 id)
{
    if ((id >= BR_TRACE_SVC_ID_BASE)
        && (id < (BR_TRACE_SVC_ID_BASE + BR_TRACE_SVC_ID_SLOTS))) {
        const char *n = s_names[id - BR_TRACE_SVC_ID_BASE];
        if (n != BR_NULL) {
            return n;
        }
    }
    return br_trace_name(id);   /* fall back to core's fixed event name table (unknown ⇒ "?") */
}

static void trc_seen_add(br_u32 id)
{
    for (br_u32 i = 0u; i < s_seen_n; i++) {
        if (s_seen_id[i] == id) {
            s_seen_cnt[i]++;
            return;
        }
    }
    if (s_seen_n < TRC_SEEN_SLOTS) {
        s_seen_id[s_seen_n]  = id;
        s_seen_cnt[s_seen_n] = 1u;
        s_seen_n++;
        return;
    }
    s_other++;   /* table full: only the total is preserved, not id granularity */
}

br_u32 br_trace_svc_seen(br_u32 id)
{
    for (br_u32 i = 0u; i < s_seen_n; i++) {
        if (s_seen_id[i] == id) {
            return s_seen_cnt[i];
        }
    }
    return 0u;
}

br_u32 br_trace_svc_report(br_u32 max_events)
{
    /* max_events == 0 means unlimited, but a hard bound is needed: ISRs may keep refilling while we
     * consume, so bound by ring capacity and let repeated calls drain it fully. */
    const br_u32 limit = (max_events == 0u) ? (br_u32)BR_TRACE_RING_SIZE : max_events;
    br_u32 taken = 0u;

    while (taken < limit) {
        br_u32 want = limit - taken;
        if (want > TRC_DRAIN_BATCH) {
            want = TRC_DRAIN_BATCH;
        }

        const br_u32 n = br_trace_drain(s_batch, want);
        if (n == 0u) {
            break;   /* ring drained */
        }

        for (br_u32 i = 0u; i < n; i++) {
            s_drained++;
            br_log_info("[TRACE] #%u id=%u name=%s a=0x%x b=0x%lx",
                        s_drained, s_batch[i].id,
                        br_trace_svc_name(s_batch[i].id),
                        s_batch[i].a, s_batch[i].b);
            trc_seen_add(s_batch[i].id);
        }
        taken += n;
    }

    return taken;
}

br_u32 br_trace_svc_summary(void)
{
    const br_u32 total   = br_trace_total();
    const br_u32 overrun = br_trace_overrun();
    /* live lower-bounded at 0: producer and consumer counts briefly disagree when the ring is overwritten */
    const br_u32 live    = (total >= s_drained) ? (total - s_drained) : 0u;

    br_log_info("[TRACE] total=%u drained=%u overrun=%u ids=%u live=%u",
                total, s_drained, overrun, s_seen_n, live);

    /* Remainder folded into "other" once the per-id table is full, keeping summary consistent */
    if (s_other > 0u) {
        br_log_info("[TRACE] other=%u (per-id count table full at %u entries, remainder folded into this bucket)",
                    s_other, TRC_SEEN_SLOTS);
    }

    return s_drained;
}

void br_trace_svc_reset(void)
{
    for (br_u32 i = 0u; i < TRC_SEEN_SLOTS; i++) {
        s_seen_id[i]  = 0u;
        s_seen_cnt[i] = 0u;
    }
    s_seen_n  = 0u;
    s_other   = 0u;
    s_drained = 0u;
    /* Does not touch the core ring cursors or the name table (registration is image-level). */
}

int br_trace_svc_init(void)
{
    br_trace_svc_reset();

    /* Register this plugin's two own names; v0.1 emits no events with these ids, the entries exist
     * for offline decode. With linear probing -ENOSPC appears only when all slots are occupied. */
    (void)br_trace_svc_register(TRC_NAME_REPORT);
    (void)br_trace_svc_register(TRC_NAME_RESET);

    return 0;
}

/* Accessors for trace_selftest.c only: the cases re-check the collision premise and decoded name
 * through the real algorithm, which the public API cannot express. They forward to production. */

br_u32 br_trace_internal_slot_of(const char *name)
{
    return trc_slot_of(name);
}

br_bool br_trace_internal_streq(const char *a, const char *b)
{
    return trc_streq(a, b);
}


/* Plugin lifecycle hooks referenced by the generated plugin_desc.c. Self-declared prototypes
 * -Wmissing-prototypes; the hook names come from the generator, not the public header. */
int trace_early_init(void);
int trace_init(void);
int trace_start(void);

/* EARLY phase: no action (nothing to register here). */
int trace_early_init(void)
{
    return 0;
}

int trace_init(void)
{
    return br_trace_svc_init();
}

int trace_start(void)
{
    return 0;
}
