/*
 * service/backtrace — 栈回溯捕获服务(实现)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md` §3:
 *   "捕获集: 寄存器组、TCB 全集 + **各线程栈**、trace 环 …" 与
 *   "host 工具: 离线分析(线程时序对照 trace、fault 解码、**栈回溯**)"。
 * 职责边界因此被设计划死: **target 只捕获, host 才符号化**
 *   (addr2line/llvm-symbolizer 把 pc 变成 函数+偏移) —— 本文件不携带也不生成
 *   符号表(那要两遍链接, 属 v1.x 的"离线解码"优化), 见 README 的归属边界。
 *
 * 编译期前提: 全镜像以 `-fno-omit-frame-pointer` 编译(Makefile 的 CFLAGS) ——
 *   否则 x29 会被 -O2 当普通寄存器省掉, 链断在第一帧。这是本插件唯一的编译期前提。
 *
 * aarch64 帧链约定(AAPCS64):
 *   函数序言 `stp x29, x30, [sp, #-16]!; mov x29, sp` ⇒ x29 指向保存区,
 *   `[x29]` = 调用者的 x29(上一帧 fp), `[x29 + 8]` = 返回地址(下一条要执行的 pc)。
 *   栈向低地址增长 ⇒ 沿链上行的 fp **严格递增**(这也是防环的主要护栏)。
 *
 * ISR 安全: `br_bt_*` 全部 **thread-only**; fault 现场的捕获入口接收异常帧里的
 *   fp/pc(platform 的 fault 分派路径传入), 不自己再触发异常。
 */
#include <br/debug/br_bt.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>

#include "backtrace_internal.h"    /* 自检用的内部视图(见该头; 生产路径不用它) */

/* 无边界信息时的保守回退窗口: 以走查者当前 SP 为下界、SP + 1 MiB 为上界。
 * 真实启动栈只有几十 KiB(link.ld), 1 MiB 足够覆盖全部调用者帧, 又能挡住
 * "fp 被踩成一个远处的野值"这种最坏情况。 */
#define BT_FALLBACK_STACK_BYTES  (1024u * 1024u)

/* =====================================================================
 * 内部状态
 * ===================================================================== */

static br_uintptr_t s_stack_bottom;
static br_uintptr_t s_stack_top;     /* s_stack_top == 0 ⇒ 未设置边界 */

static br_bt_frame_t s_last[BR_BT_MAX_FRAMES];   /* 最近一次捕获的快照 */
static br_u32        s_last_count;

/* =====================================================================
 * 小工具
 * ===================================================================== */

/* 本次走查的栈范围: 优先用 platform 设的边界; 否则回退到"当前 SP .. +1 MiB"。
 * 回退下界取走查者自己的 SP —— 它比任何被走查的调用者帧都低(栈向下增长),
 * 因此不会误杀合法帧; 代价是上界只能靠这个固定窗口保守估计。 */
static void bt_bounds(br_uintptr_t *lo, br_uintptr_t *hi)
{
    if (s_stack_top != 0u) {
        *lo = s_stack_bottom;
        *hi = s_stack_top;
        return;
    }

    br_uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    *lo = sp;
    *hi = sp + (br_uintptr_t)BT_FALLBACK_STACK_BYTES;
}

/* =====================================================================
 * 帧链走查
 * ===================================================================== */

/*
 * 从 (fp, pc) 起走 fp 链。第 0 帧 = 传入的 (pc, fp)。
 * 护栏(全部通过之后才允许解引用 fp, 不许先读后校验):
 *   ① fp != 0; ② fp 16 字节对齐(AAPCS64); ③ 在栈范围内;
 *   ④ 严格递增(next_fp > fp, 环/断裂即停); ⑤ 深度 < BR_BT_MAX_DEPTH;
 *   ⑥ 帧数 < max。
 * 返回写入 out 的帧数。
 */
static br_u32 bt_walk(br_uintptr_t fp, br_uintptr_t pc,
                      br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }

    br_uintptr_t lo;
    br_uintptr_t hi;
    bt_bounds(&lo, &hi);

    br_u32 depth = 0u;

    while ((depth < max) && (depth < BR_BT_MAX_DEPTH)) {
        /* ---- 解引用前的护栏 ---- */
        if (fp == 0u) {
            break;
        }
        if ((fp % 16u) != 0u) {
            break;
        }
        if ((fp < lo) || (fp >= hi)) {
            break;
        }
        if ((depth > 0u) && (fp <= out[depth - 1u].fp)) {
            break;   /* 必须严格上行 */
        }

        out[depth].pc    = pc;
        out[depth].fp    = fp;
        /* sp: fp + 16 是栈帧里的"规范位置"(保存区之后), 不是真实 SP ——
         * 真 SP 在帧内会随函数体变化, 不具可移植语义。 */
        out[depth].sp    = fp + 16u;
        out[depth].depth = depth;
        depth++;

        if ((depth >= max) || (depth >= BR_BT_MAX_DEPTH)) {
            break;
        }

        /* ---- 读上一帧 fp 与返回地址; 用 volatile 防编译器缓存(栈可能被改) ---- */
        const volatile br_uintptr_t *frame = (const volatile br_uintptr_t *)fp;
        const br_uintptr_t next_fp = frame[0];
        const br_uintptr_t ret_pc  = frame[1];

        if (next_fp <= fp) {
            break;   /* 链必须严格上行, 否则是环或已被踩坏 */
        }
        fp = next_fp;
        pc = ret_pc;
    }

    return depth;
}

/* 走查 + 落 static 快照(供 br_bt_last/dump 复用, 免得重复走栈) */
static br_u32 bt_snapshot(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max)
{
    const br_u32 n = bt_walk(fp, pc, out, max);

    if (out != s_last) {
        const br_u32 k = (n < BR_BT_MAX_FRAMES) ? n : BR_BT_MAX_FRAMES;
        for (br_u32 i = 0u; i < k; i++) {
            s_last[i] = out[i];
        }
    }
    s_last_count = (n < BR_BT_MAX_FRAMES) ? n : BR_BT_MAX_FRAMES;
    return n;
}

/* =====================================================================
 * 对外的捕获面
 * ===================================================================== */

/* noinline 是**语义要求**而不是优化提示: 帧 0 的 fp 必须是本函数自己的帧,
 * 否则 `mov x29` 读到的是调用者的帧, 帧链就少了一层(TC-DBG-010 的探针判据)。
 * 头文件的声明面不含该属性, 故只加在定义上(ABI 不变)。 */
__attribute__((noinline))
br_u32 br_bt_capture(br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }

    br_uintptr_t fp;
    br_uintptr_t lr;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    __asm__ volatile("mov %0, x30" : "=r"(lr));

    /* 帧 0 的 pc 取 lr = "回到调用者后的下一条指令" —— 与帧链里其它帧
     * (都存返回地址)口径一致, 因此不需要 +/- 4 的修正。 */
    return bt_snapshot(fp, lr, out, max);
}

__attribute__((noinline))
br_u32 br_bt_capture_from(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max)
{
    if ((out == BR_NULL) || (max == 0u)) {
        return 0u;
    }
    return bt_snapshot(fp, pc, out, max);
}

int br_bt_set_stack_bounds(br_uintptr_t bottom, br_uintptr_t top)
{
    if ((bottom == 0u) || (top <= bottom)) {
        return BR_ERR(BR_EINVAL);
    }
    s_stack_bottom = bottom;
    s_stack_top    = top;
    return 0;
}

/* =====================================================================
 * 呈现
 * ===================================================================== */

static void bt_emit(const br_bt_frame_t *fr, br_u32 n)
{
    for (br_u32 i = 0u; i < n; i++) {
        br_log_info("[BT] %u: pc=0x%lx fp=0x%lx sp=0x%lx",
                    fr[i].depth, fr[i].pc, fr[i].fp, fr[i].sp);
    }
    br_log_info("[BT] frames=%u", n);
}

br_u32 br_bt_print(void)
{
    /* 直接写 static 快照: capture 本来就会存一份, 省一个 512 B 的中间缓冲 */
    const br_u32 n = br_bt_capture(s_last, BR_BT_MAX_FRAMES);
    bt_emit(s_last, n);
    return n;
}

br_u32 br_bt_print_from(br_uintptr_t fp, br_uintptr_t pc)
{
    const br_u32 n = br_bt_capture_from(fp, pc, s_last, BR_BT_MAX_FRAMES);
    bt_emit(s_last, n);
    return n;
}

br_u32 br_bt_last_count(void)
{
    return s_last_count;
}

const br_bt_frame_t *br_bt_last(void)
{
    return s_last;
}

int br_bt_init(void)
{
    /* 清"最近一次捕获"快照。★ 不动栈边界: 边界由 platform 在 early_init 里按
     * 链接脚本符号设置, 而 init 是 LATE 相 —— 在这里清零会把那次设置抹掉。 */
    for (br_u32 i = 0u; i < BR_BT_MAX_FRAMES; i++) {
        s_last[i].pc    = 0u;
        s_last[i].fp    = 0u;
        s_last[i].sp    = 0u;
        s_last[i].depth = 0u;
    }
    s_last_count = 0u;

    /* 不跨插件注册 trace 事件名: 声明面里没有 service/backtrace → service/trace 的
     * [[dep]] 边(这一点已写进 br_bt.h 的 init 注释), 跨插件调用会引入未声明的依赖。
     *
     * ⚠ 栈边界**保持不变**: br_bt.h 的 init 注释写的是"清状态并复位栈范围", 但同一
     *    头文件里 set_stack_bounds 的注释说 platform 在 **early_init** 调用它, 而
     *    init 是 LATE 相 —— 在这里清零会把 early_init 那次设置抹掉。本实现按"不清
     *    边界"落地(两处注释的顺序自洽读法), 措辞冲突已作为待确认项回报给主控。 */
    return 0;
}

/* =====================================================================
 * 自检用的内部视图
 *
 * 见 `src/backtrace_internal.h` 的说明: 这两条**只**为 `backtrace_selftest.c` 存在 ——
 * 用例要把栈边界临时改窄再**原样复原**, 而"复原到未设置(0/0)"经公开的
 * `br_bt_set_stack_bounds` 表达不出来(它拒非法值)。★ 直接读写**生产状态本身**,
 * 不在别处留副本, 否则用例改的是副本的边界。
 * ===================================================================== */

void br_bt_internal_read_stack_bounds(br_uintptr_t *bottom, br_uintptr_t *top)
{
    if (bottom != BR_NULL) {
        *bottom = s_stack_bottom;
    }
    if (top != BR_NULL) {
        *top = s_stack_top;
    }
}

void br_bt_internal_restore_stack_bounds(br_uintptr_t bottom, br_uintptr_t top)
{
    s_stack_bottom = bottom;
    s_stack_top    = top;
}


/* =====================================================================
 * 插件生命周期钩子(名字 = symbol_prefix(short) + 相; 由生成物
 * `build/gen/service/backtrace/plugin_desc.c` 引用; 设计 1-01 §9 / 3-05 §2)
 * 自带原型满足 -Wmissing-prototypes(钩子名由生成器推导, 不进对外头与 [[export]])。
 * ===================================================================== */
int backtrace_early_init(void);
int backtrace_init(void);
int backtrace_start(void);

/* EARLY 相: 无动作(栈边界由 platform 在它的 early_init 里设置, 本插件不碰)。 */
int backtrace_early_init(void)
{
    return 0;
}

/* LATE 相(Service 类别 ⇒ ② 完成点): 现有 init 的转调。 */
int backtrace_init(void)
{
    return br_bt_init();
}

/* START 相: 本服务没有需要"中断可用/可建线程"之后才做的事。 */
int backtrace_start(void)
{
    return 0;
}

