/*
 * Self-test suite for provider/memleak: its own cases TC-DBG-040..043. All four exercise only the
 * public surface (br_memleak_live / live_by_owner / owner / report / corruptions, plus core's heap
 * observation contract in br_mem.h), so no *_internal.h is needed; the s_ml_pass/s_ml_fail and
 * ml_report/ml_case_* helpers are case-private and static here.
 *
 * Returns the failure count (0 = all green) and logs [DBGCONF] PASS/FAIL plus its SUMMARY;
 * a failure does not halt. The entry name memleak_selftest is derived by the generated code from
 * symbol_prefix; it is not this plugin's public API.
 */
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>
#include <br/core/br_types.h>
#include <br/debug/br_memleak.h>

int memleak_selftest(void);

static br_u32 s_ml_pass;
static br_u32 s_ml_fail;

static void ml_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_ml_pass++;
        br_log_info("[DBGCONF] PASS %s %s", tag, what);
    } else {
        s_ml_fail++;
        br_log_info("[DBGCONF] FAIL %s %s", tag, what);
    }
}

/* TC-DBG-040: a real leak is detectable. The pointer is deliberately dropped, so the criterion uses
 * only core's observation; it is still freed at the end, leaving no real leak behind. */
static void ml_case_040(void)
{
    const br_u32     live0 = br_memleak_live();
    const br_u32     mine0 = br_memleak_live_by_owner(br_memleak_owner());
    const br_owner_t prev  = br_heap_owner_set(br_memleak_owner());
    void            *leaked;
    br_u32           live1;
    br_u32           mine1;
    br_u32           rep;

    /* The owner tag must really be registered, else this case counts unowned blocks */
    ml_report(br_memleak_owner() != BR_OWNER_NONE, "TC-DBG-040",
              "owner tag registered (br_memleak_owner != NONE)");

    leaked = br_malloc(123u);   /* pointer deliberately not part of the criterion */

    live1 = br_memleak_live();
    mine1 = br_memleak_live_by_owner(br_memleak_owner());
    rep   = br_memleak_report();   /* dump the state, returning the live block count */

    (void)br_heap_owner_set(prev);

    ml_report(leaked != BR_NULL, "TC-DBG-040", "br_malloc(123) succeeded (pointer deliberately dropped)");
    ml_report(mine1 >= mine0 + 1u, "TC-DBG-040",
              "live blocks for this owner >= baseline+1 (br_memleak_live_by_owner)");
    /* report live is the printed figure; the delta can only come from this owner */
    ml_report((rep == live1) && ((live1 - live0) == (mine1 - mine0)), "TC-DBG-040",
              "report live matches observation and the delta has a single owner");

    br_free(leaked);   /* return the heap to a clean state */
}

/* TC-DBG-041: a red-zone overflow is detectable. Core's layout puts the red zone immediately after
 * the user area, so for a 64 B request the first overflow byte is p+64, filled with 0xA5. The case
 * restores that byte, otherwise later cases would see unreparable damage. */
static void ml_case_041(void)
{
    void           *p = br_malloc(64u);
    volatile br_u8 *rz;
    br_u32          bad;
    br_u32          fixed;

    if (p == BR_NULL) {
        ml_report(BR_FALSE, "TC-DBG-041", "br_malloc(64) failed, cannot build the red-zone scenario");
        return;
    }

    rz = (volatile br_u8 *)p;
    rz[64] = 0xFFu;               /* overflow by 1 byte: lands on the red zone's first byte */
    bad = br_heap_check();

    rz[64] = 0xA5u;               /* restore to core's red-zone fill value */
    fixed = br_heap_check();

    ml_report(bad > 0u, "TC-DBG-041", "red zone stepped on by 1 byte ⇒ br_heap_check() > 0");
    ml_report(fixed == 0u, "TC-DBG-041", "after red-zone restore (0xA5) br_heap_check() == 0");

    br_free(p);
}

/* TC-DBG-042: double free is detectable and does not corrupt the heap. Core's guard must intercept
 * the second free and keep the block chain consistent, allowing one more normal alloc/free. */
static void ml_case_042(void)
{
    void            *p = br_malloc(32u);
    br_heap_stats_t  st = { 0 };
    void            *q;
    br_bool          ok;
    int              rc;

    if (p == BR_NULL) {
        ml_report(BR_FALSE, "TC-DBG-042", "br_malloc(32) failed, cannot build the double-free scenario");
        return;
    }

    br_free(p);
    br_free(p);   /* second time: must be intercepted, not entered into the free list */

    rc = br_heap_stats_get(&st);
    ml_report((rc == 0) && (st.double_free >= 1u), "TC-DBG-042",
              "second br_free was intercepted (double_free >= 1)");
    ml_report(br_heap_check() == 0u, "TC-DBG-042",
              "heap still clean after the double free (br_heap_check == 0)");

    q  = br_malloc(48u);
    ok = (q != BR_NULL) ? BR_TRUE : BR_FALSE;
    if (ok) {
        br_free(q);
    }
    ml_report(ok, "TC-DBG-042", "after the double free one more normal alloc/free still works");
}

/* TC-DBG-043: accounting matches. The used delta for 10 x 100 B is >= 1000 (each block also carries
 * header/footer/red zone, so larger); after everything is returned it must come back to the
 * baseline exactly, not relaxed to <=. */
static void ml_case_043(void)
{
    br_size_t u0 = 0u, total0 = 0u;
    br_size_t u1 = 0u, total1 = 0u;
    br_size_t u2 = 0u, total2 = 0u;
    void     *blk[10];
    br_u32    got = 0u;
    int       rc0;
    int       rc1;
    int       rc2;

    for (br_u32 i = 0u; i < 10u; i++) {
        blk[i] = BR_NULL;
    }

    rc0 = br_heap_usage(&u0, &total0);
    for (br_u32 i = 0u; i < 10u; i++) {
        blk[i] = br_malloc(100u);
        if (blk[i] != BR_NULL) {
            got++;
        }
    }
    rc1 = br_heap_usage(&u1, &total1);
    for (br_u32 i = 0u; i < 10u; i++) {
        if (blk[i] != BR_NULL) {
            br_free(blk[i]);
        }
    }
    rc2 = br_heap_usage(&u2, &total2);

    ml_report((rc0 == 0) && (got == 10u), "TC-DBG-043", "baseline readable and all 10 × 100 B allocations succeeded");
    ml_report((rc1 == 0) && (u1 >= u0 + 1000u), "TC-DBG-043",
              "used delta >= 1000 (10 × 100 B)");
    ml_report((rc2 == 0) && (u2 == u0), "TC-DBG-043",
              "used returns exactly to baseline after all frees");
}

int memleak_selftest(void)
{
    s_ml_pass = 0u;
    s_ml_fail = 0u;

    br_log_info("[DBGCONF] info memleak selftest (TC-DBG-040..043)");

    /* Idempotent: the suite carries its own owner-tag init, so it holds under any call order. */
    (void)br_memleak_init();

    ml_case_040();
    ml_case_041();
    ml_case_042();
    ml_case_043();

    /* plugin-owned summary line (the gate only recognizes the `[DBGCONF] SUMMARY fail=0` form) */
    br_log_info("[DBGCONF] SUMMARY memleak pass=%u fail=%u total=%u",
                s_ml_pass, s_ml_fail, s_ml_pass + s_ml_fail);
    return (int)s_ml_fail;
}
