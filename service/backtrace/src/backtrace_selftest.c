/*
 * brickOS prototype v0.2.0 — **self-test suite** for service/backtrace
 * (service/backtrace/src/backtrace_selftest.c)
 *
 * Design basis and ruling: `docs/decisions/0010-plugin-selftest.md` (self-tests separate from
 * production code). This file holds the cases (TC-DBG-010/011) **moved verbatim** from
 * `backtrace.c`: case ids, assertions and log strings unchanged — the gate `tests/gates.toml`
 * judges red/green by `[DBGCONF] PASS/FAIL` and require_tags.
 *
 * ## Boundary
 *   Driven only through the **public API** of `br/debug/br_bt.h` (capture / capture_from /
 *   print / set_stack_bounds / last / last_count);
 *   **one exception**: TC-DBG-011 must restore the stack bounds to "as before the call"
 *   (possibly unset 0/0), via the accessors in `src/backtrace_internal.h` that read/write
 *   production state (no copy); rationale in that header.
 *
 * ## Contract with the plugin manager
 *   Returns the **number of failures** (0 = all green); it logs itself
 *   (`[DBGCONF] PASS/FAIL ...`).
 *   **A failure does not halt** — the gate judges (`[DBGCONF] FAIL` is in tests/gates.toml's
 *   forbid list).
 *   The entry name `backtrace_selftest` is derived by the generated artifact from
 *   `symbol_prefix` (see `.selftest` in plugin_desc.c); it is not this plugin's public API.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/debug/br_bt.h>

#include "backtrace_internal.h"

/* Entry prototype (the declaration surface is centralized in the artifact; repeated here only
 * to satisfy the global-function prototype discipline). */
int backtrace_selftest(void);

/* Scan window used by TC-DBG-010 to decide "pc lies inside the probe function body" */
#define BT_PROBE_SPAN            512u

/* Record one DBGCONF criterion; returns 0/1 so failures can be accumulated */
static br_u32 bt_conf(br_bool ok, const char *tag, const char *what)
{
    br_log_info("[DBGCONF] %s %s %s", ok ? "PASS" : "FAIL", tag, what);
    return ok ? 0u : 1u;
}

/* Probe: must be noinline, else its frame is folded and the pc-location criterion is meaningless */
static void __attribute__((noinline))
bt_probe(br_bt_frame_t *out, br_u32 max, br_u32 *n)
{
    *n = br_bt_capture(out, max);
}

/* Whether some frame's pc lies in [&bt_probe, &bt_probe + BT_PROBE_SPAN) —
 * that proves the frame chain really crossed "the call site that called capture", not merely
 * that the current frame was read. */
static br_bool bt_probe_covered(const br_bt_frame_t *fr, br_u32 n)
{
    const br_uintptr_t lo = (br_uintptr_t)&bt_probe;
    const br_uintptr_t hi = lo + (br_uintptr_t)BT_PROBE_SPAN;

    for (br_u32 i = 0u; i < n; i++) {
        if ((fr[i].pc >= lo) && (fr[i].pc < hi)) {
            return BR_TRUE;
        }
    }
    return BR_FALSE;
}

int backtrace_selftest(void)
{
    static br_bt_frame_t frames[BR_BT_MAX_FRAMES];
    br_u32 fails = 0u;

    /* ---- TC-DBG-010: chain integrity on the real stack + call-site coverage ---- */
    br_u32 n = 0u;
    bt_probe(frames, BR_BT_MAX_FRAMES, &n);

    br_bool ok10 = (n >= 2u) && (frames[0].depth == 0u) && bt_probe_covered(frames, n);

    for (br_u32 i = 0u; i < n; i++) {
        ok10 = ok10 && (frames[i].pc != 0u);
        ok10 = ok10 && ((frames[i].fp % 16u) == 0u);
        if (i > 0u) {
            ok10 = ok10 && (frames[i].fp > frames[i - 1u].fp);
        }
    }

    fails += bt_conf(ok10, "TC-DBG-010",
                     "frames>=2, pc!=0, fp 16-aligned and strictly increasing, pc covers probe body");

    /* ---- TC-DBG-011: empty/bad context rejected; bounds guard works; max works ---- */
    br_uintptr_t saved_bottom = 0u;   /* white-box save; the case restores it at the end */
    br_uintptr_t saved_top    = 0u;
    br_bt_internal_read_stack_bounds(&saved_bottom, &saved_top);
    br_bool ok11 = BR_TRUE;

    /* fp == 0 ⇒ 0 frames (no context); fp not 16-aligned ⇒ 0 frames and no dereference (no death) */
    ok11 = ok11 && (br_bt_capture_from(0u, 0u, frames, 8u) == 0u);
    ok11 = ok11 && (br_bt_capture_from(0x1234u, 0x40080000u, frames, 8u) == 0u);

    /* illegal bounds rejected (the setter validates before writing ⇒ state unchanged; this
     * also checks the invariant) */
    ok11 = ok11 && (br_bt_set_stack_bounds(0u, 0x1000u) == BR_ERR(EINVAL));
    ok11 = ok11 && (br_bt_set_stack_bounds(0x2000u, 0x1000u) == BR_ERR(EINVAL));

    /* legal but very narrow bounds: the current stack is outside ⇒ capture must truncate to
     * 0 frames (proves bounds are really checked) */
    ok11 = ok11 && (br_bt_set_stack_bounds(0x1000u, 0x2000u) == 0);
    ok11 = ok11 && (br_bt_capture(frames, 4u) == 0u);

    /* restore the pre-call bounds (possibly 0/0 = "unset"), leaving no narrow case bounds behind */
    br_bt_internal_restore_stack_bounds(saved_bottom, saved_top);

    /* max takes effect: asking for 1 frame writes only 1 frame */
    ok11 = ok11 && (br_bt_capture(frames, 1u) == 1u);

    fails += bt_conf(ok11, "TC-DBG-011",
                     "capture_from(0/unaligned)=0, bad bounds rejected, narrow bounds truncate, max works");

    return (int)fails;
}
