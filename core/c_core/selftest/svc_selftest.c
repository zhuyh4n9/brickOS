/*
 * Service registry self-test suite: TC-SVC-001..003. The gate judges red/green by the
 * [SVCCONF] PASS/FAIL and [SVCCONF] SUMMARY lines. White-box access via
 * ../src/svc/svc_internal.h (see that header for why).
 * Returns the number of failures (0 = all green); never stops early.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_svc.h>
#include <br/core/br_types.h>

#include "../src/plugin/svc_internal.h"

int br_service_selftest(void);

/* Names used by cases all carry the `selftest.` prefix (removed from the table afterwards). */
#define SVC_T_A   "selftest.svc.a"
#define SVC_T_B   "selftest.svc.missing"

static br_u32 s_sc_pass;
static br_u32 s_sc_fail;

static void sc_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_sc_pass++;
        br_log_info("[SVCCONF] PASS %s %s", tag, what);
    } else {
        s_sc_fail++;
        br_log_info("[SVCCONF] FAIL %s %s", tag, what);
    }
}

int br_service_selftest(void)
{
    static const br_u32 ops_a = 0xA1B2C3D4u;   /* "ops" only needs to be a stable pointer */
    static const br_u32 ops_b = 0x5A5A1234u;

    s_sc_pass = 0u;
    s_sc_fail = 0u;

    br_log_info("[SVCCONF] service registry conformance (publish/lookup/error codes)");

    /* Defensive cleanup: same-named entries from a previous run (invocation is idempotent). */
    svc_remove(SVC_T_A);

    /* ---- TC-SVC-001: after publish, lookup returns the same pointer (count/name_at agree) ---- */
    {
        const br_u32 before = br_service_count();
        const int rp = br_service_publish(SVC_T_A, &ops_a);
        const void *got = br_service_lookup(SVC_T_A);
        const br_u32 after = br_service_count();

        br_bool ok = (rp == 0) && (got == (const void *)&ops_a) && (after == before + 1u);
        /* name_at/count agree with the table */
        ok = ok && (after > 0u);
        if (ok) {
            br_bool found = BR_FALSE;
            for (br_u32 i = 0u; i < after; i++) {
                if (name_equal(br_service_name_at(i), SVC_T_A)) {
                    found = BR_TRUE;
                    break;
                }
            }
            ok = found;
        }
        ok = ok && (br_service_lookup_hits(SVC_T_A) >= 1u);
        sc_report(ok, "TC-SVC-001", "publish -> lookup round trip: same pointer, count/name_at in sync, hit accounting");
    }

    /* ---- TC-SVC-002: duplicate-name publish => -EEXIST, original pointer not overwritten ---- */
    {
        const int rp2 = br_service_publish(SVC_T_A, &ops_b);
        const void *still = br_service_lookup(SVC_T_A);
        sc_report((rp2 == BR_ERR(EEXIST)) && (still == (const void *)&ops_a),
                  "TC-SVC-002", "duplicate publish => -EEXIST and original ops not overwritten");
    }

    /* ---- TC-SVC-003: unpublished name => BR_NULL; null name/ops => -EINVAL ---- */
    {
        const void *miss = br_service_lookup(SVC_T_B);
        const int   e_name = br_service_publish(BR_NULL, &ops_b);
        const int   e_ops  = br_service_publish(SVC_T_B, BR_NULL);
        sc_report((miss == BR_NULL) && (e_name == BR_ERR(EINVAL))
                  && (e_ops == BR_ERR(EINVAL)),
                  "TC-SVC-003", "lookup miss => NULL; empty name/empty ops => -EINVAL");
    }

    /* ---- Wrap-up: remove the case entries so later registry dumps stay clean ---- */
    svc_remove(SVC_T_A);
    svc_remove(SVC_T_B);

    br_log_info("[SVCCONF] SUMMARY pass=%u fail=%u total=%u",
                s_sc_pass, s_sc_fail, s_sc_pass + s_sc_fail);

    return (int)s_sc_fail;
}
