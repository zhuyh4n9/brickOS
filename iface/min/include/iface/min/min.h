/*
 * iface/min — core native 极简别名皮肤(Interface; 人写)
 *
 * 设计出处: `1-03` §1(iface-min: 极简别名层, 直通 native)、`1-01` §7.4(皮肤分类)、
 * §7.3(A-2: APP 只依赖 Interface 插件)。
 *
 * ★ 本件**不新增实现、不改名**: 它只是把 APP 需要的 core native 头**原样转出**,
 *   让"日志/时钟"也有一条 Interface 身份可依赖。声明只有一份真值 —— core 的头。
 *
 * ★ 消费方(APP)只 include 本头(`<iface/min/min.h>`) + 声明 `[[dep]] iface/min`;
 *   于是 APP 的源码里不再出现 `#include <br/core/...>`, 符合"A-2: APP 不依赖 iface
 *   层以下的接口"。这一条由 `brickie check`/`build` 的"APP 直连 core 头"检查执法
 *   (见 ADR-0020)。
 */
#ifndef BR_IFACE_MIN_MIN_H
#define BR_IFACE_MIN_MIN_H

/* 生命周期钩子(生成物 build/gen/iface/min/plugin_desc.c 引用)。 */
int iface_min_early_init(void);
int iface_min_init(void);
int iface_min_start(void);

/*
 * 直通的 core native 面(按需增补, 不做 core 全量别名):
 *   - `br_log.h`     日志入口与 `br_log_info/warn/error/debug` 等级宏
 *   - `br_time.h`    单调时钟(`br_clock_now`)与 tick 计数(`br_clock_tick_count`)
 *   - `br_version.h` 原型/版本标识
 * 类型(`br_u32/br_u64/br_bool/br_time_t`)随 `br_time.h` 的传递包含就位。
 */
#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_version.h>

#endif /* BR_IFACE_MIN_MIN_H */
