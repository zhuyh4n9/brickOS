/*
 * brickOS prototype v0.2.0 — bdev-core **插件私有**内部契约(不导出, 不在 include/)
 *
 * 给谁看: 本插件自己的 `src/bdev_selftest.c`。放在 `src/`(而不是 `include/`)是刻意的 ——
 * `include/` 下的一切都是**插件对外面**(会被 `brickie check` 的接口域治理, 也进
 * `descriptor_include` 的选用范围); 而这里的东西**只有自检**需要。
 *
 * ## 为什么自检要读内部
 *   有一条判据的**被测对象就是内部簿记**: TC-BDEV-007 要断言"槽位占用数不超过静态上界"
 *   与"槽位占用数与注册表里 BR_CLASS_BDEV 条目数一致"。公开面只给 `br_bdev_get`(按名取回),
 *   它回答不了"池里已经用了几个、上界是多少" —— 而那正是"表满 ⇒ -ENOSPC"这条结构性守卫的
 *   可观测前提。
 *   把它做成公开 API 会更糟: 容量是**实现细节**(改上界不该是接口变更)。所以只在这里暴露。
 *
 * ## 边界纪律(与 cdev-core 的 `cdev_internal.h`、core 的 `*_internal.h` 同源)
 *   这里**只**暴露自检真的用到的两样, 逐条写明用途; 且一律**转调**生产实现的同一份状态,
 *   不复制常量(否则用例比的是副本, "上界就是那个上界"根本没被验证)。
 */
#ifndef BR_BDEV_CORE_INTERNAL_H
#define BR_BDEV_CORE_INTERNAL_H

#include <br/core/br_types.h>

/* 本件的静态容量(BR_BDEV_MAX; 生产实现里的编译期常量)。
 * 用途: TC-BDEV-007 —— 断言占用数 <= 容量, 且容量 >= 1。 */
br_u32 bdev_internal_capacity(void);

/* 当前占用的槽位数(生产实现里的 static `s_bdev_n`)。
 * 用途: TC-BDEV-007 —— 与注册表里的 BR_CLASS_BDEV 条目数交叉核对(簿记不漂)。 */
br_u32 bdev_internal_slot_count(void);

#endif /* BR_BDEV_CORE_INTERNAL_H */
