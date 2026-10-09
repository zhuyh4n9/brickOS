/*
 * brickOS prototype v0.2.0 — service/trace 的**自检套件**(service/trace/src/trace_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与生产代码分离)。
 * 本文件是从 `trace.c` **原样搬来**的用例(TC-DBG-001..003): case id、断言与日志串
 * 一字未改 —— 门禁 `tests/gates.toml` 按 `[DBGCONF] PASS/FAIL` 与 require_tags 判红绿。
 *
 * ## 边界
 *   只经 `br/debug/br_trace_svc.h` 的**公开 API** 驱动(marker / register / name /
 *   report / seen / reset / summary), 不看 core 环的内部;
 *   **例外只有两处**: "两个 tag 初始槽位相同"与"名字逐字节相等"要用生产实现里的
 *   `trc_slot_of` / `trc_streq`。它们经 `src/trace_internal.h` 的访问器**转调生产实现**
 *   (不复制逻辑), 理由见该头文件。
 *
 * ## 与插件管理器的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[DBGCONF] PASS/FAIL ...`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[DBGCONF] FAIL`)。
 *   入口名 `trace_selftest` 由生成物按 `symbol_prefix` 推导(见 plugin_desc.c 的
 *   `.selftest`), 不是本插件的对外 API。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/debug/br_trace_svc.h>

#include "trace_internal.h"

/* 入口原型(声明面集中在生成物里; 这里重述只为满足全局函数的原型纪律)。 */
int trace_selftest(void);

/* ==================================================================== 用例夹具
 *
 * selftest 的 tag 用静态数组: marker 的幂等判据是指针相等。
 */
static const char TRC_TAG_SELFTEST[] = "selftest.a";

/* 线性探测判据: 下面两个 tag 的初始槽位**相同**(此处 = 16; 用例内用 trc_slot_of
 * 复核), 用来断言"撞槽不再 -ENOSPC, 而是探测到下一个空槽、各自拿到不同 id"。 */
static const char TRC_TAG_SLOT_A[] = "selftest.victor";
static const char TRC_TAG_SLOT_B[] = "selftest.yankee";

/* "内容同名但指针不同 ⇒ -ENOSPC" 判据。刻意用**非 const**的可写数组: 常量合并
 * (-fmerge-constants)可能把内容相同的 const 数组并成同一地址, 那就测不到
 * "指针不同"这一支了。 */
static char TRC_TAG_DUP_A[] = "selftest.dup";
static char TRC_TAG_DUP_B[] = "selftest.dup";

/* 记一条 DBGCONF 判据; 返回 0/1 便于累加失败数 */
static br_u32 trc_conf(br_bool ok, const char *tag, const char *what)
{
    br_log_info("[DBGCONF] %s %s %s", ok ? "PASS" : "FAIL", tag, what);
    return ok ? 0u : 1u;
}

/* ==================================================================== 用例 */

int trace_selftest(void)
{
    br_u32 fails = 0u;

    /* ---- TC-DBG-001: marker 幂等 + 动态 id/名字 + 消费计数 ---- */
    br_trace_svc_reset();

    const int   id1 = br_trace_svc_marker(TRC_TAG_SELFTEST);
    const int   id2 = br_trace_svc_marker(TRC_TAG_SELFTEST);
    const char *nm  = (id1 >= 0) ? br_trace_svc_name((br_u32)id1) : BR_NULL;

    fails += trc_conf((id1 >= (int)BR_TRACE_SVC_ID_BASE) && (id1 == id2)
                      && (nm != BR_NULL) && br_trace_internal_streq(nm, TRC_TAG_SELFTEST),
                      "TC-DBG-001",
                      "marker 两次得到同一动态 id(>= BASE)且 name 可解码");

    const br_u32 got1 = br_trace_svc_report(0u);
    fails += trc_conf((got1 >= 2u) && (id1 >= 0)
                      && (br_trace_svc_seen((br_u32)id1) >= 2u),
                      "TC-DBG-001",
                      "report(0) 取回 >=2 条且 seen(id) >= 2");

    /* ---- TC-DBG-001(续): 撞槽走线性探测, 不再"撞车即错" ----
     * 两个 tag 的初始槽位相同(用例内用 trc_slot_of 复核, 免得 hash 改动后这条
     * 断言悄悄变成空转); 断言两者都注册成功、id 不同、名字各自可解码。
     * 用 register(不发事件); tag 是静态数组(指针稳定) ⇒ 重复跑 selftest 仍幂等。 */
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
                      "hash 同槽的两个 tag 线性探测后都注册成功(id 不同、名字正确)");

    /* 内容同名但指针不同 ⇒ -ENOSPC(身份不可判定, 不复用也不新建第二份名字)。
     * 顺带断言两个静态数组地址确实不同(常量合并若发生, 这条会先报出来)。 */
    const int da = br_trace_svc_register(TRC_TAG_DUP_A);
    const int db = br_trace_svc_register(TRC_TAG_DUP_B);

    /* -Warray-compare: 直接比较数组名会被警告, 这里显式比首元素地址 */
    fails += trc_conf((da >= (int)BR_TRACE_SVC_ID_BASE)
                      && (db == BR_ERR(BR_ENOSPC))
                      && (&TRC_TAG_DUP_A[0] != &TRC_TAG_DUP_B[0]),
                      "TC-DBG-001",
                      "内容同名但指针不同 ⇒ -ENOSPC(不复用、不新建)");

    /* ---- TC-DBG-002: 故意把环打满, overrun > 0 是"这段历史不可信"的诚实信号 ----
     * 连续发 RING_SIZE + 8 条而不消费: 生产者覆盖尚未消费的旧槽位, overrun 递增。
     * summary 打印的 overrun 直接取 br_trace_overrun(), 与断言值同源 ⇒ 相等按构造成立;
     * 这里额外验证"打印前后读数不变"(drain/summary 都不改生产者计数)。 */
    const br_u32 orun_before = br_trace_overrun();

    for (br_u32 i = 0u; i < ((br_u32)BR_TRACE_RING_SIZE + 8u); i++) {
        (void)br_trace_svc_marker(TRC_TAG_SELFTEST);
    }

    /* 全部 drain; 有界循环: 消费期间 ISR 可能继续补货, 不能死等"空" */
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
                      "打满环后 overrun > 0 且 summary 的 overrun 与之一致");

    /* ---- TC-DBG-003: 截断消费后从最旧未消费处续读, 不丢中间积压 ---- */
    br_trace_svc_reset();

    for (br_u32 i = 0u; i < 5u; i++) {
        (void)br_trace_svc_marker(TRC_TAG_SELFTEST);
    }

    const br_u32 first = br_trace_svc_report(2u);
    const br_u32 rest  = br_trace_svc_report(0u);

    fails += trc_conf((first == 2u) && (rest >= 3u),
                      "TC-DBG-003",
                      "report(2) 只取 2 条, 紧接 report(0) 续读 >=3 条");

    /* ---- 收尾: 清计数并把环 drain 空, 免得污染后续 dump 的 trace 段 ---- */
    br_trace_svc_reset();
    for (br_u32 i = 0u; i < 8u; i++) {
        if (br_trace_svc_report(0u) == 0u) {
            break;
        }
    }
    br_trace_svc_reset();

    return (int)fails;
}
