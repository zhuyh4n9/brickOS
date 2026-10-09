/*
 * brickOS prototype v0.2.0 — service/dump 的**自检套件**
 * (service/dump/src/dump_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与生产代码分离)。
 * 本文件承接原 `dump.c` 里那个调试域 conformance 入口的**自有**用例(TC-DBG-030/031/032/
 * 100): case id、断言与日志串一字未改 —— 门禁 `tests/gates.toml` 按 `[DBGCONF] PASS/FAIL`、
 * `[DBGCONF] SUMMARY` 与 require_tags 判红绿。
 *
 * ## 边界(本插件**不需要** `*_internal.h`)
 *   四条用例的被测对象全是**对外面**: 本插件的 `br_dump_regions` / `br_dump_memory` /
 *   `br_dump_all`, 以及兄弟插件与 core 的公开观测(`br_hexdump_to` / `br_hexdump_line_bytes` /
 *   `br_mm_region_count` / `br_mem_layout` / `br_bt_last_count` / `br_memleak_live` /
 *   `br_memleak_corruptions`)。原来与用例同处一文件的 `s_cf_pass/s_cf_fail/cf_report/
 *   cf_case_*` 都是**用例专有**, 所以直接搬进本文件(static) —— 没有需要暴露的生产私有符号。
 *
 * ## `[DBGCONF] SUMMARY` 口径(ADR-0010 之后**变了**, 这里如实写清)
 *   拆分前: 那个入口是**编排者** —— 它按顺序调 trace/backtrace/hexdump/
 *     memleak 四家的 `*_selftest()`, 于是它那句 SUMMARY 的 `fail` = **四家失败数之和 +
 *     自己用例的失败数**(`pass` 只算自己的, 因为子插件的 PASS 数不在它们的声明面回传)。
 *   拆分后: **core 在一个 pass 里驱动全部五个自检钩子**(`br_plugin_manager_selftest`),
 *     所以本函数只报**自己的**用例(`pass`/`fail`/`total` 都只数 TC-DBG-030/031/032/100);
 *     另外四家各自打它们自己的 `[DBGCONF] PASS/FAIL` 与各自的 SUMMARY。
 *   ⇒ 门禁 `tests/gates.toml` 里那条 `\[DBGCONF\] SUMMARY pass=[0-9]* fail=0 ` 由**本函数**
 *     满足(它只要求 fail=0); 而"每一家都跑过且都绿"由 core 的
 *     `[SELFTEST] SUMMARY ... fails=0 errors=0` 一行 + `[DBGCONF] FAIL` 的 forbid 一并保证。
 *     也就是说: **没有任何一环被少测**, 只是汇总点从"dump 之和"移到了"core 的 pass"。
 *   ⚠ 因此这句 SUMMARY 的含义**确实变了**(不再是调试域总账), 这是 ADR-0010 的裁定结果,
 *     不是顺手改口径; 若将来要恢复"域总账", 正确的落点是 core 的自检汇总, 不是某个插件。
 *
 * ## 与插件管理器的契约
 *   返回**失败项数**(0 = 全绿); **失败不停机** —— 红绿由门禁判。
 *   入口名 `dump_selftest` 由生成物按 `symbol_prefix` 推导(见 plugin_desc.c 的
 *   `.selftest`), 不是本插件的对外 API。
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

/* 入口原型(声明面集中在生成物里; 这里重述只为满足全局函数的原型纪律)。 */
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

/* TC-DBG-030: region 清单与 region 表同源同数 */
static void cf_case_030(void)
{
    const br_u32 count = br_mm_region_count();
    const br_u32 got   = br_dump_regions();

    cf_report((got == count) && (count > 0u), "TC-DBG-030",
              "region 清单条数 == region 表条数 且 > 0");
}

/* TC-DBG-031: 边界判定的两个分支(拒绝 / FORCE 不崩) */
static void cf_case_031(void)
{
    const br_uintptr_t undeclared = (br_uintptr_t)0x42000000u;
    const int strict = br_dump_memory(undeclared, (br_size_t)16u, BR_DUMP_F_STRICT);
    const int forced = br_dump_memory(undeclared, (br_size_t)16u, BR_DUMP_F_FORCE);

    cf_report(strict == BR_ERR(BR_EINVAL), "TC-DBG-031",
              "未声明地址(0x42000000) + STRICT ⇒ -EINVAL");
    cf_report(forced >= 0, "TC-DBG-031",
              "同地址 + FORCE ⇒ >= 0(表外不读取, 不崩)");
}

/* TC-DBG-032: 已声明区(堆池基址)真 dump + 呈现确实发生 */
static void cf_case_032(void)
{
    br_mem_layout_t lay;
    char            buf[256];
    br_size_t       need;
    int             rc;

    if (br_mem_layout(&lay) != 0) {
        cf_report(BR_FALSE, "TC-DBG-032", "堆布局不可读, 无法 dump 堆池基址");
        return;
    }

    rc   = br_dump_memory(lay.heap.base, (br_size_t)32u, BR_DUMP_F_STRICT);
    /* "确实渲染了"的判据取自呈现原语本身: 32 B = 2 整行(格式契约 BR_HEXDUMP_WIDTH=16) */
    need = br_hexdump_to(buf, sizeof buf, (const void *)lay.heap.base, (br_size_t)32u);

    cf_report((rc == 32) && (need > 0u) && (need == 2u * br_hexdump_line_bytes()),
              "TC-DBG-032", "堆池基址 dump 32 B: 返回 32 且 hexdump 渲染 2 行");
}

/* TC-DBG-100: 全量现场确实把所有子段都跑过(用子段自己的观测交叉验证) */
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
              "dump_all 累计行数 >= 8(标题 + region + heap + leaks + trace + bt)");
    cf_report(regions > 0u, "TC-DBG-100",
              "region 段有内容(br_mm_region_count > 0)");
    cf_report(frames > 0u, "TC-DBG-100",
              "backtrace 段确实捕获到帧(br_bt_last_count > 0)");
    cf_report(corrupt == 0u, "TC-DBG-100",
              "全量现场之后堆仍干净(br_heap_check == 0)");
}

int dump_selftest(void)
{
    s_cf_pass = 0u;
    s_cf_fail = 0u;

    br_log_info("[DBGCONF] dump conformance (region 清单 / 边界判定 / 全量现场)");

    /*
     * 顺序裁定: 本函数只跑**本插件自有**用例(03x → 100)。
     * 拆分前这里先按 trace → backtrace → hexdump → memleak 调四家的 `*_selftest()`,
     * 并代它们调四家的 `*_init()`(v0.1 的 LATE 相编排替身, WORKAROUND(br-wa-boot-001))。
     * ADR-0010 之后那两件事都归 **plugin_manager**: init 由它按 `[[dep]]` 拓扑序在 START
     * 之前调完(非 0 rc ⇒ `[PLUGIN] FAIL` + `br_panic`, ADR-0005 裁定 G6), 自检由它在一个
     * pass 里按同一拓扑序驱动。所以这里既不再有跨插件调用, 也不再需要 DBG-INIT 那条
     * "四家 init 都返回 0"的本地重述 —— 它是全局不变量的复读: 由 core 的 phase 行
     * (`[PLUGIN] phase=late name=... rc=0`)与"出现 [PANIC] 即判红"共同承担, 见
     * dump.c 的 dump_init 注释。
     *
     * TC-DBG-100 内的 `br_dump_all()` 仍会打全量现场, 于是 trace/backtrace/memleak 的
     * 真实输出照旧出现在这一段里 —— 它是本插件的生产行为(启动快照同一份), 不是编排。
     */
    cf_case_030();
    cf_case_031();
    cf_case_032();
    cf_case_100();

    /*
     * 本插件自有的摘要行(**门禁唯一 grep 的格式**)。
     * 口径(与拆分前不同, 理由见文件头 "`[DBGCONF] SUMMARY` 口径"): `pass`/`fail` 只数
     * 本插件自己的断言; 另外四家的失败体现在它们各自的 SUMMARY 与 core 的
     * `[SELFTEST] SUMMARY ... fails=0` 里。门禁只认 `fail=0`, 本行因此仍然如实反映
     * "dump 自己的用例全绿", 而全域的红绿由 core 那一行保证。
     */
    br_log_info("[DBGCONF] SUMMARY pass=%u fail=%u total=%u",
                s_cf_pass, s_cf_fail, s_cf_pass + s_cf_fail);

    return (int)s_cf_fail;
}
