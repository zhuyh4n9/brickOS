/*
 * brickOS prototype v0.2.0 — service/trace selftest suite (service/trace/src/trace_selftest.c)
 *
 * Design basis and ruling: `docs/decisions/0010-plugin-selftest.md` (selftest separate from
 * production code). Cases (TC-DBG-001..003) taken verbatim from `trace.c`: case ids,
 * assertions and log strings unchanged — gate `tests/gates.toml` decides by `[DBGCONF] PASS/FAIL`.
 *
 * ## Boundary
 *   Driven only through the public API of `br/debug/br_trace_svc.h` (marker / register / name /
 *   report / seen / reset / summary), never core ring internals;
 *   only two exceptions: "two tags share an initial slot" and "names are byte-equal" need
 *   `trc_slot_of` / `trc_streq` from the production implementation. They are reached through the
 *   `src/trace_internal.h` accessors (no logic copied); see that header for the rationale.
 *
 * ## Contract with the plugin manager
 *   Returns the failure count (0 = all green); logs are emitted here (`[DBGCONF] PASS/FAIL ...`).
 *   Failure does not halt — the gate decides (`[DBGCONF] FAIL` is in tests/gates.toml forbid).
 *   The entry name `trace_selftest` is derived by the generated code from `symbol_prefix` (see
 *   `.selftest` in plugin_desc.c), not this plugin's public API.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/debug/br_trace_svc.h>

#include "trace_internal.h"

/* Entry prototype (declarations live in generated code; restated here for the global-function prototype rule). */
int trace_selftest(void);

/* ==================================================================== Test fixtures
 *
 * selftest tags use static arrays: marker idempotence is judged by pointer equality.
 */
static const char TRC_TAG_SELFTEST[] = "selftest.a";

/* Linear-probe check: the next two tags have the same initial slot (here = 16; verified in
 * the case via trc_slot_of), asserting a slot collision no longer yields -ENOSPC but probes on to distinct ids. */
static const char TRC_TAG_SLOT_A[] = "selftest.victor";
static const char TRC_TAG_SLOT_B[] = "selftest.yankee";

/* "Same contents but different pointers => -ENOSPC" check. Deliberately non-const writable
 * arrays: constant merging (-fmerge-constants) could fold identical const arrays to one
 * address, which would no longer exercise the "different pointers" branch. */
static char TRC_TAG_DUP_A[] = "selftest.dup";
static char TRC_TAG_DUP_B[] = "selftest.dup";

/* Record one DBGCONF check; returns 0/1 so failures can be summed */
static br_u32 trc_conf(br_bool ok, const char *tag, const char *what)
{
    br_log_info("[DBGCONF] %s %s %s", ok ? "PASS" : "FAIL", tag, what);
    return ok ? 0u : 1u;
}

/* ==================================================================== Cases */

int trace_selftest(void)
{
    br_u32 fails = 0u;

    /* ---- TC-DBG-001: marker idempotence + dynamic id/name + drain count ---- */
    br_trace_svc_reset();

    const int   id1 = br_trace_svc_marker(TRC_TAG_SELFTEST);
    const int   id2 = br_trace_svc_marker(TRC_TAG_SELFTEST);
    const char *nm  = (id1 >= 0) ? br_trace_svc_name((br_u32)id1) : BR_NULL;

    fails += trc_conf((id1 >= (int)BR_TRACE_SVC_ID_BASE) && (id1 == id2)
                      && (nm != BR_NULL) && br_trace_internal_streq(nm, TRC_TAG_SELFTEST),
                      "TC-DBG-001",
                      "two markers yield the same dynamic id (>= BASE) and name decodes");

    const br_u32 got1 = br_trace_svc_report(0u);
    fails += trc_conf((got1 >= 2u) && (id1 >= 0)
                      && (br_trace_svc_seen((br_u32)id1) >= 2u),
                      "TC-DBG-001",
                      "report(0) returns >=2 records and seen(id) >= 2");

    /* ---- TC-DBG-001 (cont.): slot collision takes linear probing, no longer "collision is an error" ----
     * Both tags share the same initial slot (verified in the case via trc_slot_of, so a hash
     * change cannot silently turn this assertion into a no-op); asserts both register, with distinct
     * ids and decodable names. Uses register (no event); tags are static arrays (stable pointers) => rerunning selftest stays idempotent. */
    const int   ca  = br_trace_svc_register(TRC_TAG_SLOT_A);
    const int   cb  = br_trace_svc_register(TRC_TAG_SLOT_B);
    const char *cna = (ca >= 0) ? br_trace_svc_name((br_u32)ca) : BR_NULL;
    const char *cnb = (cb >= 0) ? br_trace_svc_name((br_u32)cb) : BR_NULL;

    fails += trc_conf((br_trace_internal_slot_of(TRC_TAG_SLOT_A) == br_trace_internal_slot_of(TRC_TAG_SLOT_B))
                      && (ca >= (int)BR_TRACE_SVC_ID_BASE)
                      && (cb >= (int)BR_TRACE_SVC_ID_BASE)
                      && (ca != cb)
                      && (cna != BR_NULL) && br_trace_internal_streq(cna, TRC_TAG_SLOT_A)
                      && (cnb != BR_NULL) && br_trace_internal_streq(cnb, TRC_TAG_SLOT_B),
                      "TC-DBG-001",
                      "two tags with the same hash slot both register after linear probing (distinct ids, correct names)");

    /* Same contents but different pointers => -ENOSPC (identity undecidable: no reuse, no second name).
     * Also asserts the two static array addresses really differ (constant merging would surface here first). */
    const int da = br_trace_svc_register(TRC_TAG_DUP_A);
    const int db = br_trace_svc_register(TRC_TAG_DUP_B);

    /* -Warray-compare: comparing array names warns, so compare first-element addresses explicitly */
    fails += trc_conf((da >= (int)BR_TRACE_SVC_ID_BASE)
                      && (db == BR_ERR(ENOSPC))
                      && (&TRC_TAG_DUP_A[0] != &TRC_TAG_DUP_B[0]),
                      "TC-DBG-001",
                      "same contents but different pointers => -ENOSPC (no reuse, no new entry)");

    /* ---- TC-DBG-002: deliberately overflow the ring; overrun > 0 is the honest "this history is untrustworthy" signal ----
     * Emit RING_SIZE + 8 records without draining: the producer overwrites unread old slots, so overrun grows.
     * The overrun printed by summary comes straight from br_trace_overrun(), same source as the asserted
     * value => equality holds by construction; additionally verifies the reading is unchanged across the print (drain/summary do not touch the producer count). */
    const br_u32 orun_before = br_trace_overrun();

    for (br_u32 i = 0u; i < ((br_u32)BR_TRACE_RING_SIZE + 8u); i++) {
        (void)br_trace_svc_marker(TRC_TAG_SELFTEST);
    }

    /* Drain everything; bounded loop: an ISR may refill during draining, so never spin waiting for "empty" */
    for (br_u32 i = 0u; i < 8u; i++) {
        if (br_trace_svc_report(0u) == 0u) {
            break;
        }
    }

    const br_u32 orun_after = br_trace_overrun();
    (void)br_trace_svc_summary();
    const br_u32 orun_post = br_trace_overrun();

    fails += trc_conf((orun_after > 0u) && (orun_after >= orun_before)
                      && (orun_post == orun_after),
                      "TC-DBG-002",
                      "after overflowing the ring overrun > 0 and summary's overrun matches");

    /* ---- TC-DBG-003: after a truncated drain, resume at the oldest unread record without losing the backlog ---- */
    br_trace_svc_reset();

    for (br_u32 i = 0u; i < 5u; i++) {
        (void)br_trace_svc_marker(TRC_TAG_SELFTEST);
    }

    const br_u32 first = br_trace_svc_report(2u);
    const br_u32 rest  = br_trace_svc_report(0u);

    fails += trc_conf((first == 2u) && (rest >= 3u),
                      "TC-DBG-003",
                      "report(2) takes only 2 records, then report(0) resumes with >=3");

    /* ---- Teardown: reset counters and drain the ring so later dumps' trace section stays clean ---- */
    br_trace_svc_reset();
    for (br_u32 i = 0u; i < 8u; i++) {
        if (br_trace_svc_report(0u) == 0u) {
            break;
        }
    }
    br_trace_svc_reset();

    return (int)fails;
}
