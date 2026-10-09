# iface/min — core native 极简别名皮肤

> 设计出处: `1-03` §1(`iface-min`: 极简别名层, 直通 native, `api_type = native`,
> `form = "api"`)、`1-01` §7.4(皮肤分类)、§7.3(A-2: **APP 只依赖 Interface 插件**)。
> 落地记录: ADR-0020(A-2 收口: APP 不直连 core/下层, 一律经 Interface 皮肤)。

## 它是什么

Interface 类别里的**极简别名层**: 把 APP 需要的 core native 面(日志 / 时钟 / tick
计数 / 版本标识)原样转出, 使 APP 的依赖边**只**落在 Interface 插件上。

| 转出面 | 来源(单一真值) |
|---|---|
| `br_log_set_level` / `br_log_get_level` / `br_log_write` + `br_log_info/warn/error/debug` 宏 | `core/include/br/core/br_log.h` |
| `br_clock_now` / `br_clock_freq_hz` / `br_clock_ticks_per_ms` / `br_clock_tick_hz` / `br_clock_tick_count`、`BR_US_PER_MS` | `core/include/br/core/br_time.h` |
| `BR_PROTOTYPE_NAME` / `BR_VERSION_STRING` | `core/include/br/core/br_version.h` |

**皮肤里没有一行 core 实现** —— 头文件把 core 头原样 `#include` 进来(声明只有一份真值),
`src/iface_min.c` 只有三个空钩子。

## 为什么不是 `form = "skin"`

`form = "skin"` + `reexport_of = ["provider#unit"]` 用于再导出**另一个插件**的接口单元
(如 `iface/posix` 再导出 `runtime/posix#posix`)。core **不是插件**、没有 `provider#unit`,
所以直通 core 只能是 `form = "api"`(与设计对 `iface-min` 的分类一致)。

## 与 `iface/posix` 的分工

| 皮肤 | `form` | 转出面 | 被谁依赖 |
|---|---|---|---|
| `iface/posix` | `skin` | 插件 `runtime/posix#posix`(POSIX 运行时面) | APP |
| `iface/min` | `api` | core native(日志/时钟/版本) | APP |

## 声明面(唯一真值)

| 项 | 值 |
|---|---|
| `plugin_type` / `api_type` | `interface` / `native` |
| `symbol_prefix` | `iface_min_`(显式; short 推导是 `min_`) |
| `phase` | `late`(interface 的 ② 完成点) |
| `[[dep]]` | 无(直通 core) |
| `[[export]]` | 单元 `min`(`form = "api"`,`api_iface = "native"`) |

## 目录

```
plugin.toml                        人写   ← 插件级唯一真值(导出面/条目)
include/iface/min/min.h            人写   ← 转出的 core 头 + 三个钩子原型
src/iface_min.c                    人写   ← 三个空钩子
tests/smoke.toml                   人写   ← 用例声明(自述, 不被消费)
README.md                          人写   ← 本文件
```

**不做**: core 全量别名(只按 APP 需要增补; 见 plugin.toml 头注)。ABI/符号级清单暂不
逐个声明(与 `iface/posix` 同口径: `truth = "decl"`, 提供方是 unfrozen)。
