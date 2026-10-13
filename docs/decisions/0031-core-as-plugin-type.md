# 0031 — `core` 成为**第一类插件**: 新增 `plugin_type = "core"`, 必须有且仅有一个, 不定义生命周期

> 状态: **已落地**(工具侧 `rules.rs`/`model.rs`/`solver.rs`/`plan.rs`/`build.rs` + schema + 模板;
> 目标侧 `br_plugin.h`(新常量)+ `plugin_mgr.c`(运行期基数自证 + 删掉 selftest 旁路);
> 声明面 `core/c_core/brick.toml`(新)+ `product.toml`。
> 判据: `make tools-test`(644 例全绿: 含 `brickie new core` 的 V-1 用例、solver 的
> `core` 正/负例与重发的协议快照; 另有 `brickie-core --selftest` 514 例)、
> `make smoke`/`plugin-test`/`irq-test`/`dbg-test`/`sched-test`/`sync-test`/`fs-test`/`posix-test`
> 全绿, `check-build`/`check-workarounds`/`check-headers` 一致。
> 取代: **ADR-0001 裁定 4("core 不是插件")** 与 **ADR-0004 §2.2("core 不是插件, 没有自己的
> manifest;`[compat].core` 只钉版本区间")**。这两条里"core 不在插件树"的事实被本刀推翻;
> 它们保留的其他裁定(目录即插件、声明面唯一真值、Makefile 只编工具)不变。
> 相关: ADR-0005(插件管理器/相位数轴)、ADR-0008(core 启动链)、ADR-0010(自检)、
> ADR-0016(心跳收归 core)、ADR-0017(HZ)、ADR-0018(包含面 = 声明依赖闭包)、ADR-0020(APP 只依赖 Interface)。

## 1. 背景: 缺的不是"core 要不要被声明", 而是"驱动者没有身份"

上一版的形态是: 插件有类型(app / interface / ability / platform)、有描述符、有相位、有闭包与预算;
**只有 core 什么都算不上** —— 它由 `product.toml [build].core_sources` / `core_includes` 两个字段
(全局源集合)编进镜像, 被 `[product].core` 的版本区间"引用", 却没有 manifest、没有类型、不进依赖图。
三处代价:

1. **声明面有唯一的破例**: 其余每个组合件都在自己的 `brick.toml` 里自述"我怎么编、我依赖谁",
   core 的构建事实却挂在产品上。于是 `[budget]`(Σ `[[res]]`)、依赖图、接口治理都**看不见**它,
   而它其实是静态 RAM 的大头之一。
2. **一条真实的互相调用从未被工具看见**: `core/c_core/src/time.c` 调 `br_plat_ticks_freq()` /
   `br_plat_ticks_now()`, 而 platform 的 `start.S` 调 `br_core_main()`。core 不在插件树里 ⇒
   ADR-0018 的"包含面 = 声明依赖闭包"执法**从来没有扫过 core 的 `#include`**,
   这条 core ↔ platform 的双向边一直靠注释和构建脚本里的 `platform_incs` 特判维持。
3. **"恰一个 core" 只能靠链接器兜**: "每镜像恰一个 platform、恰一个 app"是成文约束,
   而 core 的基数约束只体现为链接期的未定义符号(`br_core_main`)与重复定义。它是**真的**,
   但不可读、不可评审、也不是组合期的判据。

## 2. 决策

### D-1 新增 `plugin_type = "core"`, 与既有四类并列

`app | interface | ability | platform | core`。**`service` 不动** —— 它仍是 `ability` 的 `subkind`
(`scheduler|framework|io|fs|service`): 那一条子分类回答的是"能力件的**角色**", 与"是不是基座"不同轴。

### D-2 `core` **不定义生命周期**, 它驱动相位机而不参与其中

`core` 的描述符把 `early_init` / `init` / `start` **恒发** `BR_PLUGIN_NO_HOOK`, 且 `phase` 只能是
`early`(唯一的含义是"没有 `init` 钩子", 即没有 `init` 完成点)。理由:

* 它是**驱动者**: `br_core_main()`(四阶段启动链)与 `br_plugin_manager_*()`(扫段/拓扑/四相/自检 pass)
  都在它里面。若它还有 `init` 钩子, "core 排第几"就成了一个只能由 core 自己(或由特判)回答的问题 ——
  与 D-3 的"必须存在且唯一"叠加后, 相位机里会多出一个不可排序的节点。
* **自检不是生命周期**(ADR-0010: 自检是观测, 不是第五相)⇒ core 照常以描述符的 `.selftest` 钩子声明
  自己的套件。这**顺带删掉了一根旁路**: `plugin_mgr.c` 原先用弱符号 `br_core_selftest` 直接调 core 的
  套件 —— 那正是"core 不是插件"的残留。现在它是 `core/c_core` 的描述符钩子(符号名仍是
  `br_core_selftest`, 由 `symbol_prefix = "br_core_"` 得到), 与其它插件同一条路径。

### D-3 必须有且仅有一个: 三处执法, 一处成文判据 + 两处兜底

| 处 | 形态 | 说明 |
|---|---|---|
| 组合期 | `solver.rs` 的**产品级基数判据** `BRV-TAX-0020` | 闭包内 `plugin_type == "core"` 的插件数必须 = 1。与"恰一个 app / 恰一个 platform"同层; 按裁定 **R-1**, 只在存在 `product.toml` 时执法(裸插件树自成一体, 不套产品级计数) |
| 运行期 | `plugin_mgr.c` 在 `.br_plugins` 上重证 | 0 个**不可达**(没有 core 就没有 `br_core_main`, 链接不过), 但 **2 个是可达的**: 两个 core 各自有一份 `br_core_main` ⇒ 重复定义; 若将来两份实现符号不冲突, "谁拥有启动链"会**静默**由链接顺序决定 ⇒ 必须在段上重证 |
| 链接期 | `start.S` 的 `bl br_core_main` | 兜底, 不是判据(它给不出可读的诊断) |

### D-4 命名: `core/` 是**实现的命名空间**, 本刀 = `core/c_core`

`core/c_core`(C 实现), Rust 实现留给 `core/r_core`; **每镜像选其中一个**。理由: 需要被区分的不是
"这是什么角色"(都是 core), 而是"哪一份实现"—— 这正是 `<namespace>/<short>` 的用法。源码/头/自检
随之搬进插件根(`core/c_core/{src,include,selftest}/`), 满足 §8.3 ④"目录名 = 插件名"。

### D-5 构建归属: core 的源集合回到它自己的 `[build]`

`product.toml [build].core_sources` / `core_includes` **退役**(一处事实一个可写处):
core 的源/包含目录写在 `core/c_core/brick.toml [build]`。
**例外**: `selftest_sources` 留在 product —— 自检开关是**产品策略**(ADR-0010 的生成期开关),
这些源只在开关打开时被**追加到 core 插件的构建单元**; 关掉 ⇒ 整段不编译(不是"编译了再裁")。

### D-6 依赖方向: 谁都能依赖 core; core 只能"隔着契约"看 platform

* **→ core**: 任何插件依赖 core 都合法(`provider_pt == "core" && consumer != "core"`)。core 的头是
  **环境基座**: 与 A-2(APP 直连 core 头是违例, 其它插件不受限)同源, 声明 `[[dep]]` 不是使用前提。
* **core → core**: 非法(恰一个, 与 platform 同)。
* **core → platform/ability**: 合法, 但**不得是 `init` 边**。core ↔ platform 的互相调用是真实的
  (见 §1.2), 它由启动链的两步拆开(① `core.init` → ② platform `early_init`), **不是拓扑序能表达的
  东西**; 若两侧都写成 `init`, 求解器会报环 —— 那是对的(它是一个真实的环), 所以要避开的是"把它
  写成 init", 不是"假装它不存在"。
* **core 包含 `<br/platform/**>`**: 那是对**平台契约**(`br_plat.h`, 由所选 platform 插件提供)的
  依赖, 不是对"某个 platform 插件"的依赖, 因此这一族 `#include` 不做声明闭包执法
  (`build.rs`/`check.rs` 各一处显式豁免, 与 core 头对其它插件的豁免对称)。构建侧同理: core 的
  构建单元拿到 platform 的包含目录。

### D-7 权限上限 P4

core 建立恒等映射页表并开 MMU(`br_mm_activate`), 属 §3.4 的 machine 层。

### D-8 工具侧的形状

| 位置 | 改动 |
|---|---|
| `rules.rs` | `PLUGIN_TYPES += "core"`;`NAMESPACES += "core"`;`namespace_plugin_type("core") = "core"`;`phase_reason`: core 只接受 `early`;`max_privilege_level("core") = P4`;`dep_direction_ok` 按 D-6;`api_type_edge_reason` 把 core 计入"有面的消费者"(native core 不得依赖 `third_party`) |
| `schema/plugin.schema.json` | `plugin_type` enum 加 `core` |
| `plan.rs` + `templates/native/core/c/` | 新模板;`has_lifecycle = plugin_type != "core"` ⇒ 三个钩子与原型全部不发;`plugin_type_id = BR_PLUGIN_TYPE_CORE`。三个描述符模板把硬编码的 `{{symbol_prefix}}early_init`/`start` 换成 `{{early_hook}}`/`{{start_hook}}` |
| `build.rs` | 删掉合成 "core" 单元;core 作为普通插件单元编译;`core_includes` 的角色由**所选 core 插件的 `[build].includes`** 承担(仍作为每个单元的**环境基座**前置);platform 包含目录只加给 core 单元;`selftest_sources` 追加到 core 单元 |
| `model.rs` | `ProductBuild` 去掉 `core_sources`/`core_includes`;`[compat]` 对 core 保持可选(它不写自指区间) |

## 3. 被否决的替代

| 方案 | 为什么不取 |
|---|---|
| core 保留生命周期钩子, 强制排在最前 | "谁排第一"要由特判回答; 与"必须存在且唯一"叠加后相位机多一个不可排序节点(见 D-2) |
| core 只叫 `core`(不设命名空间) | 将来 Rust 实现只能改名或改类型; 命名空间存在的意义正是"同一角色的多个实现"(D-4) |
| core 的源仍留 `product.toml` | 插件自述原则对唯一一个插件破例, 与 D-5 相反 |
| 把 `[product].core` 从版本区间升级为"选择 + 区间" | 与 `[select].plugins` 的既有语义重叠(两处选择), 且 `[select]` 已经能表达"唯一被选中的那个" |
| 让 platform → core 成为 `init` 边(把启动链写进 DAG) | 拓扑序表达不了互相调用; core 也没有 `init` 完成点(D-2) |
| 保留 `br_core_selftest` 弱符号旁路 | 它就是"core 不是插件"的残留; 统一走描述符钩子后少一条特例(D-2) |

## 4. 后果与遗留项(诚实清单)

**新的事实**
* `core/c_core/brick.toml` 是 core 的**唯一构建真值**; `[budget]` 的 Σ 现在包含 core 的 `[[res]]`(16 KiB)。
* `[compat].core` 的语义变得更具体: 它是**其它插件对 `core/c_core` 的版本区间**。工具今天仍只
  **记录不求解**(与从前相同, 见 `iface`/`ver` 的现状), 自指问题由"core 不写 `[compat]`"回避。
* 自检的判据面变化: 段里描述符 +1(`[SELFTEST] SUMMARY plugins=` 23 → 24), core 的套件从
  "弱符号旁路"变成一条普通钩子 —— 日志行由 `[SELFTEST] core rc=N` 变成
  `[SELFTEST] core/c_core rc=N`。**`ran=`/`skipped=` 不变**(那条旁路本来也算一次 `ran`),
  所以门禁的 `ran=[1-9][0-9]* fails=0 errors=0` 判据一字未改。同一批
  `[PLGCONF]/[SVCCONF]/[TASKCONF]/[SYNCCONF]/[WQCONF]` tag 也一字未改。

**仍欠(不要误报为已还)**
1. core 的 **native 面仍未发布成接口单元**: `brick.toml` 里没有 `[[export]]`, `api/iface/` 下也没有
   `core/c_core`; `br_*` 家族今天仍是"环境基座"而不是可冻结的接口面。要发, 得先定"core 的面算几个单元"。
2. `[compat].core` 仍未参与求解(版本区间只是记录)。
3. `core/r_core` 只是命名空间上的位置, 未实现。
4. `BR_PLUGIN_TYPE_CORE` 加的是一类**基座**; `[lint].allow_edges` 仍是空表 —— 没有新增豁免。

## 5. 判据(本刀怎么算绿)

```bash
make tools-test                            # 644 例: schema/rules/plan/solver/build/新模板(V-1 core)+ 重发快照
build/host/x86-64/linux/bin/brickie-core --selftest   # 514 例: core 类型的正/负例(基数/相位/方向)
make all                                   # core 作为普通插件单元编译; core_sources 字段已不存在
make smoke plugin-test irq-test dbg-test sched-test sync-test fs-test posix-test
```
负例(必须红): 从 `product.toml [select].plugins` 删掉 `core/c_core` ⇒ `BRV-TAX-0020`
(exit 1, 信息里给出"found 0"); 再添一个 core 插件 ⇒ 同码"found 2"。
