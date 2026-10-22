# provider/core — core 层规格皮肤

## 它是什么

core 层**唯一的对外门**。它把 core 层的 20 个接口单元（**一头一单位**）原样转出，并承载
core 层的**规范正文**（本文）。它没有实现、没有状态：三个空钩子 + 一张转出的头文件面。

> **ADR-0039 之后**这 20 个单元来自 **4 个提供者**：`core/c_core` 12 个（base + plugin manager）与
> `hal/int` 4 个 / `hal/mm` 2 个 / `hal/sched` 3 个（中断、内存、调度机制）。拆分对消费者
> **不可见** —— 它们仍然只声明 `[[dep]] provider/core`。

```
core/c_core (12 units) ─┐
hal/int     (4 units)  ─┼─[[dep]] kind="type"─▶ provider/core ──[[dep]]──▶ 其余 21 个插件
hal/mm      (2 units)  ─┤                        (20 条 reexport_of)
hal/sched   (3 units)  ─┘                              ▲
                                            iface/min ─┘ (再转出 log / time / version 给 APP)
```

## 为什么是“皮肤”，不是包装层

1. **规范有落点**：core 的命名与约定需要一个**写给人看**的家（本文），而 `core/c_core` 是实现；
   把规范和实现分开，规范才能先于实现被讨论。
2. **出口只有一处**：消费者一律 `[[dep]] provider/core`。将来 core 拆成
   `frameworks/core/{c_core,r_core}`、或换一门语言实现，改的是**这里每个单元的一行
   `reexport_of`**，不是二十多份 `brick.toml`。
3. **零行为**：皮肤不持有状态、不包一层转调。声明与定义都留在 core —— `form = "skin"`
   的语义正是“我声明的那一面**就是**它的那一面”，所以不会长出第二份真值。

## 单元表（一头一单位）

| 单元 | 头 | 提供者 | 单元 | 头 | 提供者 |
|---|---|---|---|---|---|
| `log` | `br_log.h` | core | `fault` | `br/hal/br_fault.h` | hal/int |
| `console` | `br_console.h` | core | `exc` | `br/hal/br_exc.h` | hal/int |
| `mem` | `br/hal/br_mem.h` | hal/mm | `time` | `br_time.h` | core |
| `mm` | `br/hal/br_mm.h` | hal/mm | `trace` | `br_trace.h` | core |
| `sched` | `br/hal/br_sched.h` | hal/sched | `svc` | `br_svc.h` | core |
| `sync` | `br/hal/br_sync.h` | hal/sched | `plugin` | `br_plugin.h` | core |
| `work` | `br/hal/br_work.h` | hal/sched | `types` | `br_types.h` | core |
| `irq` | `br/hal/br_irq.h` | hal/int | `main` | `br_main.h` | core |
| `pic` | `br/hal/br_pic.h` | hal/int | `error`/`errno` | `br_error.h` | core |
| | | | `version` | `br_version.h` | core |

粒度是刻意的：冻结/版本按**头**走，所以 `br_log.h` 加一个函数不会把 `br_mm.h` 的
`compat_gen` 一起顶上去。

## core 层规范（每条都指明谁执法）

| # | 规范 | 执法者 |
|---|---|---|
| C1 | 名字前缀 `br_`（类型 `_t` 后缀，宏 `BR_` 前缀）；标识符不得出现 `__` 前导 | 评审 + `BRV-SRC-0002` 的 `__` 约定 |
| C2 | 错误约定：`int` 返回负 errno（`-EXXX`），成功为 0 或正值；错误码只在 `br_error.h` 定义，禁止就地编造 | 评审 + `ft-test` 与内核 errno 逐码对拍 |
| C3 | **头文件是唯一的声明面**：`.c` 里不得出现跨翻译单元的声明（含 `extern`、链接器符号、弱符号） | `brickie check --src`：**BRV-SRC-0001** |
| C4 | 一头一单位：公共头与 `[[export]]` 单元一一对应，单元名 = 头名去掉 `br_`/`.h` | 评审（`brickie iface list` 可核对） |
| C5 | **全局变量不出插件**：文件作用域对象一律 `static`，对外只给函数 | 文本：**BRV-SRC-0002**（`check --src`）；符号：同一码由 `build` 的 `.a` 审计给出 |
| C6 | 插件的对外面只含函数 / 类型 / 宏，**不含变量**（`kind = "var"` 被拒绝） | **BRV-SRC-0002**（声明面 + 符号两层） |
| C7 | 依赖必须**显式**声明：用谁的头就必须 `[[dep]]` 谁，传递依赖不授权 | **BRV-DEP-0012**（`check --deps` 的文本面 + `build` 的 `.a` 审计） |
| C8 | 平台契约例外：core 可以用 `br/runtime/**` 而不点名某个 platform 插件（方向表不允许 core→platform） | 契约 §9 R-22；两侧实现同一例外 |

## 声明面（唯一真值）

| 项 | 值 |
|---|---|
| `plugin_type` / `api_type` | `ability` / `native` |
| `subkind` | `service`（由 `runtime/` 命名空间派生） |
| `symbol_prefix` | `runtime_core_`（显式；short 推导是 `core_`，与 `core/c_core` 的 `br_core_` 不同名） |
| `phase` | `late`（`ability` + `service` 的完成点） |
| `[[dep]]` | `core/c_core`，`kind = "type"`（纯编译期面，不产生启动边） |
| `[[export]]` | 20 个单元，全部 `form = "skin"`、`api_iface = "native"`、**无 entries**（面由 provider 派生） |
| 冻结 | **未冻结**（原型期：core 接口还会变；见 `WORKAROUNDS.md` 的 `br-wa-rel-001`） |

## 目录

```
brick.toml                                  人写  ← 20 条 reexport + 依赖 + typedefs
include/provider/core/core.h                 人写  ← 转出的 20 张 core 头 + 三个钩子原型
src/runtime_core.c                          人写  ← 三个空钩子
README.md                                   人写  ← 本文（core 层规范正文）
```
