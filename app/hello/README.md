# app/hello

> 设计出处: `1-03` §1 —— “`app/hello` + conformance | APP | 启动链演示
> (**M0: 直接主循环, 不依赖 iface —— M0 引导例外**; 此时 Interface 插件尚未交付)”。

本插件是镜像里**唯一的 APP**(`§3.2`: `app` 恰 1)。它实现 M0 的启动链演示:
一个 **APP 线程**(`hello_start()` 用 `br_task_create` 创建, 入口 `hello_mainloop`)里
"`br_task_sleep` + 日志 + trace/一致性套件"的主循环。

## 声明面(唯一真值)

| 项 | 值 | 出处 |
|---|---|---|
| `plugin_type` | `app` | 唯一业务逻辑, 恰 1且不被任何插件依赖 |
| `api_type` | `native` | 纯消费者: 只表示“代码面向 native API 编码” |
| `phase` | `app` | ② 完成点落在 APP 相(无 `init` 钩子, 只有主循环) |
| 接口单元 | **无** | APP 不抛接口面(它不提供能力) |

## M0 引导例外(可评审的例外, 不是静默放行)

`app` 直调 `platform` 违反 `§7.3` 的通则(`app ✗ platform`)。这条边**显式登记**在
`product.toml` 的 `[lint].allow_edges` 里:

```toml
allow_edges = [ ["app/hello", "platform/qemu-aarch64"] ]
```

**退出条件**: `iface-min` 交付(`1-03` 的 M2)后, 改为 `app → iface → platform`,
并把这条豁免从 `product.toml` 删掉。

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
| 平台名/ISA 打印 | 插件管理器的描述符枚举 + 本文件的启动日志(读 `br_plat_*`) |
| `br_irq_cpu_init` | core 的 `br_core_main()` 阶段 ①(ADR-0008 后 `core.init` 有了独立入口) |
| `br_mem_init` / 开 MMU | core 的 `br_core_main()` 阶段 ③(memory + 地址映射的建立归 core) |
| 五个 service 插件的 init | 插件管理器按 `[[dep]]` 拓扑驱动(LATE 相, dump 最后) |
| `br_plat_irq_start` / 一致性套件 | platform 的 `start()`(中断已开)＋各插件自己的 init/start |
| 循环体 + `br_delay_ms` | **APP 线程 + `br_task_sleep`**(`br-wa-boot-001` ① 已还清) |
| `br_log_init` / trace 环 | core 的 `br_core_main()` 阶段 ①(时钟/日志起点) |
