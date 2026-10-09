/*
 * brickOS prototype v0.2.0 — service/memleak 的**自检套件**
 * (service/memleak/src/memleak_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与生产代码分离)。
 * 本文件是从 `memleak.c` **原样搬来**的用例(TC-DBG-040..043): case id、断言与日志串
 * 一字未改 —— 门禁 `tests/gates.toml` 按 `[DBGCONF] PASS/FAIL` 与 require_tags 判红绿。
 *
 * ## 边界(本插件**不需要** `*_internal.h`)
 *   四条用例的被测对象全是**对外面**: 本插件的 `br_memleak_live` /
 *   `br_memleak_live_by_owner` / `br_memleak_owner` / `br_memleak_report` /
 *   `br_memleak_corruptions`, 以及 core 的堆观测契约(`br_mem.h`)。原来与用例同处一
 *   文件的 `s_ml_pass/s_ml_fail/ml_report/ml_case_*` 都是**用例专有**, 所以直接搬进
 *   本文件(static), 生产文件不再背着它们 —— 没有需要暴露的生产私有符号。
 *
 * ## 与插件管理器的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[DBGCONF] PASS/FAIL` + 本插件 SUMMARY)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[DBGCONF] FAIL`)。
 *   入口名 `memleak_selftest` 由生成物按 `symbol_prefix` 推导(见 plugin_desc.c 的
 *   `.selftest`), 不是本插件的对外 API。
 */
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/debug/br_memleak.h>

/* 入口原型(声明面集中在生成物里; 这里重述只为满足全局函数的原型纪律)。 */
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
 * TC-DBG-040 真泄漏可检出。
 * 故意"丢掉指针"的含义是: 判据只用 core 的观测(br_heap_walk / report), 不看返回值;
 * 但用例收尾仍用那块地址归还, 不给系统留真泄漏(用例是判据, 不是破坏现场)。
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

    /* 归属标签必须真的注册过, 否则本用例退化成"未归属块"的计数 */
    ml_report(br_memleak_owner() != BR_OWNER_NONE, "TC-DBG-040",
              "归属标签已注册(br_memleak_owner != NONE)");

    leaked = br_malloc(123u);   /* 指针故意不参与判据 */

    live1 = br_memleak_live();
    mine1 = br_memleak_live_by_owner(br_memleak_owner());
    rep   = br_memleak_report();   /* 打印现场, 顺带回传存活块数 */

    (void)br_heap_owner_set(prev);

    ml_report(leaked != BR_NULL, "TC-DBG-040", "br_malloc(123) 成功(指针故意丢掉)");
    ml_report(mine1 >= mine0 + 1u, "TC-DBG-040",
              "本归属存活块 >= 基线+1(br_memleak_live_by_owner)");
    /* report 的 live 是被打印口径, 必须与独立观测一致; 且增量只可能来自本归属 */
    ml_report((rep == live1) && ((live1 - live0) == (mine1 - mine0)), "TC-DBG-040",
              "report 的 live 与观测一致且增量归属唯一");

    br_free(leaked);   /* 还堆干净状态 */
}

/*
 * TC-DBG-041 红区越界可检出。
 * 依赖 core 的块布局约定: 用户区 p..p+size-1, 紧随其后是红区 ⇒ 请求 64 B 时
 * 首字节越界落在 p+64; 红区填充值 0xA5(见 core 的 mem.c 块布局注释)。
 * 用例收尾把那一字节复原 —— 否则堆会带着"修不好的破坏"进入后续用例。
 */
static void ml_case_041(void)
{
    void           *p = br_malloc(64u);
    volatile br_u8 *rz;
    br_u32          bad;
    br_u32          fixed;

    if (p == BR_NULL) {
        ml_report(BR_FALSE, "TC-DBG-041", "br_malloc(64) 失败, 无法构造红区场景");
        return;
    }

    rz = (volatile br_u8 *)p;
    rz[64] = 0xFFu;               /* 越界 1 字节: 落在红区首字节 */
    bad = br_heap_check();

    rz[64] = 0xA5u;               /* 复原为 core 的红区填充值 */
    fixed = br_heap_check();

    ml_report(bad > 0u, "TC-DBG-041", "红区被踩 1 字节 ⇒ br_heap_check() > 0");
    ml_report(fixed == 0u, "TC-DBG-041", "红区复原(0xA5)后 br_heap_check() == 0");

    br_free(p);
}

/*
 * TC-DBG-042 双重释放可检出且不破坏堆。
 * 第二次 free 必须被 core 的重复释放拦截(double_free 计数), 且块链仍然自洽、
 * 之后还能正常分配/释放一次。
 */
static void ml_case_042(void)
{
    void            *p = br_malloc(32u);
    br_heap_stats_t  st = { 0 };
    void            *q;
    br_bool          ok;
    int              rc;

    if (p == BR_NULL) {
        ml_report(BR_FALSE, "TC-DBG-042", "br_malloc(32) 失败, 无法构造双重释放场景");
        return;
    }

    br_free(p);
    br_free(p);   /* 第二次: 应被拦截, 不进空闲链 */

    rc = br_heap_stats_get(&st);
    ml_report((rc == 0) && (st.double_free >= 1u), "TC-DBG-042",
              "第二次 br_free 被拦截(double_free >= 1)");
    ml_report(br_heap_check() == 0u, "TC-DBG-042",
              "双重释放之后堆仍干净(br_heap_check == 0)");

    q  = br_malloc(48u);
    ok = (q != BR_NULL) ? BR_TRUE : BR_FALSE;
    if (ok) {
        br_free(q);
    }
    ml_report(ok, "TC-DBG-042", "双重释放之后仍能正常分配/释放一次");
}

/*
 * TC-DBG-043 记账匹配。
 * 增量判据用实际观测: 10 × 100 B 的 used 增量 >= 1000(每块还带 header/footer/红区,
 * 故实际更大); 全部归还后必须**精确**回到基线(不放宽成 <=)。
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

    ml_report((rc0 == 0) && (got == 10u), "TC-DBG-043", "基线可读且 10 × 100 B 全部分配成功");
    ml_report((rc1 == 0) && (u1 >= u0 + 1000u), "TC-DBG-043",
              "used 增量 >= 1000(10 × 100 B)");
    ml_report((rc2 == 0) && (u2 == u0), "TC-DBG-043",
              "全部释放后 used 精确回到基线");
}

int memleak_selftest(void)
{
    s_ml_pass = 0u;
    s_ml_fail = 0u;

    br_log_info("[DBGCONF] info memleak selftest (TC-DBG-040..043)");

    /* 幂等: 本套件自带 init(归属标签注册), 于是它在任何调用次序下都成立 ——
     * 不再依赖"某个编排者先替我把四家 init 调一遍"(那套编排已按 ADR-0010 交给
     * plugin_manager: 各插件 init 在 START 相之前完成, 自检在 START 之后统一驱动)。 */
    (void)br_memleak_init();

    ml_case_040();
    ml_case_041();
    ml_case_042();
    ml_case_043();

    /* 插件自有的摘要行(门禁只认 `[DBGCONF] SUMMARY ... fail=0` 这一条的口径) */
    br_log_info("[DBGCONF] SUMMARY memleak pass=%u fail=%u total=%u",
                s_ml_pass, s_ml_fail, s_ml_pass + s_ml_fail);
    return (int)s_ml_fail;
}
