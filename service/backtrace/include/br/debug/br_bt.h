/*
 * service/backtrace — 栈回溯捕获服务(对外声明面)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md` §3(mini ramdump 的"捕获集: 寄存器组 … +
 * 栈回溯"; "host 工具: 离线分析(线程时序对照 trace、fault 解码、**栈回溯**)")。
 * 因此本插件的**职责边界**是 5-01 划的:
 *   - target 侧: **捕获**(按帧指针链走栈, 得到 pc/fp/lr 三元组序列) —— 只做确定能做的事;
 *   - host 侧: **符号化**(addr2line/llvm-symbolizer 把 pc 变成 函数+偏移), 离线。
 * 原型不在镜像里塞符号表(那要两遍链接, 属 v1.x 的"离线解码"之后的优化)。
 *
 * 依赖: 编译期必须 `-fno-omit-frame-pointer`(否则 x29 链断裂) —— 已写进 Makefile 的
 * CFLAGS, 并在 README 里点名(这是本插件唯一的编译期前提)。
 *
 * ISR 安全: `br_bt_*` 全部 **thread-only**; fault 现场的捕获入口接收异常帧里的
 * fp/pc(由 platform 的 fault 分派路径传入), 不自己再触发异常。
 */
#ifndef BR_DEBUG_BR_BT_H
#define BR_DEBUG_BR_BT_H

#include <br/core/br_types.h>

#define BR_BT_MAX_FRAMES   16u   /* 单次捕获的帧数上界(静态缓冲; 不分配) */
#define BR_BT_MAX_DEPTH    32u   /* fp 链的绝对深度护栏(防环) */

/* 一帧: pc 是"下一条要执行的地址"(= 调用点的返回地址) */
typedef struct {
    br_uintptr_t pc;
    br_uintptr_t fp;
    br_uintptr_t sp;      /* 由 fp 推得(栈帧内的规范位置); 不可推 ⇒ 0 */
    br_u32       depth;
} br_bt_frame_t;

/* LATE 相 init: 清捕获快照并返回 0。**不动栈边界**(边界由 `br_bt_set_stack_bounds`
 * 在 init 之后按需设置; 本原型没有合法调用方, 见下)。
 * (也不跨插件注册 trace 事件名: 声明面没有 backtrace → service/trace 的 `[[dep]]` 边,
 *  见本插件 README 的"声明面欠账"。) */
int br_bt_init(void);

/* 自检(打印 `[DBGCONF] PASS/FAIL TC-DBG-01x <desc>`; 返回失败数)。 */
int br_bt_selftest(void);

/*
 * 告知栈范围(**可选**; 让栈走查能判断"fp 已越界", 从而在链被踩坏时停下来而不是
 * 越走越野)。未设置 ⇒ 只靠 `BR_BT_MAX_DEPTH` 与"fp 必须 16 字节对齐 + 严格递增"
 * 这三条护栏(以及"以走查者的 SP 为下界"的保守回退)。
 *
 * ★ **调用方与时机**(与 `br_bt_init` 的分工, 二者不冲突):
 *   - `br_bt_init`(LATE 相)**不动栈边界** —— 它只清捕获快照; 否则会把先设好的边界抹掉;
 *   - 本函数只能在 `br_bt_init` **之后**调用(之前调用没有意义: init 会清状态)。
 *   - **本原型里没有任何调用方**: 栈边界是 platform 的事实(`__stack_bottom/__stack_top`),
 *     而"platform → ability 插件"是设计 §7.3 的**非法依赖边**(platform 不得依赖 ability)
 *     ⇒ 为了不造未声明的跨层调用, 边界留空, 走查用保守回退。这条接口留给 v1.x:
 *     边界信息的合法来源是"服务/接口层"或将来 core 的栈元数据(栈保护区 vx.0 的同一处)。
 */
int br_bt_set_stack_bounds(br_uintptr_t bottom, br_uintptr_t top);

/* 从**当前**上下文捕获。返回写入的帧数(>=1); out==NULL/max==0 ⇒ 0。 */
br_u32 br_bt_capture(br_bt_frame_t *out, br_u32 max);

/*
 * 从给定现场捕获(异常帧 / 已知 fp)。`pc` 作为第 0 帧的 pc, `fp` 作为起点。
 * 返回帧数。护栏: fp 为 0 / 非 16 对齐 / 越出栈范围 / 非单调 / 超深度 ⇒ 停止。
 */
br_u32 br_bt_capture_from(br_uintptr_t fp, br_uintptr_t pc,
                          br_bt_frame_t *out, br_u32 max);

/* 捕获并打印: `[BT] 0: pc=0x<hex> fp=0x<hex>` 每帧一行 + `[BT] frames=<n>`。返回帧数。 */
br_u32 br_bt_print(void);
br_u32 br_bt_print_from(br_uintptr_t fp, br_uintptr_t pc);

/* 最近一次捕获的帧数(观测; 供 dump 复用同一份快照而不重复走栈)。 */
br_u32 br_bt_last_count(void);
const br_bt_frame_t *br_bt_last(void);

#endif /* BR_DEBUG_BR_BT_H */
