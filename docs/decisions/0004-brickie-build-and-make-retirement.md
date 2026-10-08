# 0004 — brickie 构建功能(声明面驱动的镜像组合)与 `Makefile` 退役

> 状态: **已落地**(S1–S3 + S4 的入口切换; 见 §2.8)
> 影响面: `tools/brickie/**`(新命令族)、`product.toml` / `platform/*/plugin.toml`(新声明面)、
> `tests/gates.toml`(新文件)、顶层 `Makefile`(降级)、`tools/check-build.sh`(判据重写)、
> `WORKAROUNDS.md`(`br-wa-entry-001` / `br-wa-toolchain-001` / `br-wa-mem-001`)
> 设计依据: `Design/docs/decisions/0003-build-ownership-makefile-retirement.md`(S0–S4 台阶)、
> `2-02` §1(职责表)/§3 BR-D4(构建后端边界)/§7(M0–M5)、`brickie-v0.1.md` §0(边界纪律)/§7.1(`[build]`)
> 相关: ADR-0002(自洽工具集)、`brickie-v0.1.md` 的实现契约 `tools/brickie/docs/contract.md`

## 1. 背景

`br-wa-entry-001` 记了半年的一句话是: **"镜像的源码集合、编译标志与链接脚本由手写 `Makefile`
决定"**。它的根因不是"make 不好用", 而是 `2-02` §1 把"插件发现 / manifest 解析 / 闭包求解 /
拓扑与环检测 / 生成物 / **构建编排**"整块划给了 `brickie`, 而那块能力当时不存在 —— 于是
`Makefile` 里出现了 `CORE_SRCS := $(wildcard core/src/*.c) …`、`PLAT_SRCS := …`、`INCLUDES := -I…`
这样一批**本该从声明面算出来**的字面量。

`Makefile` 表达不了的东西是具体的: 依赖闭包、`compat_gen` 精确匹配、分类学禁则、相位单调、
资源预算、以及"哪些源码进镜像"。继续让它做组合, 等于把这些判断要么写死、要么丢掉。

设计侧 `Design/docs/decisions/0003` 给了一条可执行时间线: S1(`Makefile` 生成物化)→ S2(后端可换:
make 或 ninja, 出**逐字节一致**的镜像)→ S3(入口换成 `brickie build`)→ S4(`Makefile` 只编 tool)。
本 ADR 是那条时间线在原型上的落点与裁定。

## 2. 决策

### 2.1 命令族与"边界纪律"的准确表述(裁定 R-13)

新增 `brickie` 的第二族命令:

| 命令 | 干什么 |
|---|---|
| `brickie build [--profile] [--jobs] [--backend direct\|make\|ninja] [--tag] [--dry-run] [--force] [--emit-backends] [--no-post]` | 组合期 check → 生成物 → 编译/链接/objcopy → 构建后门禁 |
| `brickie test [name] [--list]` | 跑 `tests/gates.toml` 声明的门禁(宿主用例 + QEMU 日志判据 + 脚本) |
| `brickie run` / `size` / `disasm` | 先构建再跑 QEMU / 看体积 / 反汇编 |
| `brickie clean [--all]` | 清镜像侧派生品(不动 `build/host` 的工具) |

`brickie-v0.1.md` §0 的边界纪律写的是"v0.1 的任何命令**不得**要求 `cc`/`cargo`/`nm` 在场"。
**本刀显式偏离它**, 并把准确表述写进 contract §9:

> 该纪律约束的是**组合期命令**(`check`/`dep`/`iface`/`ver`/`gen`/`new`/`init`) —— 它们是
> "声明面完备性检查器", 保证组合逻辑自洽, 不保证编得过。构建族(`build`/`run`/`test`/…)的
> **职责就是**驱动交叉工具链; "要求编译器在场"是它的定义, 不是纪律的例外条款。

于是 CI 的"零编译依赖"作业仍然只跑组合期命令 + 验证器自检; 构建作业才有工具链前置。

### 2.2 声明面: 一处事实一个可写处

| 事实 | 写在哪 | 为什么是这里 |
|---|---|---|
| 选哪些插件 | `product.toml [select].plugins` + 各插件 `[[dep]]` 闭包 | 已是既有唯一真值 |
| 每个插件怎么编(源/包含/宏) | 各插件 `plugin.toml [build]` | 插件自述(`§9.1` 规则 1) |
| **目标事实**(arch / 交叉前缀 / arch 标志 / 链接脚本 / QEMU 型号) | **platform 插件**的 `[build.target]` | 每镜像恰一个 platform(§8.3 ④); "平台是什么"本来就包含这些 |
| **产品策略**(编译/汇编/链接标志、产物落点、核心本体源集合、profile 追加标志) | `product.toml [build]` / `[build.release]` | 产品级选择与预算的既有归属 |
| **门禁的判据**(宿主用例 / 日志正则 / 脚本) | `tests/gates.toml` | 判据以前散在 `Makefile` recipe 里, 那是第二处真值 |
| 生成物(`build/gen/**`) | 由 `brickie gen` 产出, `[build].gen_sources` 决定是否进镜像 | 生成物不手改(C6) |

`core/`(非插件)的源集合只能由产品声明 —— `[build].core_sources`。这一点如实反映了设计现状:
core 不是插件, 没有自己的 manifest(`[compat].core` 只钉版本区间)。

### 2.3 core 出计划与判据, L5 只执行(裁定 S-B1/R-14/R-16)

`brickie-core build` 返回的是**具体命令**: `data.steps[i] = {label, kind, argv, cwd, group, log,
timeout_s, expect_timeout, judge, stdout_lines, outputs, fail_code, tool_code}`。L5(`runner.py`)只做
四件事: 校验可执行、并发编排(同 `group` 并发, 组间串行)、超时/收日志、呈现进度。

由此派生的三条裁定:

* **S-B1(工具解析在 core)**: 工具候选序(`<cross>gcc` → `<cross>gcc-16…` → `cc`)与**解析结果**都在
  core。理由很硬: 生成物 `build/gen/build.mk`/`build.ninja` 必须把解析结果**烧进去**(make/ninja
  自己不会去猜 `aarch64-linux-gnu-gcc-16`); L5 只执行 argv。
* **S-B5(门禁判据在 core)**: `judge` 命令按 `tests/gates.toml` 的正则判日志, 返回诊断与 `failed`
  计数。L5 不作红绿判断 —— 于是"是否通过"只有一处真值, `--json`/文本/CI 退出码同源。
* **S-B6(生成物归属)**: `build/gen/build.mk`、`build/gen/build.ninja`、`build/gen/build-state.json`
  的**正文**由 core 产出(`files[]`, `kind = "machine"`), L5 只落盘(§9.1.1 写路径纪律)。

### 2.4 增量口径(裁定 S-B2)

两条判据, 与 make/ninja 同族:

1. **命令指纹**: `argv` + `kind` 的 sha256 变了(改标志、换编译器、改落点) ⇒ 重跑;
2. **时间戳**: 目标不存在, 或任一输入(源 + `.d` 里列的头 + 链接用的对象/链接脚本)比目标新,
   或输入不见了 ⇒ 重跑。

外加一条显式传播: **同一个计划里上游要重建 ⇒ 下游必须重建**。它不能省 —— 计划是在构建**之前**
算的, 那时对象文件还是旧的, 光看时间戳会得出"链接不用跑"的错误结论。

为什么不用"输入集合 + 内容戳"(第一版的做法): `.d` 是**构建的产物**, 首次编译后才存在。按输入
集合算戳 ⇒ 头一次构建之后的两次运行必然不一致 ⇒ **头依赖永远多编一轮**(实测: 干净构建后第二次
仍会重编 30 个文件)。时间戳口径没有这个自指。

### 2.5 三后端与 BR-D4 的 A/B(裁定 S-B3/S2 判据)

BR-D4 的两个备选在本刀**同时落地**, 且用同一份计划渲染:

| 后端 | 谁干活 | 生成物 |
|---|---|---|
| `direct`(缺省) | brickie 自己编排(direct 执行 steps) | 状态文件 `build/gen/build-state.json` |
| `make` | `make -f build/gen/build.mk`(BR-D4 A) | `build/gen/build.mk` |
| `ninja` | `ninja -f build/gen/build.ninja`(BR-D4 B) | `build/gen/build.ninja` |

`--tag <t>` 把全部落点整体挪开(`build/obj-<t>`、`build/brick-<t>.elf`), 于是三种后端可以并存比对。
**S2 的出口判据是"两种后端产出的镜像逐字节一致"** —— 实测三者的 ELF 与 BIN 全部逐字节相同(§5.6)。

### 2.6 门禁声明面与正则子集(裁定 S-B4)

`tests/gates.toml` 有三张表: `[[hosttest]]`(宿主可执行用例)、`[[gate]]`(QEMU 限时跑 + 日志判据)、
`[[script]]`(脚本门禁), 外加 `[build].post`(构建成功后自动跑的短名单)。判据词汇: `require`(必须
出现的正则)、`forbid`(不得出现的正则)、`require_tags`(必须打出的 `PASS <tag>` 用例 tag —— 裁掉
用例也算红, 与旧 `irq-test` 同一纪律)。

**如实声明正则子集**: 支持 `.` `*` `+` `?` `[...]`(`^` 取反、`a-z` 区间)与 `\x` 转义;
**不支持**分组/交替/锚点(`( ) | ^ $ { }`)。理由: 判据只需要"日志里有没有这么个形状", 引一个正则
库会多一条工具依赖(§9.3 最小依赖集)。代价必须**如实拒绝**而不是静默不匹配 —— 写了子集外的元字符
⇒ `BRV-BLD-0012`, 退出码 2。所以"三种 CONF 的 FAIL"写成三条 `forbid`, 而不是一条交替正则。

### 2.7 profile 与 `--release`

`--profile release` ⇒ (a) 组合期校验用 `check --profile release`(设计 `brickie-v0.1.md` 第 885 行
明确要求"release 构建必须强制以 release 完成检查"); (b) `[build.release]` 的 `*_extra` 标志追加。
**诚实说明**: 本原型的 release 与 dev 目前只差检查严格度 —— 镜像内的断言轴(`1-01` §13 的
debug=panic / release=trace)尚未落地, 是留待项(§4)。

### 2.8 `Makefile` 退役路径的实际落点

| 步 | 判据 | 状态 |
|---|---|---|
| S0 | `make` / `make smoke` 全绿 | ✅(ADR-0003 之前已达成) |
| S1 | 镜像规则不再手写; `build/gen/**` 由 brickie 产出 | ✅ 编译输入由 `[build]` 声明面算出; `build.mk`/`build.ninja` 是生成物 |
| S2 | 后端可换且镜像逐字节一致 | ✅ 实测三后端一致 |
| S3 | CI 入口 = `brickie build`(覆盖 check + 生成 + 编译) | ✅ `build` 内部先 `check` 再 `gen` 再编译 |
| S4 | 顶层 `Makefile` 只编 tool | ✅ 缺省目标是工具; 镜像目标降级为**过渡别名**(委派给 brickie) |

`br-wa-entry-001` 的"Makefile 直编"这半条由此**注销**; 剩下的是"描述符段驱动的启动链",
由 ADR-0005(插件管理器)接手。`br-wa-toolchain-001` 的**位置**从 `Makefile` 迁到
`product.toml` + platform 的 `[build.target]`(交叉前缀的声明处), 工具解析顺序在 core。

### 2.9 新诊断码族 `BRV-BLD-*`(登记)

`BRV-D8` 的编码表没有 BUILD 族, 本刀新开一族并逐条登记在 contract §9:

| 码 | 含义 | 退出码 |
|---|---|---|
| `BRV-BLD-0001` | `product.toml` 缺 `[build]`(或整个 product 缺席) | 2 |
| `BRV-BLD-0002` | `[build].sources` 展开后为空 | 2 |
| `BRV-BLD-0003` | 没有 platform 声明 `[build.target]` | 2 |
| `BRV-BLD-0004` | 多个 platform 声明 `[build.target]`(谁是真值不明) | 2 |
| `BRV-BLD-0005` | 工具解析失败(交叉编译器 / objcopy / QEMU 不在场) | 2 |
| `BRV-BLD-0006` | 闭包内的插件没有 `[build]`(会被组合却没说怎么编) | 2 |
| `BRV-BLD-0007` | `[build].sources` 里的**字面**路径不存在(通配不匹配不算错) | 2 |
| `BRV-BLD-0008` | `tests/gates.toml` 里没有这个门禁(或整文件缺席) | 2 |
| `BRV-BLD-0009` | 门禁判红(缺 require / 命中 forbid / 缺 PASS 用例 tag) | 1 |
| `BRV-BLD-0010` | `[build]` 的形状问题 | 2 |
| `BRV-BLD-0011` | 执行的步骤失败(编译/链接/脚本非零退出) | 1 |
| `BRV-BLD-0012` | 门禁正则超出 core 支持的子集(拒绝而非静默) | 2 |

## 3. 被否决的替代方案

| 方案 | 评价 |
|---|---|
| ❌ 继续让 `Makefile` 做组合, 只把 `brickie` 的闭包结果"告诉" make | 判断仍在两处: `Makefile` 要维护源集合, 而 `[select]` 也要 —— 漂移只是时间问题。且 `2-02` §1 已经把这块划给 brickie |
| ❌ `brickie` 只生成 `build/gen/Makefile`, 自己**不**执行(纯 BR-D4 A) | 少了 S2 的"后端可换"就无法证明"生成的构建输入是对的"; 而且用户可见入口仍是 make, 达不到"替代 make" |
| ❌ 直接引一个正则库(PCRE/regex crate)做门禁判据 | 与 §9.3 最小依赖集冲突; 判据模式很短, 自定义子集 + **如实拒绝**已足够 |
| ❌ 增量用"输入内容 hash" | 见 §2.4: `.d` 是构建产物, 输入集合自指 ⇒ 头依赖反复重编 |
| ❌ 让 L5(Python)自己拼编译命令 | 违反 `§9.1.1`(机器文件正文归 core)且会让"生成物里烧的是哪条命令"失去唯一真值 |
| ◐ `[build]` 放 `product.toml` 一处(不引入 `[build.target]`) | 合法停靠点, 但会让"目标事实"与"产品策略"混在一张表里; platform 插件才是"平台是什么"的主人 |
| ◐ ninja 缺席就退到 direct | 已按此实现: `make`/`ninja` 是**可选**后端; `direct` 不需要任何外部构建工具 |

## 4. 后果与留待项

* **手写构建入口只剩一个**(编 tool); 仓库里其余构建输入都是声明面或生成物。镜像的组合语义
  (闭包/分类学/预算/相位)第一次真的**参与**了镜像构建 —— 在此之前它们只影响 `check` 的红绿。
* **`Makefile` 是过渡别名**: `make run`/`smoke` 之类仍在, 但它们只是转调 brickie。彻底删掉它们的
  时机 = 所有开发者与 CI 都改用 `brickie *`(设计 ADR-0003 §6 的"缩减期两条路径并存")。
* **宿主编译器的自洽性**(设计 ADR-0003 §5-2 / ADR-0004 的种子): `make` 仍需宿主编译器来编 tool;
  与本次改动无关, 但 S4 之后它是唯一的外部宿主依赖。
* **留待项**:
  1. 镜像内的断言轴(`1-01` §13 的 debug=panic / release=trace)—— 于是 `--release` 目前只差检查
     严格度与 `[build.release]` 声明的追加标志;
  2. `[build].gen_sources` 在 ADR-0005(插件管理器)落地后才真正非空 —— 生成物进镜像的链路已就绪,
     但描述符的字段完备性(依赖边/类别)属那条线;
  3. 每插件独立编译参数(现在只有 `sources/includes/defines`), 与"per-plugin arena 预算"同属 v2;
  4. `br-wa-toolchain-001` 内部工具链就绪后, 只改 `[build.target].cross` 即可(构建规则不动);
  5. `BRV-D8` 编码表需要补 BUILD 族(设计侧回灌项)。

## 5. 门禁抓到的真问题(都是"编得过但跑不对"那一类)

### 5.1 产物父目录没人建(新源码目录必炸)

`direct` 后端第一次跑"新增了一个源码目录"的构建时, 编译器报
`fatal error: opening dependency file build/obj/core/src/sched/sched_core.o.d: No such file or directory`。
旧 `Makefile` 有 `@mkdir -p $(dir $@)`, 而我的执行器假设"目录都在" —— 只有 `build/obj` 里恰好已有
全部旧目录时才看不出来。修法: 执行器按 `steps[i].outputs` 逐级 `mkdir -p`(这是**编排**, 不是判定 ——
core 只声明"这一步写出哪些文件")。教训与 ADR-0003 §5 的同款: **只有从零构建才暴露的问题**,
所以"干净树全链路"必须是一条门禁, 而不是偶发的手工验证。

### 5.2 ninja 后端: `$cc` 未定义 ⇒ 每条命令都从 `-std=c11` 开始

生成的 `build.ninja` 里 `command = $cc -std=c11 …`, 但我漏了顶部的 `cc = <解析结果>` ⇒ ninja 把
`$cc` 展开成空串, `/bin/sh: 1: -std=c11: not found`(32 条全挂)。教训: **生成物的关键行要有门禁**
(`selftest` 里已钉住 `nj.contains("cc = ")` 与 `mk.contains("-include $(DEPS)")`)。

### 5.3 增量口径的自指(§2.4)

第一版"输入集合 + 内容戳"让干净构建后的**第二次**运行仍重编 30 个文件 —— 因为 `.d` 是那一轮才生成的。
改成时间戳口径后, 稳态 `steps_todo = 0`, 且改一个被广泛包含的头(`br_log.h`)后重编 14 步并**一次收敛**。

### 5.4 注释里的 `*/`(同一个坑第二次踩)

`sched_internal.h` 的文件头注释里写了 `core/src/sched/*.c` —— `*/` 提前闭合注释, 编译器报
`error: '/*' within comment`。这与 ADR-0003 记的 `TC-MEM-*/TC-MM-*` 是同一个坑: **注释里不要出现
`*/` 序列**。仓内的自查手段目前只有编译器; 这类坑不值得单开门禁(编译器 100% 抓得住), 但值得写进文档。

### 5.5 生成物的 glob 与 `[select]` 是"两处真值" ⇒ 由工具对齐(裁定 S-B7)

`[build].gen_sources` 是产品级的 glob, 而"哪些插件进镜像"由 `[select]` 的闭包决定 ——
两者一旦不一致, 就会出现最坏的一类错: **插件树里有、但没被产品选中的插件, 它的描述符
被编进镜像, 而它的 `.c` 不会** ⇒ 链接期未定义符号(本刀的 F1 实测踩到, 当时 `sched/coop`
已在插件树里但还没进 `[select]`)。

修法不是"要求人写对 glob", 而是工具对齐: `gen_sources` 展开后**按闭包过滤**, 被跳过的
生成物留一条 `BRV-BLD-0013` 的 **info**(不是错误 —— 那是开发期的正常状态, 但必须看得见)。
同一批改动还给 glob 加了 `**`(跨任意层目录, 可匹配零层): 生成物路径的层级 = 插件名的
命名空间深度, 写 `build/gen/*/*/plugin_desc.c` 等于把"能不能发现生成物"绑死在"插件名恰好
两段"上 —— 那是巧合, 不是契约。

### 5.6 装载期的 info/warning 曾经被静默丢弃(bug)

`load_ctx()`(出计划的那一步)走 `Ok` 分支时把装载期收集的诊断**丢掉了** —— 也就是说
"能编、但按纪律不该编"的东西永远看不见(5.5 的那条 info 正是因此第一次没出现)。现在把
`Diags` 一并返回并由各调用方 merge。这条的教训很通用: **"计划成功"不等于"没有话要说"**,
诊断的收集面与判定面必须一样宽。

### 5.7 门禁判据要钉在判据上, 不是钉在判据可能用到的词上

`smoke` 的 `forbid = ["EARLY"]` 本意是抓"延时早醒"(MainLoop 的延时自检会打 `... us: EARLY)`),
但插件管理器新增的启动横幅里写了 `(EARLY/CORE/LATE -> irq on -> START)` ⇒ **误伤**, smoke 假红。
修法: 判据精确到 `us: EARLY\)`, 并**顺手加强**为一条正向 require `us: ok\)`
("延时自检真的被测过且通过"比"没出现失败字样"强得多)。
门槛上的一句话: **裸词禁则是最脆的判据** —— 它对"别的模块也开始用这个词"零容忍, 而那种
变化本身是正确的。

### 5.8 骨架链路:`brickie init` 出来的产品以前根本编不了

`templates/product/c/product.toml.tmpl` 里**没有 `[build]`** ⇒ 新建的产品跑 `brickie build`
直接 `BRV-BLD-0001`, 而且没有任何提示("你还差什么"完全靠猜)。本刀补齐:
* 产品骨架给一份**能编**的 AArch64 裸机缺省(`[build]` 的 22 条 cflags 逐条写了理由,
  外加 `core_sources`/`core_includes`/`gen_sources`/`obj_dir` 与 `[build.release]` 的位置);
* 四个插件骨架里过期的 `[build] # v0.1 只记录; v0.3 起被消费` 改成事实;
* platform 骨架补 `[build.target]` 的**注释形状**, 并写明"不填就报 `BRV-BLD-0003`" ——
  模板**不能替人知道** SoC 的事实, 但必须告诉人还差什么。
端到端自证: 临时目录里 `brickie init demo` + `brickie new platform platform/qemu-virt` ⇒
两份 TOML 均可解析、`brickie check` 全绿、`brickie build --dry-run` **只**报 `BRV-BLD-0003`
(正是注释里指出的下一步)。

### 5.9 失败之后不许"拿旧产物接着跑"

执行器最初的语义是"失败不中断整批"(同组跑完, 便于一次看全错误), 但它有个副作用: 编译失败后
`ld` 仍会拿**上一次的旧对象**链接成功并打印 `ok ld` —— 退出码是对的, 日志却在骗人(而人看日志)。
修法: core 给每个步骤加 `consumes`(它消费哪些"本计划里别的步骤产出的文件"), 执行器据此
**短路下游**: 上游失败 ⇒ 下游标记 `skip <label> (上游步骤失败, 未执行)` 并计为失败。
直接单测: 编译步骤 `false` ⇒ `ld` 与 `objcopy` 双双 skip, 整批红。

### 5.10 "按名字忽略"的规则吃过一次夹具(`.gitignore` 的 `build/`)

`.gitignore` 里的 `build/` **没有锚点** ⇒ 它匹配任意深度的 `build/`。于是用例夹具
`tools/brickie/tests/fx/build/`(V-B 构建族的输入树)被**静默**忽略: 本地一切正常、
`git status` 也看不出异常 —— 但只要 clone 出来就没有它, `tools-test` 立刻红。
修法: 锚定到根(`/build/`), 并显式列出工具自己的产物目录(`/tools/brickie/cxx/build/`)。
教训: **凡"按名字忽略"的规则, 先问一句"会不会有别的正当目录也叫这个名字"**;
而"夹具目录名"与"产物目录名"撞车是迟早的事。

## 6. 实测证据(全部来自本刀的真跑)

| 判据 | 命令 | 结果 |
|---|---|---|
| 组合期声明面(dev + release) | `brickie check` / `brickie check --profile release` | 0 错误 / 0 警告 / 0 提示(四域全零); 闭包 8 个插件 |
| 生成物可复现 | `brickie gen --check` | 8 个文件逐字节一致 |
| 全量构建(干净树) | `brickie clean --all && brickie build -j16` | **46 步**(44 源 + 链接 + objcopy), 0 失败 |
| 增量收敛 | `brickie build -j16` 连跑 | `steps_todo = 0`; `touch core/include/br/core/br_log.h` 后 14 步, 再跑即 0 |
| **S2 后端一致** | 干净树上 `brickie build` / `--backend make --tag mk` / `--backend ninja --tag ninja` | 三个 ELF 与 BIN **逐字节一致**(实测) |
| S2 的**口径**修正 | 同上, 但在"增量树"上先跑一个后端、再跑另一个 | 中途若有任何输入变化, 各后端的对象各自重编 ⇒ **跨后端比对只在"同一份输入"下有意义**。所以门禁要把 S2 放在**干净树**上(或先 `clean`); 否则量到的是"两次构建之间有没有改过东西", 不是后端等价性 |
| 门禁(全部经 brickie) | `brickie test -j16` | **18 条全绿**: 宿主 `string-test`/`mem-test`/`sync-test`/`sched-test` + QEMU `smoke`/`irq-test`/`dbg-test`/`sync-test`/`plugin-test`/`sched-test` + 脚本 `check-string`/`check-headers`/`check-workarounds`/`check-build` |
| 构建族五条命令的成功路径 | `brickie build` / `test` / `size` / `disasm` / `run` | 全通: `size` = `text 113312 / data 101 / bss 121632`; `run` 的日志里能看到管理器逐相驱动 |
| 领域自检 | `brickie-core --selftest` | **491 cases** 全绿(含构建族的规划/码/门禁/正则/后端渲染/`**`/闭包过滤) |
| 工具自身用例 | `bash tools/brickie/tests/run.sh` | **通过 632 / 失败 0**(含 V-B 构建族 96 条) |
| 领域一致性(目标侧) | `build/logs/{smoke,plugin,sched,sync}.log` | `[PLGCONF] 6/0`、`[SVCCONF] 3/0`、`[TASKCONF] 12/0`、`[SYNCCONF] 12/0` —— 每条 gate 日志里四套摘要都在 |
| 后端一致性 + 增量在**真链**上的表现 | `brickie test smoke` | 609 行日志 / 169 PASS; `tick=2` 落在 2.488 s(smoke timeout 3 s) |

> 门禁日志的三条摘要读数(`[IRQCONF] 136 PASS` / `[MEMCONF]` / `[DBGCONF]`)与旧 `Makefile` 路径
> 完全一致 —— 这正是"换入口不换语义"的证据。
