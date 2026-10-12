/*
 * brickOS prototype v0.2.0 — **self-test suite** for service/memleak
 * (service/memleak/src/memleak_selftest.c)
 *
 * Design basis and rulings: `docs/decisions/0010-plugin-selftest.md` (self-test separated from
 * production code).
 * This file holds the cases (TC-DBG-040..043) **moved verbatim** out of `memleak.c`: case id,
 * assertions and log strings are unchanged word for word — the gate `tests/gates.toml` decides
 * red/green by `[DBGCONF] PASS/FAIL` and require_tags.
 *
 * ## Boundary (this plugin does **not** need a `*_internal.h`)
 *   All four cases exercise only the **public surface**: this plugin's `br_memleak_live` /
 *   `br_memleak_live_by_owner` / `br_memleak_owner` / `br_memleak_report` /
 *   `br_memleak_corruptions`, plus core's heap observation contract (`br_mem.h`). The
 *   `s_ml_pass/s_ml_fail/ml_report/ml_case_*` that used to share a file with the cases are
 *   **case-private**, so they moved here directly (static) and the production file no longer
 *   carries them — there are no production-private symbols to expose.
 *
 * ## Contract with the plugin manager
 *   Returns the **number of failures** (0 = all green); it logs its own lines (`[DBGCONF]
 *   PASS/FAIL` + this plugin's SUMMARY). **A failure does not halt** — the gate decides
 *   red/green (`[DBGCONF] FAIL` is in the forbid list of `tests/gates.toml`). The entry name
 *   `memleak_selftest` is derived from the generated code by `symbol_prefix` (see `.selftest`
 *   in plugin_desc.c); it is not this plugin's public API.
 */
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/debug/br_memleak.h>

/* Entry prototype (declarations live in the generated code; repeated here only to satisfy the
 * prototype discipline for global functions). */
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

/*
 * TC-DBG-040: a real leak is detectable.
 * "Dropping the pointer" on purpose means the criterion uses only core's observation
 * (br_heap_walk / report), not the return value; but the case still frees that address at the
 * end, leaving no real leak in the system (a case is a criterion, not a scene to wreck).
 */
static void ml_case_040(void)
{
    const br_u32     live0 = br_memleak_live();
    const br_u32     mine0 = br_memleak_live_by_owner(br_memleak_owner());
    const br_owner_t prev  = br_heap_owner_set(br_memleak_owner());
    void            *leaked;
    br_u32           live1;
    br_u32           mine1;
    br_u32           rep;

    /* The owner tag must really be registered, else this case degenerates into counting
     * unowned blocks */
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
    /* report's live is the printed figure and must match independent observation; the delta
     * can only come from this owner */
    ml_report((rep == live1) && ((live1 - live0) == (mine1 - mine0)), "TC-DBG-040",
              "report live matches observation and the delta has a single owner");

    br_free(leaked);   /* return the heap to a clean state */
}

/*
 * TC-DBG-041: a red-zone overflow is detectable.
 * Relies on core's block layout convention: user area p..p+size-1 with the red zone
 * immediately after ⇒ for a 64 B request the first overflow byte lands at p+64; the red zone
 * fill value is 0xA5 (see the block layout comment in core's mem.c).
 * The case restores that byte at the end — otherwise the heap would enter later cases with
 * "damage that cannot be repaired".
 */
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

/*
 * TC-DBG-042: double free is detectable and does not corrupt the heap.
 * The second free must be intercepted by core's double-free guard (double_free counter), and
 * the block chain must stay consistent, still allowing one more normal alloc/free.
 */
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

/*
 * TC-DBG-043: accounting matches.
 * The delta criterion uses actual observation: the used delta for 10 × 100 B is >= 1000 (each
 * block also carries header/footer/red zone, so it is actually larger); after everything is
 * returned it must come back to the baseline **exactly** (not relaxed to <=).
 */
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

    /* Idempotent: this suite carries its own init (owner-tag registration), so it holds under
     * any call order — no longer relying on "some orchestrator runs all four inits first"
     * (that orchestration was handed to plugin_manager per ADR-0010: plugin inits complete
     * before the START phase, self-tests are driven uniformly after START). */
    (void)br_memleak_init();

    ml_case_040();
    ml_case_041();
    ml_case_042();
    ml_case_043();

    /* plugin-owned summary line (the gate only recognizes the `[DBGCONF] SUMMARY ... fail=0`
     * form) */
    br_log_info("[DBGCONF] SUMMARY memleak pass=%u fail=%u total=%u",
                s_ml_pass, s_ml_fail, s_ml_pass + s_ml_fail);
    return (int)s_ml_fail;
}
