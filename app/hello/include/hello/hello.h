/*
 * brickOS prototype v0.2.0 — APP 插件 `app/hello` 的对外面
 *
 * 布局依 `4-02` §2: 插件对外头在 `include/<short>/<short>.h`(本插件 = `hello/hello.h`)。
 * 本文件是**声明面锚点**: 生成物 `build/gen/app/hello/plugin_desc.c` 会 `#include` 它
 * (生成器的规则: 插件 `include/` 下声明该插件 `[[export.entries]]` 最多的头)。
 *
 * 这里只声明**生命周期钩子** —— 它们是插件管理器经 `.br_plugins` 段 + 生成物调用的
 * 入口(设计 1-01 §9 / 3-05 §2)。名字由 `symbol_prefix(short)` 推导, 不是可选的 API 面:
 * 钩子**不**进 `plugin.toml` 的 `[[export]]`(那会改接口 hash, 而钩子是组合期契约,
 * 不是插件对外能力)。
 *
 * 为什么不做成"导出 API": APP 是纯消费者(设计 §7.3: app 只经 interface 依赖),
 * 它没有任何东西值得被别的插件调用 —— 所以本文件里只有这三个钩子, 没有 `br_*`。
 */
#ifndef BR_APP_HELLO_HELLO_H
#define BR_APP_HELLO_HELLO_H

/* EARLY 相(不用堆/无线程/关中断): APP 无动作, 返回 0。 */
int hello_early_init(void);

/* CORE 相(类别决定的 ② 完成点): APP 的准备(日志等级等)。 */
int hello_init(void);

/* START 相(全局开中断后, **最后**一个 start): 自检序列 + MainLoop。不返回。 */
int hello_start(void);

#endif /* BR_APP_HELLO_HELLO_H */
