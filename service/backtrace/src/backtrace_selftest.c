/*
 * brickOS prototype v0.2.0 — service/backtrace 的**自检套件**
 * (service/backtrace/src/backtrace_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与生产代码分离)。
 * 本文件是从 `backtrace.c` **原样搬来**的用例(TC-DBG-010/011): case id、断言与日志串
 * 一字未改 —— 门禁 `tests/gates.toml` 按 `[DBGCONF] PASS/FAIL` 与 require_tags 判红绿。
 *
 * ## 边界
 *   只经 `br/debug/br_bt.h` 的**公开 API** 驱动(capture / capture_from / print /
 *   set_stack_bounds / last / last_count);
 *   **例外只有一处**: TC-DBG-011 收尾要把栈边界复原成"调用前的原样"(可能是未设置的
 *   0/0), 经 `src/backtrace_internal.h` 的访问器读/写生产状态(不复制), 理由见该头文件。
 *
 * ## 与插件管理器的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[DBGCONF] PASS/FAIL ...`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[DBGCONF] FAIL`)。
 *   入口名 `backtrace_selftest` 由生成物按 `symbol_prefix` 推导(见 plugin_desc.c 的
 *   `.selftest`), 不是本插件的对外 API。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/debug/br_bt.h>

#include "backtrace_internal.h"

/* 入口原型(声明面集中在生成物里; 这里重述只为满足全局函数的原型纪律)。 */
int backtrace_selftest(void);

/* TC-DBG-010 判定"pc 落在探针函数体内"的扫描窗口 */
#define BT_PROBE_SPAN            512u

/* 记一条 DBGCONF 判据; 返回 0/1 便于累加失败数 */
static br_u32 bt_conf(br_bool ok, const char *tag, const char *what)
{
    br_log_info("[DBGCONF] %s %s %s", ok ? "PASS" : "FAIL", tag, what);
    return ok ? 0u : 1u;
}

/* 探针: 必须 noinline, 否则它的帧会被折叠, pc 落点判据失去意义 */
static void __attribute__((noinline))
bt_probe(br_bt_frame_t *out, br_u32 max, br_u32 *n)
{
    *n = br_bt_capture(out, max);
}

/* 是否有某一帧的 pc 落在 [&bt_probe, &bt_probe + BT_PROBE_SPAN) ——
 * 这证明帧链真的穿过了"调用 capture 的那个调用点", 而不只是读到了当前帧。 */
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

    /* ---- TC-DBG-010: 真栈上的链完整性 + 调用点覆盖 ---- */
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
                     "帧数>=2, pc!=0, fp 16 对齐且严格递增, 且 pc 覆盖探针函数体");

    /* ---- TC-DBG-011: 空/非法现场被拒; 边界护栏生效; max 生效 ---- */
    br_uintptr_t saved_bottom = 0u;   /* 白盒保存, 用例收尾复位 */
    br_uintptr_t saved_top    = 0u;
    br_bt_internal_read_stack_bounds(&saved_bottom, &saved_top);
    br_bool ok11 = BR_TRUE;

    /* fp == 0 ⇒ 0 帧(无现场); fp 未 16 对齐 ⇒ 0 帧且不解引用(不死) */
    ok11 = ok11 && (br_bt_capture_from(0u, 0u, frames, 8u) == 0u);
    ok11 = ok11 && (br_bt_capture_from(0x1234u, 0x40080000u, frames, 8u) == 0u);

    /* 非法边界被拒(setter 先校验后写入 ⇒ 不改状态, 这里顺带验证不变式) */
    ok11 = ok11 && (br_bt_set_stack_bounds(0u, 0x1000u) == BR_ERR(BR_EINVAL));
    ok11 = ok11 && (br_bt_set_stack_bounds(0x2000u, 0x1000u) == BR_ERR(BR_EINVAL));

    /* 合法但极窄的边界: 当前栈在界外 ⇒ 捕获必须掐成 0 帧(证明边界真的被查) */
    ok11 = ok11 && (br_bt_set_stack_bounds(0x1000u, 0x2000u) == 0);
    ok11 = ok11 && (br_bt_capture(frames, 4u) == 0u);

    /* 复位到调用前的边界(可能是 0/0 = "未设置"), 不残留用例里的窄边界 */
    br_bt_internal_restore_stack_bounds(saved_bottom, saved_top);

    /* max 生效: 只要 1 帧就只写 1 帧 */
    ok11 = ok11 && (br_bt_capture(frames, 1u) == 1u);

    fails += bt_conf(ok11, "TC-DBG-011",
                     "capture_from(0/未对齐)=0, 非法 bounds 被拒, 窄 bounds 掐断, max 生效");

    return (int)fails;
}
