# app/hello

> 设计出处: `1-03` §1 —— “`app/hello` + conformance | APP | 启动链演示
> (**M0: 直接主循环, 不依赖 iface —— M0 引导例外**; 此时 Interface 插件尚未交付)”。

本插件是镜像里**唯一的 APP**(`§3.2`: `app` 恰 1), 也是 **POSIX 多线程样例**:
`hello_start()` 用 `pthread_create`(经 `iface/posix` 皮肤)建 APP 线程; 入口
`hello_mainloop()` 先跑一遍 `app_pthread_conformance()`(多线程自判), 再进周期心跳循环。

## POSIX 多线程样例(ADR-0019)

APP 只用 POSIX 面(线程/同步/延时: `pthread_*` / `sem_*` / `usleep`), 且**只经 Interface
皮肤**取得声明; 只有 `br_log_info`(观测; `printf` 属 TR-C)与 `br_clock_now`/
`br_clock_tick_count`(心跳判据读数)是 core 调用。7 项自判各打一行 `[APPCONF] PASS/FAIL`,
末尾 `[APPCONF] SUMMARY`; `smoke` 门禁逐 tag 点名(`APP-PT-001..007`)并要求 `fail=0`:

| tag | 内容 |
|---|---|
| `APP-PT-001` | 3 线程 × 500 次 mutex 保护计数 + `pthread_barrier` 起跑 + `join` 搬 retval |
| `APP-PT-002` | `condvar` 会合(主线程等全部 worker 就绪后 broadcast 放行) |
| `APP-PT-003` | `pthread_key_*` 每线程值隔离 + 退出析构 |
| `APP-PT-004` | `detach`: detached 线程照跑; `join` detached ⇒ `EINVAL` |
| `APP-PT-005` | 每线程 `errno` 隔离 |
| `APP-PT-006` | `rwlock`: 读者并发(`active_max>=2`)、写者独占 |
| `APP-PT-007` | `pthread_once` 恰一次 + `setname/getname_np` |

## 声明面(唯一真值)

| 项 | 值 | 出处 |
|---|---|---|
| `plugin_type` | `app` | 唯一业务逻辑, 恰 1且不被任何插件依赖 |
| `api_type` | `native` | 纯消费者: 只表示“代码面向 native API 编码” |
| `phase` | `app` | ② 完成点落在 APP 相(无 `init` 钩子, 只有主循环) |
| 接口单元 | **无** | APP 不抛接口面(它不提供能力) |

## 依赖面: 两条**正解边**(`app → iface/posix` + `app → iface/min`)

`app` 曾有的跨层直读/直连, 现在都不存在:

| 曾经的直读/直连 | 现在归谁 |
|---|---|
| `br_plat_timer_ticks()`(MainLoop 的 `irq_ticks=` 取值) | **core**: platform 的 timer ISR 每拍调 `br_clock_tick_notify()`, 计数由 core 持有, APP 经 `iface/min` 的 `br_clock_tick_count()` 读(ADR-0016) |
| `br_plat_name()` / `br_plat_isa()`(启动横幅) | **platform 自己**: `qemu_aarch64_start()` 打同格式的 `platform: …` 一行(ADR-0016) |
| `#include <unistd.h>` / `<pthread.h>` / `<stdio.h>`(POSIX) | **`iface/posix` 皮肤**: APP 只 `#include <iface/posix/posix.h>` 并声明 `[[dep]] iface/posix` —— 设计 §7.3/D18 的**正解边**(`app → interface`); 皮肤零状态, 它再导出 `runtime/posix#posix` |
| `#include <br/core/br_log.h>` / `<br/core/br_time.h>` / `<br/core/br_version.h>`(core native) | **`iface/min` 皮肤**(ADR-0020): APP 只 `#include <iface/min/min.h>` 并声明 `[[dep]] iface/min`; 皮肤 `form = "api"` 直通 core 头 —— 于是 **APP 源码里不出现 `<br/core/...>`**, "APP 不依赖 iface 层以下"由 `brickie check`/`build` 判红 |

⇒ `product.toml [lint].allow_edges` **保持空表**: APP 的边都是方向表里的合法边, 不需要豁免
(原先那条 `app → platform` 的 M0 引导例外已随 ADR-0016 删除)。声明与使用的对齐由
[ADR-0018](../../docs/decisions/0018-declared-dep-include-closure.md) 的"包含面 = 声明依赖
闭包"与 [ADR-0020](../../docs/decisions/0020-app-only-iface-consolidation.md) 的"APP 直连
core 头 = 红"在 `brickie check`/`brickie build` 两侧执法。

原退出条件里写的"等 `iface-min` 后改为 `app → iface → platform`"由本件兑现:
`iface/posix` + `iface/min` 就是那两个 Interface(`iface/min` 正是设计清单里的 `iface-min`)。

## 目录

```
plugin.toml          人写   ← 插件级唯一真值
src/main.c           人写   ← 三个钩子(early_init/init/start) + APP 线程体
tests/smoke.toml     人写   ← 用例声明面(与镜像里 [PLGCONF]/[SVCCONF] 的 id 一一对应)
```

M0 那个"在 APP 里直调各子系统"的旧 `br_core_main` 已经不存在了 —— 与
`core/include/br/core/br_main.h` 一起在 ADR-0005 那一刀被拆掉(`WORKAROUND(br-wa-boot-001)`
的口径就是"它的归宿不是长大, 而是被拆掉")。

★ **ADR-0008 之后 core 又有了 `br_core_main`** —— 但它是"四阶段启动链的**编排**",
不是 M0 那个 APP 替身(见 [br_main.h](../../core/include/br/core/br_main.h)):

| 原来在 M0 的 `br_core_main` 里的事 | 现在归谁 |
|---|---|
| 平台名/ISA 打印 | platform 自己的 `qemu_aarch64_start()`(APP 不再读 `br_plat_*`; ADR-0016) |
| `br_irq_cpu_init` | core 的 `br_core_main()` 阶段 ①(ADR-0008 后 `core.init` 有了独立入口) |
| `br_mem_init` / 开 MMU | core 的 `br_core_main()` 阶段 ③(memory + 地址映射的建立归 core) |
| 五个 service 插件的 init | 插件管理器按 `[[dep]]` 拓扑驱动(LATE 相, dump 最后) |
| `br_plat_irq_start` / 一致性套件 | platform 的 `start()`(中断已开)＋各插件自己的 init/start |
| 循环体 + `br_delay_ms` | **APP 线程 + `usleep`**(经 `iface/posix`; `br-wa-boot-001` ① 的调度器已接手 CPU 占用) |
| `br_log_init` / trace 环 | core 的 `br_core_main()` 阶段 ①(时钟/日志起点) |
