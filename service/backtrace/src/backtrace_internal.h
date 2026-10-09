/*
 * brickOS prototype v0.2.0 — service/backtrace **插件私有**内部契约(不导出, 不在 include/)
 *
 * 给谁看: 本插件自己的 `src/backtrace_selftest.c`。放在 `src/`(而不是 `include/`)是刻意的
 * —— `include/` 下的一切都是**插件对外面**(会被 `brickie check` 的接口域治理, 也进
 * `descriptor_include` 的选用范围); 而这里的两条**只有自检**需要。
 *
 * ## 为什么自检要读内部
 *   TC-DBG-011 要构造"栈边界极窄 ⇒ 捕获被掐成 0 帧"的场景, 之后必须把边界**原样复原**
 *   —— 未设置时是 (0, 0)。公开的 `br_bt_set_stack_bounds` 会拒绝 0 下界 / top<=bottom,
 *   所以"复原"没法经公开面表达; 而把"读边界 / 原样写回"做成公开 API 会把插件的内部
 *   状态(栈范围是谁设的、设成多少)塞进 golden 接口面, 那正是要避免的。
 *
 * ## 边界纪律(与 framework/vfs-core 的 `src/vfs_internal.h` 同源)
 *   只暴露自检真用到的两条, 且一律**读写生产状态本身**(生产实现里的
 *   `s_stack_bottom`/`s_stack_top`), 不在别处留第二份副本 —— 否则用例改的是副本的边界。
 *   生产代码(`backtrace.c`)的其余部分仍然是 `static`。
 */
#ifndef BR_DEBUG_BR_BT_INTERNAL_H
#define BR_DEBUG_BR_BT_INTERNAL_H

#include <br/core/br_types.h>

/* 读当前栈边界(未设置 ⇒ *bottom = *top = 0)。用例在改边界前保存现场。 */
void br_bt_internal_read_stack_bounds(br_uintptr_t *bottom, br_uintptr_t *top);

/* 原样写回栈边界(**不做任何校验**; 0/0 = 未设置)。用例收尾复原现场 ——
 * 不能用公开的 `br_bt_set_stack_bounds`: 它拒 bottom==0 / top<=bottom。 */
void br_bt_internal_restore_stack_bounds(br_uintptr_t bottom, br_uintptr_t top);

#endif /* BR_DEBUG_BR_BT_INTERNAL_H */
