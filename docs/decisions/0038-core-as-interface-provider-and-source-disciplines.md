# 0038 — core 一等接口化 + 三条源码纪律（R1/R2/R3）

> 状态: **已落地**（`brickie check` 0 错 0 警；`brickie build` 编译 + `ar` + **符号审计** + 链接全绿；
> `brickie test -j8` 11 条 QEMU 门禁 + 7 条宿主 + 11 条脚本门禁全绿；`make tools-test` 711/711；
> `brickie-core --selftest` 585/585）
>
> 落点: `core/c_core/brick.toml`（20 个接口单元）、**新增** `runtime/core/`（规格皮肤 + 规范正文）、
> `iface/min/brick.toml`（`form = "api"` → `skin`）、22 份插件 `brick.toml`（`[[dep]]`）、
> `tools/brickie/rust/src/{srcscan.rs,audit.rs,check.rs,model.rs,build.rs,rules.rs,solver.rs}`、
> `tools/brickie/python/brickie/cli.py`、源码整改 6 处（见 §5）、`tools/brickie/tests/run.sh`（V-22 一节）
>
> 回灌: 设计 `2-01` §7（声明面是唯一真值）、`4-02` §2（插件布局与 include 面）、
> `1-01` §6.1（core 的接口面）；契约 `docs/contract.md` §3（新码）、§5.5（`judge` 保留名）、§9 R-20–R-22

## 0. 一句话

**core 变成一等接口提供者**（一头一单位，经 `runtime/core` 皮肤对外），**三条源码纪律**由
`brickie check` 的 `src` 域（纯文本）与 `brickie build` 的 `.a` 符号审计（精确）两侧共同执法。

---

## 1. 问题：三处“工具看不见”的越权

| # | 现状 | 后果 |
|---|---|---|
| P1 | 一个 `.c` 可以只写声明不写定义；跨 TU 的声明散落在 `.c` 里（`extern` 链接器符号、弱钩子、套件入口） | “这个文件实现了什么”必须靠读遍全树才知道；改签名时编译器只在**用到**的 TU 报错 |
| P2 | 文件作用域全局变量默认可跨 TU 可见；`runtime/posix` 甚至通过公共头导出 `stdin/stdout/stderr` | 插件的对外面出现了**变量**，而变量没有版本、没有冻结、没有类型检查的边界 —— 与“接口 = 函数/类型/宏”的纪律冲突 |
| P3 | `#include` 的授权面是**声明依赖的传递闭包**（R-19），且 core 头对所有人**无条件**在 `-I` 里 | 经 B 用到 C 不声明 C；core 的面永远不需要声明 —— core 因此可以是“暗物质” |

## 2. core 一等接口化

### 2.1 一头一单位

`core/c_core` 新增 **20 个 `[[export]]`**，与 20 张公共头一一对应：`log console mem mm sched
sync work irq pic fault exc time trace svc plugin types main error errno version`。

* 条目 = 该头的**完整**公共面：函数（`sig` = `"<ret> (<参数类型…>)"`，参数名不进入 hash）、
  类型（`layout = "# {字段…}"`）、枚举（`ops` = 枚举量顺序）、宏（`value`）。
  共 143 个函数 + 49 个类型 + 1 个枚举 + 174 个宏。
* 这些条目由一次性脚本从头上抓草稿、人工复核后落库（脚本不进库、不新增 CLI 面）。
* 粒度选“一头一单位”而不是“一个大单位”：冻结/版本按头走，`br_log.h` 的改动不会顶起
  `br_mm.h` 的 `compat_gen`。
* `[iface.typedefs]` 钉住基类型拼写（`br_u32 = "unsigned int"` 等），使未来的非 C 实现只要
  归一签名相同就产出同一个 `surface_hash`。
* **不冻结、不 publish 基线**（原型期 core 接口还会变；见 `WORKAROUNDS.md` `br-wa-rel-001`）。
  代价：`check --profile release` 会因“再导出未冻结单元”报 `BRV-VER-0004` —— 如实登记。

### 2.2 `runtime/core`：规格皮肤（core 层唯一的门）

`runtime/core` 是 `plugin_type = "ability"` 的**零状态皮肤**：20 条 `[[export]]`（`form =
"skin"`，`reexport_of = ["core/c_core#<unit>"]`），**没有 entries** —— 面由 provider 派生，杜绝
第二份真值。

它同时是 **core 层规范正文的落点**（`runtime/core/README.md`）：命名、负 errno 约定、“头是唯一
声明面”“一头一单位”“全局变量不出插件”“依赖必须显式声明”，每条都写明**谁执法**（哪个 BRV 码、
哪条 gate）。

为什么不是包装层：皮肤不持有状态、不转调，只声明“我这一面**就是**它那一面”。出口只有一个的
收益是**未来可换实现**：core 拆成 `frameworks/core/{c_core,r_core}` 或换语言时，改的是这里每个
单元的一行 `reexport_of`，而不是二十多份清单。

### 2.3 谁依赖谁

| 消费者 | 声明 | 理由 |
|---|---|---|
| 21 个非 APP 插件（含 `iface/min`） | `[[dep]] runtime/core`，`kind = "runtime"` | core 的唯一门 |
| `platform/qemu-aarch64` | `[[dep]] core/c_core`，`kind = "runtime"` | 方向表把 platform 定为最低 SoC 层，**不许依赖 ability**；core 对非 core 消费者恒可依赖（`dep_direction_ok` 首条） |
| `app/hello` | 不变（`iface/posix` + `iface/min`） | A-2：APP 只见 Interface 层；`iface/min` 把 `log/time/version` 再转出 |

`iface/min` 由 `form = "api"` 改为 `form = "skin"`（三个单元 `min-log/min-time/min-version`），
删掉手抄的 20 条 core 条目。

### 2.4 单元名唯一性：从“裸单元名”改为“单元 id”

`check --iface` 原先按**裸单元名**判重，于是 `core/c_core#trace` 与 `service/trace#trace` 不能
共存。但单元 id 的定义是 `<provider>#<unit>`（§5.6），快照路径也是
`api/iface/<provider>/<unit>.toml` —— 裸名重复是合法的。本次改为按 **id** 判重（同一插件的
两个 `[[export]]` 同名仍然报红）。`brickie-core --selftest` 的对应用例同刀改判。

---

## 3. 三条源码纪律

两条执行线，都默认生效：

| 线 | 证据 | 落点 | 需要工具链 |
|---|---|---|---|
| 文本 | `.c` 的声明/定义配对 + 公共头的 `extern` | `brickie check` 的新域 **`src`**，并由 `brickie build` 的组合期自动继承 | 不需要（守住 R-13：组合期零编译依赖） |
| 符号 | 每个插件的 `build/lib/<plugin>.a` | `brickie build`：编译 → `ar` → **audit** → 链接 | 需要 `ar`/`nm` |

适用面：闭包内插件的 `[build].sources` + 产品 `[build].selftest_sources` 展开出的 `.c`，以及
`[build].includes` 下的 `.h`。**不适用**：`third_party/**` 的源、`build/gen/**`（生成物）、
`tests/**` 与 `[[hosttest]]`（测试替身）。

### R1 — 一个 `.c` 是自包含的（`BRV-SRC-0001`）

* 文件作用域的**每个函数原型**（含 `static`）必须在本文件内有同名定义；
* 文件作用域的**每个 `extern` 声明**（对象或函数）必须在本文件内有定义；
  `extern` 本身不报错 —— 报的是“声明了却没实现”。
* `typedef` / `struct` / `union` / `enum` / `_Static_assert` / `asm` 不算这条说的“声明”；
  `.h` 不受本规则约束（头就是声明面）。

### R2 — 全局变量不出插件（`BRV-SRC-0002`）

三层证据：

* **文本**：`[build].includes` 下的公共头不得出现 `extern` 对象或 tentative 定义；
* **声明面**：`[[export.entries]] kind = "var"` 一律拒绝（`var` 留在 schema 枚举里以保证形状可
  解析，纪律在 check 报码，退出码 1）；
* **符号**：`.a` 里由 B 定义的**数据符号**（`nm` 类型 `B D G S R C`）被 A(≠B) 引用 ⇒ 红；
  B 是 `native` 插件时**无视是否声明依赖**都红（对象不是接口）；弱符号不计
  （`br_plugin_gen_total` 每个描述符都弱定义一次）。

插件的对外面因此只有**函数 / 类型 / 宏**；需要暴露状态就给访问器函数。

### R3 — 依赖必须显式声明（`BRV-DEP-0012`）

* **文本**：从 A 自己的 `src/**` + `include/**` 出发，每个 `#include` 解析到的头，其 owner 必须
  在 A 的**直接** `[[dep]]` 里（或由直接依赖的 skin 再导出）；
  **传递依赖不授权** —— 经 B 用 C 就是隐式依赖 C，声明面必须写出来。
* **符号**：A 的 `.a` 里每个强 undefined 符号，定义者 B(≠A) 时：
  1. B ∈ A 的直接依赖集合（含 skin 再导出，可传递展开）⇒ 通过；
  2. 否则若该符号的**声明**出现在 A 有权包含的头（自身 / 直接依赖 / core 的 platform 契约）里
     ⇒ 通过 —— 这是 “core 声明、platform 实现”的契约面（`br_console_*`、`br_plat_ticks_*`）；
  3. 无定义者（链接器标签、libgcc 辅助）与弱符号 ⇒ 跳过。
* **`-I` 同时收紧**：`brickie build` 不再给每个单元无条件前置 core 的 `-I`。现在每个单元的
  `-I` = 自身 + **传递**依赖闭包（编译面），而**授权**按直接依赖（纪律面）。于是“漏声明
  core”从“检查报红”升级为**编译期就找不到 `<br/core/**>`**，与 ADR-0018 同一条纪律。

### 3.1 为什么链接仍然用 `.o`

`build/lib/<plugin>.a` 是**审计产物**，不参与链接。原因：`plugin_desc.c` 只有 `static const` +
弱符号、没有外部引用者，归档成员**不会**被链接器抽取 ⇒ 用 `.a` 链接会让所有描述符消失，
`KEEP(*(.br_plugins))` 无物可留。所以链接输入仍是 `.o`（语义逐字节不变），`.a` 只供 `nm` 审计。

### 3.2 audit 怎么落进 build

1. 编译组（group 0）→ `ar rcsD build/lib/<plugin>.a`（group 1，成员含该插件的描述符 `.o`）；
2. `nm -A -g <全部 .a>`（group 2，`kind = "audit"`，`judge = "symbol-audit"`，日志写
   `build/gen/symbol-audit.txt`）；
3. 链接（group 3）把审计日志列入 `consumes` ⇒ **审计判红则链接被跳过**，镜像不产出；
4. `judge` 收到的是 `nm` 文本，判定在 core（`audit.rs`）—— L5 只是执行者（V-18c）。
   `symbol-audit` 是**保留 judge 名**，不走 `tests/gates.toml`。

`ar`/`nm` 进 core 的工具表（候选序 `<cross>ar` → `llvm-ar` → `ar`，`nm` 同理），与 R-14 一致。

---

## 4. 现状实测（决定了整改量）

用树里真实 `build/obj` 的符号表与全文扫描跑过：

| 项 | 结果 |
|---|---|
| 跨插件符号边 | 46 条（其中 350 对“符号–声明”经授权） |
| 未声明依赖的符号边 | **0**（`br_console_*` / `br_plat_ticks_*` 走 §3 第 2 条契约例外） |
| 跨插件数据符号引用 | **0**（`br_plugin_gen_total` 是弱符号） |
| 文本 R1 违规 | 7 处（链接器符号 4 + 弱钩子 1 + 套件入口 5，去重后 7 个文件） |
| 文本 R2 违规 | 17 处（`plugin_mgr` 13 + `posix` 3 + 公共头 3） |
| 文本 R3 违规 | **0**（所有跨插件 include 都已直接声明） |

## 5. 源码整改（机械搬迁，不改语义）

| 位置 | 内容 | 去处 |
|---|---|---|
| `core/c_core/src/plugin/plugin_mgr.c` | 段边界 `extern` × 2；13 个外部链接全局 | 段边界 → `plugin_mgr_internal.h`；全局改 `static` + 只读快照 `br_plugin_mgr_view()` |
| `core/c_core/selftest/plugin_selftest.c` | 直读 13 个全局 | 改读快照（`v->…`），70 处机械替换 |
| `core/c_core/src/irq/irq_fault.c` | extable 段边界 × 2 | `irq_internal.h` |
| `core/c_core/src/sched/sched_core.c` | `br_sched_host_wait` 弱声明 | `sched_internal.h` |
| `core/c_core/selftest/core_selftest.c` | 5 条套件入口原型 | 新增 `selftest/selftest_internal.h` |
| `platform/qemu-aarch64/src/memmap.c` | `__image_start` / `__stack_top` | 新增 `platform/qemu-aarch64/src/linker.h` |
| `runtime/posix/{include/stdio.h,src/posix.c}` | `FILE *stdin/stdout/stderr` | `static` + `br_posix_std*()` 访问器 + `#define stdin br_posix_stdin()` |

`stdin/stdout/stderr` 三者在实现里**恒为 NULL**（没有进程模型），所以宏化不改变任何行为；
它换来的是 `.a` 里不再有对外数据符号。

---

## 6. 边界与已知限制（如实登记）

1. **文本层不做预处理求值**：`#if 0` 里的声明仍会被看见；宏拼出来的声明/定义看不见。这是
   “组合期零编译依赖”（R-13）的代价，精确的那一半由 `.a` 审计补上。
2. **多声明符语句**（`int a, b;`）只报最后一个声明符。
3. **vendor 豁免**：路径在 `third_party/**` 的源不适用；**携带 vendor 代码的插件**（
   `carries_vendor_code`：`fs/uboot-{fat,erofs}`、`driver/uboot-mmc`）豁免 R2 的**文本**条款 ——
   它们的 glue 必须给 u-boot 自己的 TU 提供全局（如 `sbi`），而“数据符号是否真的跨插件外流”
   由**符号审计**精确判定，不由文本猜测。
4. **审计是归档全量**：不判断该成员运行时是否真被 `--gc-sections` 保留。
5. **例外只有三条**：① core 用 `br/platform/**` 契约头；② `third_party/**` 路径；
   ③ `build/gen/**` / `tests/**` / `[[hosttest]]` 不适用文本规则。

## 7. 验收

| 项 | 结果 |
|---|---|
| `brickie check`（五域 `deps/iface/tax/priv/src`） | `0 errors / 0 warnings / 24 infos`（info 全是“再导出未冻结单元”，dev 等级） |
| `brickie build` | 118 → 144 步（新增 25 个 `ar` + 1 个 audit），编译 + 归档 + 审计 + 链接全绿 |
| `brickie test -j8` | 11 条 QEMU + 7 条宿主 + 11 条脚本门禁全绿（`plugin-test` 首跑出现过 TC-TASK-001 的既有
抖动，重跑绿） |
| `make tools-test` | **711 / 711**（新增 V-22 ①–⑩：三条纪律的正负例 + 审计正负例 + 管道断言） |
| `brickie-core --selftest` | **585 / 585**（新增 `srcscan` 10 例、`audit` 1 例） |
| 种子 | `make tools-prebuilt` 重发三件，`prebuilts/README.md` §5.1 同步 |

## 8. 被取代/修订的既有裁定

* **R-19 的授权口径被修订**：`-I` 仍是“声明依赖的传递闭包”，但**授权**从传递闭包改为直接依赖
  ∪ skin 再导出（本节 §3）。R-19 的其余部分（声明↔使用交叉检查、与 build 共用同一扫描）不变。
* **§2 item 4 的“单元唯一”改为按单元 id 判重**（§2.4）。
* **`iface/min` 的“手抄 core 声明”退场**（§2.3）。
