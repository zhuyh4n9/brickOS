/*
 * service/trace — trace observation service (implementation)
 *
 * Design source: `docs/5-debug/5-01-debug.md`
 *   §1 trace plugin: fixed 16B events + static ring buffer + **zero strings on the hot path**
 *      ("the id→name map is exported with plugin metadata and decoded offline");
 *   §1 "drain routes: console dump on panic / bridge live stream / host tool decode"
 *      — this plugin is the consuming side of "dump";
 *   §1 the API section leaves event macros and id assignment to the manifest; this implementation
 *      uses a 32-slot hash table for **dynamic ids** (the declaration face is frozen, see
 *      include/br/debug/br_trace_svc.h).
 * Ownership ruling (ADR-0003): the 16B event **ring and ISR emit point live in core**
 *   (`br/core/br_trace.h` `br_trace_emit`); this plugin is its only consumer: drain, decode
 *   id→name, count, render lines. Reasons in the brick.toml header comment.
 *
 * ★ Name-table premise and risk (the declaration face says `const char *`, faithfully realized
 *   here): a slot **stores only the caller-supplied pointer, never copies the string** — the
 *   contract is "tag lifetime = image" (callers normally pass string literals or static arrays).
 *   If a caller passes a stack/heap temporary buffer the pointer dangles; this prototype has no
 *   plugin manager to enforce the premise, so call discipline carries it.
 *   Identity test uses **pointer equality**: the same literal in one translation unit is merged
 *   (-fmerge-constants), so repeated markers from the same tag site are idempotent; "same content
 *   but different pointer" means the caller gave two different buffers, identity is undecidable
 *   (and the lifetime premise above is broken) ⇒ -ENOSPC.
 *   **An address collision is not an error**: probe **linearly forward** from `hash % SLOTS` for
 *   an empty slot (wrapping, at most the whole table). Why not "collision is an error": failing on
 *   the first collision in a 32-slot table would let **who registers first** decide whether others
 *   can register — another plugin's marker could push this plugin's selftest slot out by hash and
 *   make the case fail innocently (tests depending on hash slots are a design smell). Linear
 *   probing degrades to at most 32 steps only on collision; addressing stays hash O(1), fully
 *   acceptable for a 32-slot table. See README "key rulings".
 *
 * ISR safety: every function in this file is **thread-only** (drain advances the consumer
 *   cursor). ISR/fault paths may only call core's `br_trace_emit` directly.
 */
#include <br/debug/br_trace_svc.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>

#include "trace_internal.h"    /* internal view for self-test (see that header; not used on the production path) */

/* =====================================================================
 * Constants and internal state
 * ===================================================================== */

/* FNV-1a 32-bit: pairs with "zero strings on the hot path" in 5-01 §1 — the event carries only a
 * 32-bit hash; names stay in this table for online rendering or offline decode with metadata. */
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

/* =====================================================================
 * Small helpers (no libc)
 * ===================================================================== */

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

/* =====================================================================
 * Name registration / decode
 * ===================================================================== */

/*
 * Acquire a slot: probe **linearly forward** from `hash % SLOTS` (wrapping, at most SLOTS steps).
 *   - empty slot ⇒ claim it (store only the pointer, no string copy) and return the slot index;
 *   - hit the same pointer ⇒ idempotent, return the existing slot;
 *   - hit **same content but different pointer** ⇒ -ENOSPC (the caller gave two different buffers,
 *     identity is undecidable, and the "tag lifetime = image" premise is broken);
 *   - no empty slot after scanning the whole table ⇒ -ENOSPC.
 * ★ Why not "collision ⇒ -ENOSPC": failing on the first collision in a 32-slot table would let
 *   **who registers first** decide whether others can register — another plugin's marker could
 *   push this plugin's selftest slot out by hash and make the case fail innocently (tests
 *   depending on hash slots are a design smell). Linear probing degrades to at most 32 steps only
 *   on collision; addressing stays hash O(1), fully acceptable for a 32-slot table.
 * Returns the slot index (0..SLOTS-1) or a negative errno.
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

/* =====================================================================
 * Consumption counts
 * ===================================================================== */

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

/* =====================================================================
 * Drain + render
 * ===================================================================== */

br_u32 br_trace_svc_report(br_u32 max_events)
{
    /* max_events == 0 = "unlimited", but a hard bound is required, else a single call may never
     * converge while ISRs keep refilling during consumption — bound by ring capacity (repeated
     * calls drain it fully). */
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

    /* "Other" bucket: the remainder that can no longer keep id granularity once the per-id count
     * table (64 entries) is full, shown here so summary's drained does not look inconsistent with
     * the per-id counts. */
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
    /* ★ does not touch core ring cursors, nor clear the dynamic name table (registration is image-level, unrelated to counts) */
}

int br_trace_svc_init(void)
{
    br_trace_svc_reset();

    /* This plugin's two own event names. The start slot comes from the hash (trace.report→26,
     * trace.reset→31); the actual landing spot comes from linear probing. v0.1 only registers the
     * names and emits no events with these ids (traces still go through marker/core fixed
     * events), so offline decoding can recognize them.
     * Note: with linear probing, another plugin's collision no longer pushes selftest's slot 2
     * out — -ENOSPC appears only when all 32 slots are occupied (contract change, see the
     * trc_acquire_slot comment). */
    (void)br_trace_svc_register(TRC_NAME_REPORT);
    (void)br_trace_svc_register(TRC_NAME_RESET);

    return 0;
}

/* =====================================================================
 * Internal view for self-test
 *
 * See `src/trace_internal.h`: these two exist **only** for `trace_selftest.c` — the cases must
 * re-check the "collision premise" and "decoded name byte-for-byte equal", which the public API
 * cannot express. ★ Always **forward** to the production implementation, never copy logic: the
 * case must run the real algorithm, or it verifies a copy.
 * Prototypes are in that header (-Wmissing-prototypes); the entry names are not this plugin's
 * external API.
 * ===================================================================== */

br_u32 br_trace_internal_slot_of(const char *name)
{
    return trc_slot_of(name);
}

br_bool br_trace_internal_streq(const char *a, const char *b)
{
    return trc_streq(a, b);
}


/* =====================================================================
 * Plugin lifecycle hooks (name = symbol_prefix(short) + phase; referenced by the generated
 * `build/gen/service/trace/plugin_desc.c`; design 1-01 §9 / 3-05 §2)
 *
 * Self-declared prototypes satisfy -Wmissing-prototypes: hook names are derived by the
 * **generator** (not this plugin's external API), so they are not written into
 * `include/br/debug/br_trace_svc.h` nor into brick.toml [[export]] (that would change the
 * interface hash).
 * ===================================================================== */
int trace_early_init(void);
int trace_init(void);
int trace_start(void);

/* EARLY phase: no action (no heap/threads/IRQ-off; nothing to register here). */
int trace_early_init(void)
{
    return 0;
}

/* LATE phase (Service category ⇒ completion point ②): forward to the existing init. */
int trace_init(void)
{
    return br_trace_svc_init();
}

/* START phase: this service has nothing to do after "IRQs available / threads creatable". */
int trace_start(void)
{
    return 0;
}

