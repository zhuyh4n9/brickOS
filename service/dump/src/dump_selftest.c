/*
 * brickOS prototype v0.2.0 — service/dump **self-test suite**
 * (service/dump/src/dump_selftest.c)
 *
 * Basis and ruling: `docs/decisions/0010-plugin-selftest.md` (self-tests separate from
 * production code). This file takes over the debug-domain conformance entry's **own** cases
 * (TC-DBG-030/031/032/100): case ids, assertions and log strings unchanged — the
 * `tests/gates.toml` gate judges red/green by `[DBGCONF] PASS/FAIL`, `[DBGCONF] SUMMARY` and
 * require_tags.
 *
 * ## Boundary (this plugin does **not** need `*_internal.h`)
 *   All four cases test the public surface: this plugin's `br_dump_regions` / `br_dump_memory` /
 *   `br_dump_all`, plus sibling plugins' and core's public observations (`br_hexdump_to` /
 *   `br_hexdump_line_bytes` / `br_mm_region_count` / `br_mem_layout` / `br_bt_last_count` /
 *   `br_memleak_live` / `br_memleak_corruptions`). The `s_cf_pass/s_cf_fail/cf_report/
 *   cf_case_*` helpers were **case-private**, so they moved into this file (static) — no
 *   production private symbol needs exposing.
 *
 * ## `[DBGCONF] SUMMARY` semantics (**changed** after ADR-0010; stated honestly here)
 *   Before the split: that entry was the **orchestrator** — it called trace/backtrace/hexdump/
 *     memleak `*_selftest()` in order, so its SUMMARY `fail` = **sum of the four failures +
 *     its own case failures** (`pass` counted only its own, since child plugins' PASS counts
 *     are not returned on their declared surface).
 *   After the split: **core drives all five self-test hooks in one pass**
 *     (`br_plugin_manager_selftest`), so this function reports only **its own** cases
 *     (`pass`/`fail`/`total` count only TC-DBG-030/031/032/100); the other four print their own
 *     `[DBGCONF] PASS/FAIL` and their own SUMMARY.
 *   ⇒ The `\[DBGCONF\] SUMMARY pass=[0-9]* fail=0 ` line in `tests/gates.toml` is satisfied by
 *     **this function** (it only requires fail=0); "everyone ran and everyone is green" is
 *     guaranteed by core's `[SELFTEST] SUMMARY ... fails=0 errors=0` line plus the forbid of
 *     `[DBGCONF] FAIL`.
 *     In other words: **no stage lost coverage**, only the aggregation point moved from "the sum
 *     over dump" to "core's pass".
 *   ⚠ So this SUMMARY line's meaning **did change** (no longer the debug-domain ledger); that is
 *     ADR-0010's ruling, not an incidental change of semantics. If a "domain ledger" is ever
 *     wanted again, the right place is core's self-test summary, not a plugin.
 *
 * ## Contract with the plugin manager
 *   Returns the **failure count** (0 = all green); **failure does not halt** — the gate judges
 *   red/green. The entry name `dump_selftest` is derived by the generated code from
 *   `symbol_prefix` (see `.selftest` in plugin_desc.c); it is not this plugin's public API.
 */
#include <br/debug/br_bt.h>
#include <br/debug/br_dump.h>
#include <br/debug/br_hexdump.h>
#include <br/debug/br_memleak.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_types.h>

/* Entry prototype (declarations live in the generated code; restated here for prototype discipline). */
int dump_selftest(void);

static br_u32 s_cf_pass;
static br_u32 s_cf_fail;

static void cf_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_cf_pass++;
        br_log_info("[DBGCONF] PASS %s %s", tag, what);
    } else {
        s_cf_fail++;
        br_log_info("[DBGCONF] FAIL %s %s", tag, what);
    }
}

/* TC-DBG-030: region listing has the same source and count as the region table */
static void cf_case_030(void)
{
    const br_u32 count = br_mm_region_count();
    const br_u32 got   = br_dump_regions();

    cf_report((got == count) && (count > 0u), "TC-DBG-030",
              "region listing count == region table count and > 0");
}

/* TC-DBG-031: both branches of the bounds decision (reject / FORCE without crash) */
static void cf_case_031(void)
{
    const br_uintptr_t undeclared = (br_uintptr_t)0x42000000u;
    const int strict = br_dump_memory(undeclared, (br_size_t)16u, BR_DUMP_F_STRICT);
    const int forced = br_dump_memory(undeclared, (br_size_t)16u, BR_DUMP_F_FORCE);

    cf_report(strict == BR_ERR(EINVAL), "TC-DBG-031",
              "undeclared address (0x42000000) + STRICT ⇒ -EINVAL");
    cf_report(forced >= 0, "TC-DBG-031",
              "same address + FORCE ⇒ >= 0 (out-of-table not read, no crash)");
}

/* TC-DBG-032: real dump of a declared region (heap pool base) + rendering really happens */
static void cf_case_032(void)
{
    br_mem_layout_t lay;
    char            buf[256];
    br_size_t       need;
    int             rc;

    if (br_mem_layout(&lay) != 0) {
        cf_report(BR_FALSE, "TC-DBG-032", "heap layout unreadable, cannot dump heap pool base");
        return;
    }

    rc   = br_dump_memory(lay.heap.base, (br_size_t)32u, BR_DUMP_F_STRICT);
    /* "did render" is judged by the render primitive itself: 32 B = 2 full lines (BR_HEXDUMP_WIDTH=16) */
    need = br_hexdump_to(buf, sizeof buf, (const void *)lay.heap.base, (br_size_t)32u);

    cf_report((rc == 32) && (need > 0u) && (need == 2u * br_hexdump_line_bytes()),
              "TC-DBG-032", "heap pool base dump of 32 B: returns 32 and hexdump renders 2 lines");
}

/* TC-DBG-100: the full snapshot really ran every sub-section (cross-checked via their observations) */
static void cf_case_100(void)
{
    const br_u32 regions = br_mm_region_count();
    const br_u32 lines   = br_dump_all();
    const br_u32 frames  = br_bt_last_count();
    const br_u32 live    = br_memleak_live();
    const br_u32 corrupt = br_memleak_corruptions();

    br_log_info("[DBGCONF] info dump_all lines=%u regions=%u bt_frames=%u live=%u corrupt=%u",
                lines, regions, frames, live, corrupt);

    cf_report(lines >= 8u, "TC-DBG-100",
              "dump_all total lines >= 8 (header + region + heap + leaks + trace + bt)");
    cf_report(regions > 0u, "TC-DBG-100",
              "region section has content (br_mm_region_count > 0)");
    cf_report(frames > 0u, "TC-DBG-100",
              "backtrace section really captured frames (br_bt_last_count > 0)");
    cf_report(corrupt == 0u, "TC-DBG-100",
              "heap still clean after the full snapshot (br_heap_check == 0)");
}

int dump_selftest(void)
{
    s_cf_pass = 0u;
    s_cf_fail = 0u;

    br_log_info("[DBGCONF] dump conformance (region listing / bounds decision / full snapshot)");

    /*
     * Ordering ruling: this function runs only **this plugin's own** cases (03x → 100).
     * Before the split it first called the four `*_selftest()` (trace → backtrace → hexdump →
     * memleak) and called their `*_init()` for them (the v0.1 LATE-phase orchestration stand-in,
     * WORKAROUND(br-wa-boot-001)). After ADR-0010 both of those belong to **plugin_manager**:
     * it calls init in `[[dep]]` topological order before START (non-zero rc ⇒ `[PLUGIN] FAIL` +
     * `br_panic`, ADR-0005 ruling G6), and it drives the self-tests in one pass in that same
     * order. So there are no cross-plugin calls here any more, and the local restatement of the
     * DBG-INIT "all four inits returned 0" is gone too — it repeats a global invariant carried by
     * core's phase lines (`[PLUGIN] phase=late name=... rc=0`) plus "any [PANIC] means red", see
     * the dump_init comment in dump.c.
     *
     * `br_dump_all()` inside TC-DBG-100 still prints the full snapshot, so the real
     * trace/backtrace/memleak output still appears in this section — that is this plugin's
     * production behaviour (the same one as the boot snapshot), not orchestration.
     */
    cf_case_030();
    cf_case_031();
    cf_case_032();
    cf_case_100();

    /*
     * This plugin's own summary line (**the only format the gate greps**).
     * Semantics (different from before the split, reasons in the file header "`[DBGCONF] SUMMARY`
     * semantics"): `pass`/`fail` count only this plugin's own assertions; the other four failures
     * show up in their own SUMMARY and in core's `[SELFTEST] SUMMARY ... fails=0`. The gate only
     * reads `fail=0`, so this line still honestly reports "dump's own cases are all green", while
     * the whole-domain red/green is guaranteed by core's line.
     */
    br_log_info("[DBGCONF] SUMMARY pass=%u fail=%u total=%u",
                s_cf_pass, s_cf_fail, s_cf_pass + s_cf_fail);

    return (int)s_cf_fail;
}
