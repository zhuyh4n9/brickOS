/*
 * brickOS prototype v0.2.0 — service/trace **插件私有**内部契约(不导出, 不在 include/)
 *
 * 给谁看: 本插件自己的 `src/trace_selftest.c`。放在 `src/`(而不是 `include/`)是刻意的 ——
 * `include/` 下的一切都是**插件对外面**(会被 `brickie check` 的接口域治理, 也进
 * `descriptor_include` 的选用范围); 而这里的两条**只有自检**需要。
 *
 * ## 为什么自检要读内部
 *   TC-DBG-001 有两条判据的**被测对象就是内部机制本身**, 用公开 API 表达不出来:
 *     - "两个 tag 的初始槽位相同"是"线性探测真的被走到"这条断言**成立的前提**, 用例要
 *       用 `trc_slot_of` 复核它(否则哪天 hash 一改, 这条断言会悄悄退化成空转);
 *     - 名字解码要比对**逐字节**内容(公开的 `br_trace_svc_name` 只回 `const char *`)。
 *   为了这两条把它们做成公开 API 会更糟: hash 槽位布局进了 golden 接口面, 以后动
 *   实现就是接口变更(而它明明是内部机制)。
 *
 * ## 边界纪律(与 framework/vfs-core 的 `src/vfs_internal.h` 同源)
 *   只暴露自检真用到的两条, 且一律**转调生产实现**(不复制逻辑): 用例必须跑真算法,
 *   否则验证的是副本。生产代码(`trace.c`)的其余部分仍然是 `static`。
 */
#ifndef BR_DEBUG_BR_TRACE_INTERNAL_H
#define BR_DEBUG_BR_TRACE_INTERNAL_H

#include <br/core/br_types.h>

/* 槽位下标(生产实现是 static trc_slot_of: FNV-1a hash 后对槽位数取模)。
 * 用途: 用例断言两个 tag 的**初始**槽位相同 —— 撞槽前提不成立时先报出来。 */
br_u32 br_trace_internal_slot_of(const char *name);

/* 字符串比较(生产实现是 static trc_streq): 用例用它逐字节核对解码出的名字 == 注册 tag。 */
br_bool br_trace_internal_streq(const char *a, const char *b);

#endif /* BR_DEBUG_BR_TRACE_INTERNAL_H */
