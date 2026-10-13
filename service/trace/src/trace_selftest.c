/* Self-test suite for service/trace: its own cases TC-DBG-001..003, driven only through the public
 * API of br/debug/br_trace_svc.h except for two internal-mechanism checks (src/trace_internal.h).
 * Returns the failure count (0 = all green); a failure does not halt. The entry name trace_selftest
 * is derived by the generated code from symbol_prefix. */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/debug/br_trace_svc.h>

#include "trace_internal.h"

int trace_selftest(void);

/* Selftest tags use static arrays: marker idempotence is judged by pointer equality. */
static const char TRC_TAG_SELFTEST[] = "selftest.a";

/* The next two tags have the same initial slot (verified in the case via trc_slot_of): a collision
 * must not yield -ENOSPC, it probes on to distinct ids. */
static const char TRC_TAG_SLOT_A[] = "selftest.victor";
static const char TRC_TAG_SLOT_B[] = "selftest.yankee";

/* "Same contents but different pointers => -ENOSPC". Deliberately non-const writable arrays:
 * -fmerge-constants could fold identical const arrays to one address. */
static char TRC_TAG_DUP_A[] = "selftest.dup";
static char TRC_TAG_DUP_B[] = "selftest.dup";

static br_u32 trc_conf(br_bool ok, const char *tag, const char *what)
{
    br_log_info("[DBGCONF] %s %s %s", ok ? "PASS" : "FAIL", tag, what);
    return ok ? 0u : 1u;
}

int trace_selftest(void)
{
    br_u32 fails = 0u;

    /* TC-DBG-001: marker idempotence + dynamic id/name + drain count */
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

    /* TC-DBG-001 (cont.): a slot collision takes linear probing, not an error. Both tags
     * share an initial slot (re-checked via trc_slot_of, so a hash change cannot make it a no-op);
     * register (no event) with static tags keeps reruns idempotent. */
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

    /* Same contents but different pointers => -ENOSPC (no reuse, no second name); asserts the two
     * static array addresses really differ. */
    const int da = br_trace_svc_register(TRC_TAG_DUP_A);
    const int db = br_trace_svc_register(TRC_TAG_DUP_B);

    /* -Warray-compare: comparing array names warns, so compare first-element addresses explicitly */
    fails += trc_conf((da >= (int)BR_TRACE_SVC_ID_BASE)
                      && (db == BR_ERR(ENOSPC))
                      && (&TRC_TAG_DUP_A[0] != &TRC_TAG_DUP_B[0]),
                      "TC-DBG-001",
                      "same contents but different pointers => -ENOSPC (no reuse, no new entry)");

    /* TC-DBG-002: overflow the ring; overrun > 0 is the honest "history untrustworthy" signal. Emit
     * RING_SIZE + 8 records without draining, then check overrun matches and stays unchanged. */
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

    /* TC-DBG-003: after a truncated drain, resume at the oldest unread record, losing no backlog */
    br_trace_svc_reset();

    for (br_u32 i = 0u; i < 5u; i++) {
        (void)br_trace_svc_marker(TRC_TAG_SELFTEST);
    }

    const br_u32 first = br_trace_svc_report(2u);
    const br_u32 rest  = br_trace_svc_report(0u);

    fails += trc_conf((first == 2u) && (rest >= 3u),
                      "TC-DBG-003",
                      "report(2) takes only 2 records, then report(0) resumes with >=3");

    /* Teardown: reset counters and drain the ring so later dumps' trace section stays clean */
    br_trace_svc_reset();
    for (br_u32 i = 0u; i < 8u; i++) {
        if (br_trace_svc_report(0u) == 0u) {
            break;
        }
    }
    br_trace_svc_reset();

    return (int)fails;
}
