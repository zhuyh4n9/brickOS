# app/hello

> 设计出处: `1-03` §1 —— “`app/hello` + conformance | APP | 启动链演示
> (**M0: 直接主循环, 不依赖 iface —— M0 引导例外**; 此时 Interface 插件尚未交付)”。

本插件是镜像里**唯一的 APP**(`§3.2`: `app` 恰 1)。它实现 M0 的启动链演示:
`br_core_main()` 里的延时 + 日志主循环。

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
src/main.c           人写   ← MainLoop(M0 的启动链演示)
tests/                       (暂空: M0 的用例由 `6-01` 的 conformance 首版承担)
```

`br_core_main` 的**声明**仍在 core 头文件 `br/core/br_main.h`(M0 的入口契约),
实现随 APP 走 —— 这正是 `WORKAROUND(br-wa-boot-001)` 说的“`br_core_main` 的归宿
不是长大, 而是被拆掉”: 循环体变成 `app.start()` 里的 APP 线程, 循环边的
`br_delay_ms` 变成 `br_task_sleep`(M1)。
