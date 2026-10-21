# iface/min — core 极简别名皮肤

## 它是什么
Interface 类别里给 **APP 专用**的极简门：把 `runtime/core` 的三个单元（`log` / `time` /
`version`）再转出，使 APP 的依赖边**只**落在 Interface 插件上（A-2：APP 不得直接碰
`<br/core/**>`）。

| 转出单元 | 来源（单一真值） |
|---|---|
| `min-log` | `runtime/core#log` → `core/c_core#log`（`br_log_*` + `br_log_info/warn/error/debug` 宏） |
| `min-time` | `runtime/core#time` → `core/c_core#time`（`br_clock_*`、`BR_US_PER_MS`、`BR_TIMEOUT_*`） |
| `min-version` | `runtime/core#version` → `core/c_core#version`（`BR_PROTOTYPE_NAME` / `BR_VERSION_STRING`） |

**皮肤里没有一行 core 实现，也没有一行 core 声明** —— `form = "skin"` + `reexport_of` 让声明
留在 core，皮肤只声明“这一面就是 runtime/core 的那一面”。`src/iface_min.c` 只有三个空钩子。

## 为什么是 `form = "skin"`（ADR-0038 §4 改判）
本皮肤原先是 `form = "api"`，在 `brick.toml` 里**手抄**了 20 条 core 条目。那正是要消灭的第二
真值：每次 core 改一个签名，这里就得跟一次，忘了就是“声明面与实现不一致”。ADR-0038 之后
core 有了 20 个接口单元（`core/c_core`）和唯一的出口（`runtime/core`），于是这里改成
`reexport_of`，一条都不抄。

## 与 `iface/posix` 的分工

| 皮肤 | `form` | 转出面 | 被谁依赖 |
|---|---|---|---|
| `iface/posix` | `skin` | `runtime/posix#posix`（POSIX 运行时面） | APP |
| `iface/min` | `skin` | `runtime/core#{log,time,version}`（core native 面） | APP |

## 声明面（唯一真值）

| 项 | 值 |
|---|---|
| `plugin_type` / `api_type` | `interface` / `native` |
| `symbol_prefix` | `iface_min_`（显式；short 推导是 `min_`） |
| `phase` | `late`（interface 的完成点） |
| `[[dep]]` | `runtime/core`，`kind = "type"` |
| `[[export]]` | 三个单元 `min-log` / `min-time` / `min-version`，`form = "skin"` |

## 目录

```
brick.toml                        人写   ← 插件级唯一真值(依赖/再导出单元)
include/iface/min/min.h            人写   ← 转出的 core 头 + 三个钩子原型
src/iface_min.c                    人写   ← 三个空钩子
tests/smoke.toml                   人写   ← 用例声明(自述, 不被消费)
```
