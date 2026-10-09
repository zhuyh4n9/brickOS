/*
 * iface/posix — POSIX 薄皮肤(Interface; 人写)
 *
 * 设计出处: `1-03` §1(iface-posix: 薄皮肤, 再导出 svc-posix)、`1-01` §7.4(skin)、
 * §7.3(APP 仅经 Interface)。原型侧实现件 = `runtime/posix`(单元 `posix`)。
 *
 * ★ 皮肤**不自己声明一份 POSIX**: 两份声明 = "同一符号两种真值"。这里只是把
 *   `runtime/posix` 的头**原样转出** —— 声明级再导出由 plugin.toml 的
 *   `form = "skin"` + `reexport_of = ["runtime/posix#posix"]` 表达(§3.5 不变量 3)。
 *   实现全是 runtime/posix 的(单一真值): 皮肤里没有一行函数体。
 *
 * ★ 消费方(APP)只 include 本头(`<iface/posix/posix.h>`) + 声明 `[[dep]] iface/posix`。
 *   编译期可见性由"声明依赖闭包 = 包含面"保证(app → iface/posix → runtime/posix;
 *   ADR-0018): APP 拿得到 POSIX 声明, 但**没有**直连 runtime/posix 的声明边。
 */
#ifndef BR_IFACE_POSIX_POSIX_H
#define BR_IFACE_POSIX_POSIX_H

/* 生命周期钩子(生成物 build/gen/iface/posix/plugin_desc.c 引用)。
 * ★ 符号前缀是 plugin.toml 里显式覆盖的 `iface_posix_` —— 本件与 runtime/posix 共享
 *   short(`posix`), 缺省推导会撞成同一个 `posix_*`(链接期重复符号)。 */
int iface_posix_early_init(void);
int iface_posix_init(void);
int iface_posix_start(void);

/*
 * 再导出的 POSIX 面(runtime/posix 提供的全部公开头)。
 * 为什么整面转出而不是只转 APP 当前用到的那几个: 皮肤的形状应当跟提供方的面走,
 * 否则"接口面"会随某个消费者漂移。裁剪是**选择**(不选本件)的事, 不是皮肤的事。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#endif /* BR_IFACE_POSIX_POSIX_H */
