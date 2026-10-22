/*
 * Self-test suite for provider/dump: its own cases TC-DBG-030/031/032/100. All four test the public
 * surface (br_dump_regions / br_dump_memory / br_dump_all, plus sibling plugins' and core's public
 * observations), so no *_internal.h is needed.
 *
 * [DBGCONF] SUMMARY semantics: this function reports only its own cases; core drives all five
 * self-test hooks in one pass (br_plugin_manager_selftest), so the gate's fail=0 line is satisfied
 * here, while "everyone ran and is green" is guaranteed by core's [SELFTEST] SUMMARY line plus the
 * forbid of [DBGCONF] FAIL.
 *
 * Returns the failure count (0 = all green); a failure does not halt. The entry name dump_selftest
 * is derived by the generated code from symbol_prefix; it is not this plugin's public API.
 */
#include <br/debug/br_bt.h>
#include <br/debug/br_dump.h>
#include <br/debug/br_hexdump.h>
#include <br/debug/br_memleak.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>
#include <br/hal/br_mm.h>
#include <br/core/br_types.h>

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

    /* Runs only this plugin's own cases (03x -> 100): cross-plugin init and selftests are driven by
     * plugin_manager, which replaced the old orchestration stand-in (WORKAROUND(br-wa-boot-001)).
     * br_dump_all() in TC-DBG-100 still prints the real trace/backtrace/memleak output. */
    cf_case_030();
    cf_case_031();
    cf_case_032();
    cf_case_100();

    /* This plugin's own summary line, the only format the gate greps: pass/fail count only this
     * plugin's assertions; whole-domain red/green comes from core's [SELFTEST] SUMMARY. */
    br_log_info("[DBGCONF] SUMMARY pass=%u fail=%u total=%u",
                s_cf_pass, s_cf_fail, s_cf_pass + s_cf_fail);

    return (int)s_cf_fail;
}
