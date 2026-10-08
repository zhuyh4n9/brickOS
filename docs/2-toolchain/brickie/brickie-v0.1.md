# brickie v0.1 详细设计(草案)

> 章节: **2-toolchain**(旁支子目录 `brickie/`, 暂不占用章节编号 `2-03`)。状态: **草案(讨论稿)**——能力边界与倾向已列, 结论待拍板。
> 定位: **prototype v1.0 核心交付产物之一**(与 manifest 格式、repo 骨架并列; 1-03 §5 第 4/5 项)。`brickie` 的 **v0.1** = 原型 v1.0 的第一件工具交付。
> 来源: `2-02`(BR-D1–BR-D8 倾向 / §5 命令面 / §7 演进路线); `2-01` §2 大纲; `4-02`(布局)/`4-03`(manifest)/`4-04`(依赖); `1-01` §6(插件模型)/§13(工作流); `1-02`(三态/golden/三层门禁); `3-05`(运行期管理面); `10-01`(接口插件); 需求方给出的 v0.1 功能清单与本轮硬约束(TOML / 两维分类(+`subkind` 派生) / 三语言 / manifest 字段集 / 导出面分类 ≡ `api_type`)。
> 分工: 本篇 = **`brickie` 工具 v0.1 的可实现规格**(命令面 / TOML schema / 版本模型 / 接口发布机制 / 依赖分析规则 / 验收标准); `brickie` 的**全局**架构与选型 → `docs/2-toolchain/2-02-brickie-arch.md`; 工具链总纲 → `docs/2-toolchain/2-01-toolchain.md`; manifest 与依赖的**语义权威** → `4-03` / `4-04`(本篇给出 v0.1 的具体形态并回填其开放问题)。
> 怎么用: 逐条拍板 **§4 的 BRV-D1–BRV-D11**(倾向已给; **D6 版本模型 / D10 导出面分类 / D11 冻结语义** 已由需求方规则给定), 消化 **§11 的 BRV-Q1–BRV-Q16**; 拍板后本篇转"成文", §13.2 的待对齐修订清单(现已 **A-1…A-27**)回灌各篇。
> **本轮修订(v2)**: 版本模型改为 **`COMPAT_GEN.MAJOR.MINOR.REVISE`**(F 与 M 解耦、依赖精确钉代), 并新增**解冻/重新冻结**机制与 **append vs modify** 条目级判定 —— 依据需求方版本号规则 + 备忘 [`r1/05`](comment/v0.1-review/r1/05-version-model-frozen.md)(F1/F2/F3 已定)与 [`r1/06`](comment/v0.1-review/r1/06-frozen-semantics-terminology.md)(术语与解冻)。此修订**关闭 BRV-Q1**, 并解掉评审 `r1/01` P0-1/P0-2、`r1/03` P0-4(三者同源)。改动集中在 §5 全节、§6.1–§6.5、§7.4–§7.7、§8、§10、§12、§13.2。
> **本轮修订(v3, S 级收敛)**: 按 [`checklist.md`](checklist.md) §0.1 的 **S-1…S-7**(跨 6 份评审收敛的硬问题) 逐条修复, **并回灌下游文档**:
> - **S-1**(相位两完成点): §7.2 重写为"完成点 + 断言"两条规则并补覆盖率边界; 回灌 `1-01` §6.1/§6.2、`4-04` §1–§3、`3-05` §1–§3。
> - **S-2**(`sched_class`/资源预算无输入): §8.1 新增 `[plugin].sched_class` 与 per-plugin `[[res]]`; §2/§7.6/V-7 同步。
> - **S-3**(`--frozen-gen` 残留): **删除**该旗标并新增 §7.7.1"旗标纪律"。
> - **S-4**(解冻机制未回灌): `1-02` §2.1/§2.2/§2.4/§2.6.2/§2.6.4/§2.6.5 + 状态机图(`pics/1-02-...png` 已重渲染)。
> - **S-5**(release 冻结排期): `3-01` §0/§1/§15 补第四/五/六批冻结计划; `O-H7` 关闭(crypto/keyring ops **入 golden**), `11-01`/`9-02`/`1-01`/`README` 同步。
> - **S-6**(分类域不闭合): §3.5 修订两条(三值 `api_iface` + `reexport_of` 列表); 回灌 `1-01` §7.3/§7.4、`1-02` §3.2、`4-03` §1。
> - **S-7**(A 编号撞号): §13.2 增"备忘编号"对照列 + 编号纪律声明。
> 交付标注: 主文档 + 下游文档的**回灌状态**见 §13.2 台账列。
> 图: 本篇正文用 ASCII 图与表格表达; **待补的两张 PlantUML 图**(接口发布时序 / 求解器数据流)仍挂在 §15 第 4 项 —— 图源环境已就绪(同仓库 `1-02` 的状态机图已按 README 流程重渲染), 缺的是本目录 `plantUML/` + `pics/` 的图源本身。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **API** | Application Programming Interface | 应用程序接口 |
| **APP** | Application | 应用(插件类型之一: 唯一业务逻辑, 恰一个, 不被任何插件依赖) |
| **BRV-D\*** | — | 本篇 §4 待拍板决策编号 |
| **BRV-Q\*** | — | 本篇 §11 开放问题编号 |
| **CI** | Continuous Integration | 持续集成(三层门禁的落地处) |
| **DAG** | Directed Acyclic Graph | 有向无环图; init-DAG = 初始化依赖拓扑, 环即组合期硬错误 |
| **DoD** | Definition of Done | 完成定义 |
| **IFACE-IR** | interface intermediate representation | 接口面规范化中间表示(接口发布与 hash 的输入) |
| **api_iface** | — | 接口单元的分类标签(`native` \| `runtime_adapter` \| `third_party`), 恒等于提供者的 `api_type`(§3.5) |
| **init 依赖 / runtime 依赖 / type 依赖** | — | 初始化顺序依赖(禁环)/ 运行期调用依赖(允许环, 注册表晚绑定)/ 仅编译期类型可见(不拉入运行期依赖; 1-01 §6.5) |
| **JSON** | JavaScript Object Notation | 结构化数据交换格式(`brickie --json` 的机器可读输出) |
| **M0–M5** | — | v1.0/v1.x 内部里程碑(1-03 §3; M5 = HSM 完整样例) |
| **native / runtime_adapter / third_party** | — | 插件按 **API 遵守规范** 的三分类(§3.1) |
| **app / interface / ability / platform** | — | 插件按 **架构层级** 的四分类(§3.2) |
| **semver** | semantic versioning | 语义化版本(maj.min.pat); 本篇为四段 `COMPAT_GEN.MAJOR.MINOR.REVISE`(§5.2) |
| **subkind** | — | `ability` 的细分类(scheduler/framework/io/fs/service), 用于承接旧八类信息(§3.3) |
| **TOML** | Tom's Obvious Minimal Language | 配置文件格式(**本篇定稿: manifest 唯一表达格式**) |

> **编号约定**: `BRV-D*` = 本篇决策; `BRV-Q*` = 本篇开放问题; `RV-*` = 风险(§12); `V-*` = 验收项(§10); `4-03 §3` = 他篇来源; 不带上限的 `§x` = 本篇。
> **注**: `BRV-*` 前缀(决策号 `BRV-D*`/`BRV-Q*` 与错误码 `BRV-<域>-NNNN`)沿用工具旧名 `br`, 属**已分配标识符**(跨文档引用 + 错误码将进 CI), 本轮工具改名 `br` → `brickie` **不重编号** —— 见 `docs/decisions/0001-brickie-tool-naming.md` 的「明确不改的项」。

## 0. 一句话定位与 v0.1 边界

`brickie v0.1` = **纯 host 侧、零编译依赖的"声明面闭环"工具**: 从生成一个插件骨架开始, 到把它的接口发布出去、把依赖图算清楚、把版本号机器化地推进一步为止。它**不碰编译器、不碰符号表、不碰 QEMU**。

```
插件作者视角(v0.1 闭环, 全程无编译器):

  brickie new ability service/crypto --api native --lang c
        │  ① 骨架生成: plugin.toml(真值) + 实现骨架 + 生成器落 build/gen/
        ▼
  brickie gen / brickie check [--profile dev|release]
        │  ② 依赖管理: init-DAG 无环 + compat_gen 精确匹配 + range(3段) + 相位一致 + 分类学禁则
        ▼
  brickie iface publish service/crypto
        │  ③ 接口发布: 接口面 → IFACE-IR 规范化 → hash → 变更集(append/modify)
        │     → 自动版本推进 → 影响报告
        ▼
  brickie.lock + api/iface/**(版本文件: 面 + status + freeze_state + 四段版本) + CHANGELOG + dependents 索引

  改/删已有 frozen 接口时, 中间多两步(§5.3):
      brickie iface unfreeze <unit> --note <RFC>   →  改代码  →  brickie iface refreeze <unit>
                                                        ⇒ COMPAT_GEN+1
  仅新增接口时: 无 unfreeze, 只有 MINOR+1
```

**v0.1 的四件能力**(需求方清单, 逐条落到 §5–§8):

| # | 能力 | 落点 |
|---|---|---|
| 1 | 插件骨架代码生成 | §8.4(`brickie new` / `brickie init` / `brickie gen`) |
| 2 | 插件依赖管理与分析 | §7(`brickie dep *` / `brickie check`) |
| 3 | 版本管理 `COMPAT_GEN.MAJOR.MINOR.REVISE` | §5(`brickie ver *` / `brickie iface publish` / `unfreeze`·`refreeze`) |
| 4 | 接口发布(变更标记 / 新增 / 自动版本+hash / 三态) | §6(`brickie iface *`) |

**v0.1 明确不做**(需求方定为 **v0.x 后续**):

| 后续能力 | 归属版本(草案, §14) | 为什么 v0.1 不做 |
|---|---|---|
| 编译 / 链接 / 生成镜像 | v0.3 | 需要构建后端与平台参数(2-02 BR-D4/BR-D6) |
| test(conformance 矩阵) | v0.4 | 需要 host 平台插件与用例表驱动(6-01 §2) |
| run(host-native / QEMU) | v0.5 | 需要镜像与 run 后端 |
| 兼容性检查(golden / abidiff / 版本矩阵) | v0.6 | 需要**构建产物符号表**为真值(1-02 §2.6.4) |
| 接口依赖扫描检查(符号级) | v0.2 | 需要头文件/符号面提取(§6.6 的 `hash_scope` 升级) |

> **边界纪律**: v0.1 的任何命令**不得**要求 `cc`/`cargo`/`nm` 在场; CI 只需 Python3 + 两个**预编译**原生子进程二进制(经 `prebuilts/seed` 或 `build/host` 提供, §9.1)。这条纪律同时是 v0.1 的测试前提(§10 V-9: 零编译依赖指**跑测试**不需要编译器)。

## 1. 设计必须满足的事实约束

| # | 约束 | 出处 |
|---|---|---|
| C1 | 闭包求解发生在编译**之前**, 但描述符是 C 结构体(编译期产物)⇒ 声明面必须独立于编译 | `2-02` §2 C1/C2 |
| C2 | **声明面唯一真值**: 声明文件(人写)是插件级真值, 描述符 C 代码与头文件是**生成物** | `2-02` BR-D2(倾向 C) |
| C3 | golden 真值在**构建产物**, 头文件是声明真值, 文档与生成物是衍生物 | `1-02` §2.6.1 |
| C4 | M0 验收 = "故意造环看组合器报完整环路径" ⇒ 求解器必须先于 CLI 完整体存在 | `2-02` §2 C2 |
| C5 | host/target 双后端同命令 ⇒ 平台是参数不是分支; v0.1 只做声明侧, 不涉及后端 | `2-02` BR-D6 |
| C6 | 生成物不手改; 输出目录内容 hash 幂等 | `2-02` BR-D5 |
| **C7** | **`COMPAT_GEN` 不可能靠人工诚实**: 它只能由"解冻 → 改/删已有接口 → 重新冻结"这一**事件序列**产生, 且必须是机器判定(人工只能声明 `MAJOR`/`MINOR`/`REVISE`), 否则版本号退化为声明 | 本篇(需求方要求"自动的版本更新" + 版本号规则) |
| **C8** | **表达格式已定 TOML** ⇒ 关闭 `4-03` §3 的 YAML/TOML/DSL 开放问题 | 需求方本轮硬约束 |
| **C9** | 核心逻辑 C++/Rust, Python3 只做粘合 ⇒ 需要一个**稳定的进程间 JSON 契约**(否则粘合层会退化成业务逻辑) | 需求方本轮硬约束 |
| **C10** | v0.1 无编译 ⇒ 接口面真值只能是**声明层**; 它必须在 v0.x 被符号层接管, 且**不得**成为第三种真值(§6.6) | `1-02` §2.6.1 + `4-02` §1 双真值 |
| **C11** | **导出的接口分类由 `api_type` 决定**: `native` 只抛 native 接口, `runtime_adapter` 只抛 runtime_adapter 接口, `third_party` **不抛接口** ⇒ 接口分类不是独立维度, 而是一条不变量 | 需求方本轮硬约束(§3.5) |

C1 + C4 + C10 三条合起来决定: **v0.1 的交付重心是"求解器 + 版本/接口引擎 + 幂等生成器", 不是"好用的 CLI"**——CLI 只是让这套引擎可被人和 CI 调用。

## 2. v0.1 能力边界细化(与组合期校验六项的映射)

`1-01` §6.4 的六项组合期校验在 v0.1 的覆盖度必须写死, 否则"v0.1 到底能做多少校验"会随讨论漂移:

| §6.4 | 校验项 | v0.1 | 说明 |
|---|---|---|---|
| 1 | 依赖闭包 + 版本区间交集 | ✅ 全量 | **`compat_gen` 精确匹配 + `range` 3 段交集**; 单版本政策(§7.4) |
| 2 | 环检测(报完整环路径) | ✅ 全量 | **仅 init 边**; runtime/type 边成环只报 info(§7.1) |
| 3 | 调度类别 + 双保险静态分析(D10) | ◐ 声明面 | `[plugin].sched_class`(§8.1)与调度器组合合法性可查; 源码静态分析 → v0.x |
| 4 | 接口校验(APP 声明在闭包内 / 符号族碰撞) | ◐ 模块级 | 接口**单元**级碰撞可查 + **导出面分类不变量全量执法**(§3.5); **符号级** → v0.2(1-02 D13 双层粒度) |
| 5 | 资源预算(ΣRAM/栈) + IRQ/DMA 独占冲突 | ✅ 全量 | 纯声明计算: per-plugin `[[res]]{ram_kib,stack_kib}` + `[privileged.resources]`(§8.1)⇒ 合计与 `product.toml [budget]` 上限比对(§7.6) |
| 6 | `abi_id` 一致性 | ✗ | 需要工具链指纹 ⇒ v0.6 |

**由此得到 v0.1 的产品定义**: `brickie check` 是一个"**声明面完备性检查器**", 它能保证"这个组合在**逻辑上**自洽", 不保证"这个组合**编得过/跑得对**"。这句话要写进 CLI 的帮助文本与 README, 避免 v0.x 用户误信。

## 3. 术语与两维分类学(第三列 `subkind` 为派生)

### 3.1 维度一: `api_type`(API 遵守规范; **提供方视角**)

> **口径收窄(关闭 `r1/01` P0-4① + P1-2)**: `api_type` 描述的是"**这个插件抛出的面属于哪套 API 规范**"——即**提供方视角**。因此:
> - **依赖许可列只适用于"有面的提供方"**(`ability` / `platform` / `third_party`): 它们抛 `[[export]]`, 才谈得上"可依赖谁"。
> - **纯消费者不参与依赖禁则**: `plugin_type = app`(**唯一业务逻辑**, 已有专门禁则"仅经 `interface`)与 `plugin_type = interface`(**严格叶子皮肤**, 只再导出/别名)的 `api_type` 只表示"**代码面向哪套 API 编码**"(v0.1 一律 `native`), 其依赖许可由 `plugin_type` 的规则(§3.2/§7.3)判定 —— **本表的"依赖许可"列对它们不适用**。
> - **不引入第 4 个值 `consumer`**: 消费者性由 `plugin_type` 的 `app`/`interface` 表达, 再加一个 `api_type` 值会与 `plugin_type` 重复且无法回答"它面向哪套 API 编码"。`grep consumer = 0` 是**有意为之**, 不是遗漏。

| `api_type` | 定义 | **可抛出的接口分类**(§3.5) | 依赖许可(强约束; **仅对有面的提供方**) | v0.1 |
|---|---|---|---|---|
| `native` | 严格遵守 brickOS **native API** 规范, 不依赖任何 POSIX/三方基座 | **仅** `native` 接口 | **不允许依赖 `runtime_adapter` 的能力面**(如 `svc-posix`)——`kind = "type"` 的编译期类型边除外(§7.1) | 骨架生成 + 校验 |
| `runtime_adapter` | 为 `third_party` 提供接口支持的适配基座(如 POSIX 运行时) | **仅** `runtime_adapter` 接口 | **只允许**依赖 `native`(core / 框架件 / native ability); `kind = "type"` 的编译期类型边可指向 `third_party` 与 `native`(§7.1 的显式例外) | 校验可识别; 骨架模板预留 |
| `third_party` | 携带上游源码的三方件(如 `service/sqlite`), 移植增量 = 适配层 | 可抛 `third_party` 接口(契约边界, §3.5); **能力优先经注册表发布**(BRV-Q13), 无 `[[export]]` 也完全合法 | 可依赖 `native`(能力)、`runtime_adapter`(基座)与 `third_party`; **不得被 `native` 提供方依赖其能力面**(可经注册表消费运行期能力, 见下) | 校验可识别; 骨架模板预留 |

> **依赖禁则的对称表述(关闭 `r1/01` P0-4②, 与 BRV-Q13 一致)**: 禁则约束的是**能力面**, **不是**"能不能用"。精确表述为:
> - **`native` 不得依赖 `runtime_adapter` / `third_party` 的插件能力面**(否则"极小组合可裁剪"失效);
> - **反向: `runtime_adapter` / `third_party` 不得被 `native` 依赖** —— 同一条规则的对称写法, 不是第二条规则;
> - **不受禁则约束的是"经注册表消费运行期能力"**: 任何插件(含 `native`)都可以在运行期按**名字**从 core 服务注册表取用能力(如 sqlite 的 `db`), 这不产生依赖边、不拉入闭包、不影响裁剪 ⇒ 与"不得依赖其能力面"**不矛盾**(BRV-Q13 的口径)。

> **三方件的"能力优先经注册表"不是措辞含糊**: 注册表发布(如 sqlite 的 `db`)是**运行期**机制, 不产生接口单元 ⇒ 三方件**可以完全没有 `[[export]]`**, 这正是 §3.5 不变量 2 允许的常态(见 §10 V-12 的 `third_party` 无 export 正例)。声明 `[[export]]` 是**可选**的、把上游能力面纳入我们治理的动作。

> `native ↛ runtime_adapter` 这条禁则的**架构动机**: 保证"极小组合"(不链任何适配基座)永远可裁剪——`1-01` §7.5 的复用经济学。它把 `4-04` §3「调用依赖是否需要声明面」的答案锁在"**必须声明**"上: 一旦允许 native 悄悄调 POSIX, 裁剪承诺就失效。**该禁则在 v0.2 起还会投影到接口粒度**: native 插件不得 `require` 分类为 `runtime_adapter` 的接口单元(§3.5/§5.3)。
>
> **`type` 依赖的显式例外(与 `7-01`/`8-01` 白名单同源)**: 禁则约束的是**运行期能力面**。三类依赖里只有 `init`/`runtime` 会引入运行期耦合; `kind = "type"`(§7.1)只是"编译期需要头文件/类型可见", 不把提供方拉进组合的运行期依赖 ⇒ **`runtime_adapter` 允许对 `third_party` 声明 `type` 边**(判例: `svc-posix` 移植层需要 sqlite 的类型), 该边不进 `release` 的"未冻结接口"判定(§7.5 只作用于接口消费)。**禁止的是** `runtime_adapter` 对 `third_party` 声明 `init`/`runtime` 边(那会把上游件拖进极小组合); 三方件的能力面仍只经注册表消费(BRV-Q13), 不作为接口单元被依赖。

### 3.2 维度二: `plugin_type`(架构层级)

| `plugin_type` | 定义 | 数量约束 | 接口访问许可 | 允许的最高特权级(§3.4) |
|---|---|---|---|---|
| `app` | 唯一业务逻辑 | **恰 1** | **仅**允许经由 `interface` 插件提供的能力(不得直调 native, 不得声明任何特权接口) | **P0** |
| `interface` | API 皮肤(严格叶子) | 0..n, 可叠加 | 仅允许再导出其提供者的面; 不拥有状态 | **P0** |
| `ability` | 核心能力扩展(scheduler 属此类) | 0..n; `subkind = scheduler` **恰 1** | native API + 按 `subkind` 授予的特权面 | **P2**(scheduler 可 P3) |
| `platform` | 平台管理(SoC 级) | **每镜像恰 1**(= 该镜像对应当前 SoC 的那一件) | native API + 全特权面 | **P4** |
| `third_party` | 携带上游源码的三方件(`api_type = "third_party"` 的插件, 如 `service/sqlite`); 它是**纯消费者**: 能力经注册表在运行期消费, 不进入接口契约治理 | 0..n(与 `api_type` 正交, 不单独计数) | native/`runtime_adapter` 能力 + 注册表(BRV-Q13); **不得被 `native` 插件依赖** | **P0** |

> **`plugin_type` 与 `api_type` 正交**: 上表 `third_party` 一行**不是第五个 `plugin_type`**(规范四值只有 `app`/`interface`/`ability`/`platform`, 见 `1-01` §6.3 表 A), 而是把"`api_type = third_party` 的插件"作为一个**可执法的组合位置**写出来——因为 §7.3 的依赖方向表必须能回答"谁来消费三方件"。`service/sqlite` 的 `plugin_type` 仍是 `ability`(§3.3), `api_type = "third_party"`; 二者同时成立, 数量与特权取 `plugin_type` 一行, 依赖许可取两行的**交集**。

> **这一列直接改写了既有结论(并已回灌, A-2)**: `1-01` §6.3 / `4-01` §2 **原**允许 APP"直调 native", 与 `9-01` 的"必须经 Interface"长期矛盾(comment/README P0 清单第 12 项 / review2/05 P0-3); **本轮该矛盾已收口** —— 两者现统一为"APP 仅经 Interface"(旧措辞只作为历史引述保留在收敛注里)。本草案采纳**需求方的强约束**(APP 仅经 interface)作为唯一口径, 矛盾因此有解——`iface-min` 正是"零开销直通 native"的合规出口(§13 待对齐清单 A-2)。

### 3.3 与旧八类的关系: `subkind` 保留信息(不是替换)

`1-01` §6.3 的八类(Platform/Scheduler/框架件/IO/FS/Service/Interface/APP)与新的四类 `plugin_type` 是**粗化**关系, 不是替换:

| 旧八类 | `plugin_type` | `subkind` |
|---|---|---|
| Platform | `platform` | —(`platform` 自身即细类) |
| Scheduler | `ability` | `scheduler` |
| 框架件(dev-core/cdev-core/vfs-core/bdev-core) | `ability` | `framework` |
| I/O | `ability` | `io` |
| FS | `ability` | `fs` |
| Service | `ability` | `service` |
| Interface | `interface` | — |
| APP | `app` | — |

**纪律(r1/03 P1-9 口径统一)**: `subkind` 只用于"授权与检索"(特权面授予、`allow_edges` 白名单、CLI 分组显示), **不参与**数量约束与依赖方向的基本判定, 也**不新增规则或维度**; 它只在基本判定给出的许可内做**收窄**。基本判定分两层, 互不重复:

| 判定 | 由谁决定 | 说明 |
|---|---|---|
| **数量约束 / 依赖方向 / 相位** | `plugin_type`(§3.2 + §7.2/§7.3) | `app` 恰 1 且仅经 interface; `interface` 严格叶子; `platform` 每镜像恰 1; `ability` 0..n 且框架件间单向。**方向取本行与下一行的合取** = §7.3 的"依赖许可" |
| **可抛出的接口分类 / 提供方依赖许可** | `api_type`(§3.1 + §3.5) | 这条**只对有面的提供方**(`ability`/`platform`/`third_party`)生效; `app`/`interface` 是纯消费者, 不参与该禁则(§3.1 口径收窄) |

**`subkind` 与上两层的关系**: `subkind`(scheduler/framework/io/fs/service)**不**新增任何方向规则; 它只在 `plugin_type` 给出的许可范围内做**收窄**(如 `subkind = scheduler` 恰 1、框架件间的显式白名单边 `allow_edges`)。**唯一的数量收窄是 `subkind = scheduler` 恰 1** —— 它是 `ability` 0..n 的收窄, 不改变"数量约束由 `plugin_type` 决定"的分层(与 `1-01` §6.3 表 A/B 同口径: 该表 `ability` 行亦写"0..n; `subkind = scheduler` 恰 1")。当"框架件间的单向依赖"需要豁免通用方向规则时, 走 `product.toml [lint].allow_edges` 显式白名单(§7.3), 而不是放宽 `ability → ability` 的通用判定。这样 `1-01` §6.3 的"依赖方向"一列可以在 v0.x 收缩为对 `plugin_type` + `api_type` 的引用(§13 A-1)。

### 3.4 特权接口(privileged interface)—— P0–P4 级别草案

**"特权接口"是本草案新引入的概念**, 既有文档没有: 它是"**只有特定 `plugin_type` 才被许可声明的 native API 子集**"。级别划分需求方标注为"待讨论", 以下是可执法的草案:

| 级别 | 名称 | 内容 | 允许的 `plugin_type` |
|---|---|---|---|
| P0 | `none` | 仅普通 native API(任务/时间/锁/服务注册表/内存分配) | app, interface, ability, platform |
| P1 | `resource` | 资源独占声明: IRQ 线 / DMA 通道 / 引脚 / 设备名 | ability, platform |
| P2 | `memory` | 内存管理面: 池创建 / region 属性 / DMA 缓冲 / cache 维护 | ability(默认上限), platform |
| P3 | `core` | core 内核面: 中断控制 / 调度钩子 / 时钟源 / fault handler 注册 | `ability.subkind = scheduler`, platform |
| P4 | `machine` | 机器面: MMU/页表 / EL 态 / PIC / 复位 / 早期 console | platform |

**memory 特权的表达粒度**(需求方点名):

```
声明 = 级别(P2) × 操作(ops) × 粒度(granularity) × 内存类型(regions)

  ops         : alloc | free | map | protect | flush | invalidate | unmap
  granularity : byte | page | region | pool        # 越靠右的粒度授权越大
  regions     : heap | dma | contig | page | mmio | reserved
                # heap/contig/page = 3-04 的三池(TLSF 堆/连续池/页池);
                # dma/mmio/reserved = DMA 缓冲/寄存器窗口/保留区
```

> **`ops`/`regions` 的两条硬要求(A-7 一致性)**: ① `ops` **必须含 `unmap`** —— 与 `br_mm_map`/`br_mm_unmap`(`3-01` §7)的 API 面对齐, 否则"粒度为 region 的回收"没有声明载体; ② `regions` **必须覆盖 3-04 的三池**(`heap`/`contig`/`page`)—— 只写 `heap|dma|mmio|reserved` 会漏掉 contig/page 两池, 使"池创建/池级授权"无法声明。**归属**: `map`/`protect` 属 **P4(machine)**(MMU/页表面, 只有 `platform` 可声明); 池生命周期(`alloc`/`free`, granularity = `pool`)属 **P2(memory)**。**规范落点 = `docs/3-os-core/3-01-core-api-list.md` §13.6**(完整分级 + 正交性 + 诊断码 `BRV-PRIV-0001/0002`); 本节只给 v0.1 工具侧的声明形态。

**v0.1 的执法范围**: 只做"**声明合法性**"(如 `app` 声明 P1+ ⇒ 红)与登记; **不**做"实际是否越权调用"的检查——那属于"接口依赖扫描检查"(v0.2)与源码静态分析(1-01 §6.4-3 后半)。

```toml
[privileged]
level = "P2"
[[privileged.memory]]
granularity = "pool"               # 池生命周期 ⇒ P2(见下正交表)
ops         = ["alloc", "free", "flush", "invalidate"]
regions     = ["heap", "contig", "page"]   # 必须覆盖 3-04 的三池
[[privileged.resources]]
irq          = [32]
dma_channels = [3]
pins         = []
device_names = ["hsm0"]
```

> **`[privileged]` 示例为何是"池级"而不是"页级 map/protect"(关闭 `r1/03` P0-6⑤)**: 本示例是 `ability`(P2 上限)的**合法形态**。按本节正交表, `map`/`protect` 的 granularity 是 `page`/`region` 且**属 P4(machine)**(MMU/页表面 ⇒ 只有 `platform` 可声明); 若示例写 `granularity = "page"` + `ops = ["map", "protect", …]`, 就会与 `3-01` §10/§13.6 的"`br_mm_region_add` 归 Platform"**冲突**。故示例改为 **P2 + 池生命周期**(`ability` 的正当用法: 池创建/回收/缓存维护)。需要 `map`/`protect` 的插件必须声明 P4 ⇒ 按 §3.2 只有 `platform` 能通过校验(违例 `BRV-PRIV-0001`)。

**`(ops × granularity × region)` 正交表(关闭 `r1/03` P0-6③④; 非法组合 ⇒ `BRV-PRIV-0002`)**

| `ops` | 合法 `granularity` | 合法 `regions` | 最低级别 | 说明 |
|---|---|---|---|---|
| `alloc` / `free` | `byte` / `page` / `region` / `pool` | `heap`(byte/page/region)/ `contig` / `page` / `dma` | **P2** | 动态分配与归还; `granularity = pool` = **池生命周期**(创建/销毁, 只对 `contig`/`page` 两个**专用池**; TLSF 堆的 `br_malloc`/`br_free` 属 P0 普通 native API, **不需要**特权声明) |
| `map` / `protect` | `page` / `region` | `mmio` / `reserved` / `heap`(仅恒等区属性) | **P4(machine)** | MMU/页表面 ⇒ **只有 `platform`**; `ability` 声明即越级(`BRV-PRIV-0001`)。**与 `3-01` §10 的分工**: 静态 region 表由 `platform.early_init` 经 `br_mm_region_add` 声明(Platform 拥有), 运行期映射/改属性走本行 |
| `flush` / `invalidate` | `byte` / `page` / `region` | `dma` / `heap` / `contig` | **P2** | cache 维护(驱动 DMA 前后, R4) |
| `unmap` | `page` / `region` | 同 `map` | **P4(machine)** | 与 `br_mm_unmap` 对齐; 是"`granularity = region` 的回收"的声明载体 |

> **与 `3-01` §10(接口归属)不冲突**: `br_mm_region_add`(静态 region 表)归 **Platform**(3-01 §10), `br_malloc`/`br_free`(TLSF 堆)是 **P0 普通 native API**; 本表的 P2/P4 只约束"**声明**对专用池/MMU 面的使用权"。因此 §8.1 的 `ability` + P2 示例用**池生命周期**(`granularity = pool`, regions = contig/page)是合法形态, 而 `map`/`protect` 必须 P4 ⇒ 只有 `platform` 能过校验。

> **表的读法**: 一行 = "这个 op 允许的粒度/区域/最低级别"; **声明越出 `level` 档位 ⇒ `BRV-PRIV-0001`; op × granularity × region 不在同一行允许集内 ⇒ `BRV-PRIV-0002`**。`granularity = pool` **有**对应池级 op(`alloc`/`free`, 即池生命周期) —— 这是 `r1/03` P0-6③ 指出的"有 `pool` 却无池级 ops"的收口; 若不需要池级授权, schema 也允许只声明 `page`/`region` 粒度。

### 3.5 导出面(`export`): 分类由 `api_type` 决定

需求方给定**硬规则**: 插件抛出的接口分类与其 `api_type` **同域且必须相等**。v0.1 的**修订**(评审 `r1/03` P0-3② / I5、`r1/04` C-1/C-2)是两条**分类域收口**, 否则旗舰判例 `iface-pkcs11` 无处安放:

| # | 首版的域缺口 | 现版 |
|---|---|---|
| a | `third_party` **一律不得**抛接口 ⇒ 三方件的上游 API 永远无法成为接口单元 | 允许 `third_party` **抛出分类为 `third_party` 的接口**(契约边界: 声明即纳入治理); 但**能力优先经注册表发布**(BRV-Q13), 无 `[[export]]` 仍是常态与合法态 |
| b | `reexport_of` 是**单值** ⇒ 一个皮肤不能同时再导出两个提供者 | `reexport_of` 是**列表**(`reexport_of = ["<provider>#<unit>", …]`)⇒ `iface-pkcs11` 可同时再导出 `service/crypto` 与 `service/keyring` 两个单元 |

| `[plugin].api_type` | 可抛出的接口分类 | 说明 |
|---|---|---|
| `native` | **仅** `native` 接口 | 严格遵守 native API 规范的面 |
| `runtime_adapter` | **仅** `runtime_adapter` 接口 | 为三方件提供支持的适配面(POSIX 等) |
| `third_party` | **仅** `third_party` 接口(**可抛**, 也可不抛) | 上游 API 面若纳入治理即取此分类; 不抛时能力经注册表发布 |

**因此"接口分类"不是第三个独立维度, 而是一条不变量**(C11)。一个 `[[export]]` 表由两个**正交**字段描述:

| 字段 | 取值 | 语义 |
|---|---|---|
| `api_iface` | `native` \| `runtime_adapter` \| `third_party` | **接口分类** —— 必须等于 `[plugin].api_type`(三分域, 全域闭合) |
| `form` | `api` \| `skin` \| `service` | **导出形态**(机制), 与分类正交 |

| `form` | 语义 | 附加字段 | 对应既有机制 |
|---|---|---|---|
| `api` | 本插件**自有**的 API 面(头文件 + 符号) | — | `1-02` §2.6 golden 面 |
| `skin` | **再导出**提供者的面(不转移所有权) | `reexport_of = ["<provider>#<unit>", …]`(**列表; 每个元素一个被再导出单元**); 可选 `symbols = [...]`(即 `1-01` §7.3 的 `api_syms`) | `1-02` D13 `reexports` |
| `service` | 通过服务注册表发布的**能力名 + ops 契约** | `name`(注册表名) | `3-06` 注册表语义 |

**三条不变量(v0.1 全量执法)**:

| # | 不变量 | 违例 ⇒ |
|---|---|---|
| 1 | `export.api_iface == [plugin].api_type` | `BRV-TAX-0016` |
| 2 | `[plugin].api_type == "third_party"` ⇒ 闭包内该插件的每个 `[[export]]` 必须 `api_iface = "third_party"`(即三方件不得抛 `native`/`runtime_adapter` 面; **不抛 `[[export]]` 合法**) | `BRV-TAX-0017` |
| 3 | `form == "skin"` ⇒ `reexport_of` 非空(列表), 且**列表中每一个**被再导出单元的 `api_iface` 与自身相等 | `BRV-TAX-0018` |
| 4 | `form != "skin"` ⇒ 不得声明 `reexport_of`/`symbols`(反向不变量) | `BRV-TAX-0019` |

**分类的三个消费方**(这条规则为什么有实际后果, 而不是纯声明):

1. **接口单元 id 携带分类**: 单元 `<provider>#<unit>` 的解析结果里带 `api_iface` 字段, 随快照与 `--json` 一起输出(§6.1)。
2. **依赖侧按分类判合法**: `requires_iface`(§5.3)引用一个单元时, 若无脑跨类消费则红 —— **`native` 插件不得 require 分类为 `runtime_adapter` 的接口单元**; 这正是 §3.1 的 `native ↛ runtime_adapter` 禁则在**接口粒度**上的投影(v0.2 起生效, 因为 v0.1 不扫描接口依赖)。
3. **`skin` 边必须豁免 api_type 禁则**: `iface-posix`(runtime_adapter 皮肤)要依赖 `svc-posix`(runtime_adapter 基座), 若照搬"runtime_adapter 只许依赖 native"会被误杀。因此规则精确表述为: **`form = "api"` / `service` 的依赖边受 `api_type` 禁则约束; `form = "skin"` 的再导出边豁免**, 且豁免必须由 `reexport_of` 显式声明方能成立(不可隐式)。

> **旗舰判例(域闭合的验收用例)**: `iface-pkcs11` 是 `plugin_type = interface` / `api_type = runtime_adapter` 的薄皮肤, 它再导出 `service/crypto#crypto` 与 `service/keyring#keyring` 两个单元 ⇒
> - `reexport_of = ["service/crypto#crypto", "service/keyring#keyring"]`(**列表**容纳两个提供者, 缺口 b 关闭);
> - 两个被再导出单元的分类必须与 `iface-pkcs11` 自身相等 —— 这要求 **`service/crypto` / `service/keyring` 的 `api_type = runtime_adapter`**(它们的接口面按 POSIX 形状的适配契约治理), 而不是 `native`;
> - `app/hsm` 只依赖 `iface-pkcs11`(严格叶子, 9-02 §3), 于是"两个后端 + 一个皮肤"的组合在分类学上**有唯一解**。
>
> **`third_party` 的后果要写清楚**: 三方件若**不**声明 `[[export]]`, 其能力面就**不进入**接口发布与版本治理 —— 没有接口单元, 也就没有四段版本、没有 hash、没有三态、没有 dependents 报告。这是有意的记账取舍(上游 API 不受我们治理), 代价是"三方件被谁用了"在 v0.1 只能从 `[[dep]]` 看, 无法从接口面反查 —— 记为 RV-9。若三方件需要注册表发布能力(如 sqlite 的 `db`), 归 BRV-Q13。
>
> **"再导出"与"自有别名面"是两件事**(A-11 的边界, 判例见 `1-01` §7.4): `form = "skin"` + `reexport_of` 列表只用于**再导出别人的接口单元**(分类必须与被再导出单元相等); 若一个 Interface 插件暴露的是**自己拥有的面**(如 `iface-min` 的 native 别名层), 则取 `form = "api"` 且 `api_iface` 随该面的规范(`iface-min` ⇒ `api_type = native`)。**判据一句话**: "再导出别人的单元 ⇒ 随被再导出单元; 暴露自有的别名面 ⇒ 随该面的规范。"

> **v0.1 只发布与哈希, 不生成代码**(生成物的消费方在 v0.3 之后)。

## 4. 关键决策(待拍板)

### BRV-D1 — v0.1 与 `2-02` 的关系: 纵切, 不另立体系

| 备选 | 说明 | 权衡 |
|---|---|---|
| A 另立一套 v0.1 架构 | 按需求方清单从头设计 | 与 `2-02` 的 L0–L5 分层、BR-D1–D8 倾向产生两套体系 |
| **B `2-02` 分层的一个纵切** | 只落地 L0/L1/L2/L5 中 v0.1 用得到的那部分, 全部继承 `2-02` 的倾向 | 需要先认账 `2-02` 的倾向(等于是把它默认拍板) |

**倾向 B**。v0.1 落地时**同时**把 `2-02` 的 BR-D1/BR-D2/BR-D5/BR-D7 从"倾向"升为"已定"(它们正是 v0.1 直接依赖的四条)。

### BRV-D2 — 表达格式: TOML(关闭 `4-03` §3 开放问题)

| 备选 | 优点 | 代价 |
|---|---|---|
| **TOML** | 与 Rust 生态一致(Cargo 判例); 无缩进敏感; 标准库级解析器在 Python3.11+ (`tomllib`)与 Rust (`toml`) 均成熟; 数据类型显式, 无需 schema 才能读 | 深层嵌套数组啰嗦; 无锚点/引用(需自造 `include`, v0.1 不做) |
| YAML | 表达力强、锚点复用 | 缩进敏感、隐式类型陷阱(`no`→bool)、解析器生态在 Rust 侧偏弱 |
| 自定义 DSL | 可为领域优化 | 自造解析器 + 编辑器支持 + 错误信息, 全部要自己养 |

**倾向: TOML**, 并立三条纪律: (1) **禁止**用 TOML 的隐式特性表达语义(一律显式键); (2) `--json` 输出与 TOML 输入**同源**——同一内部模型的两个序列化, 关闭 `2-02` Q7; (3) schema 以 **JSON Schema** 单一形式描述(TOML 与 JSON 都能校验), 放 `brickie/schema/`。**显式裁定(r1/03 P1-12)**: JSON Schema **只表达单条记录的形状**(字段名/类型/枚举/必填/数组元素形状); **跨字段不变量**(如 `api_iface == api_type`、`third_party` 不得抛异类面)与**跨文件不变量**(引用完整性)**只由引擎报**(`BRV-TAX-0016/0017/0018/0019`、`BRV-MF-*`/`BRV-DEP-*`/`BRV-VER-*`)—— schema **不表达**这些不变量, 也**不得**为了表达它们引入第二份真值(否则 Python 会以 exit 2 先拦, 使引擎码永不打印、V-12 无法通过)。

### BRV-D3 — 三语言分工与进程边界

| 备选 | 说明 | 权衡 |
|---|---|---|
| A 单语言(Python) | 最快 | 违背需求方硬约束(C9); 求解器/版本引擎长期要嵌进 CI 与 IDE |
| B 三语言 FFI 单进程 | `pyo3` + `pybind11` 单进程 | 构建链最复杂, 三套工具链绑在一个进程里 |
| **C 三语言 + 子进程 JSON 契约** | Python3 = CLI 前端与粘合; Rust = 领域模型/求解/版本/接口面; C++ = 代码生成器; 三者以 **JSON over stdio** 通信 | 多一层序列化开销(静态工具, 无感); 换来每一层都可被 CI 独立调用与单测 |

**倾向 C**。进程边界:

```
   brickie (Python3 包)                    ← L5 前端: 参数/输出/退出码/文件编排/schema 校验
     ├── brickie-core (Rust 可执行)         ← L0/L1: 模型 + 求解 + 版本引擎 + IFACE-IR/hash
     └── brickie-gen  (C++ 可执行)          ← L2: 骨架/描述符/头文件代码生成(模板渲染)
                    └── 二者通过 JSON over stdio 交换"模型/诊断/变更集", 不交换文件句柄
```

**C++ 为什么必须在 v0.1 就有位置**: 它的产物是 **C 源码**(描述符 `BR_PLUGIN(...)` 展开、骨架 `init/start` 桩、对外头文件), 必须与 `1-01` §6.1 的 `br_plugin_t` 宏形态、`3-01` §13 的可见性宏**同源**; 且 v0.2 的"接口依赖扫描检查"注定要接 `libclang` 类的 C/C++ 头文件解析器——v0.1 先用生成器把这条边界建起来, 避免 v0.2 时把生成器从 Python 里考古出来。

### BRV-D4 — 声明文件的两级结构

| 备选 | 说明 |
|---|---|
| A 只有产品 manifest | 插件依赖写在产品 manifest 里 ⇒ 每个产品都要复述一遍插件自述 |
| **B 两级: 插件自述 + 产品选择** | `plugin.toml`(插件级真值: 依赖/资源/导出/特权) + `product.toml`(产品级: 选谁/预算/产品参数) |

**倾向 B**(继承 `2-02` BR-D2 与 `4-03` §3「两级」倾向)。**唯一真值原则**: 插件的 `dep`/`export`/`privileged` **只许**出现在 `plugin.toml`; `product.toml` 引用插件名并只声明"选择与预算", 不得重复插件自述字段——违反 ⇒ `brickie check` 红(`BRV-MF-0007`)。

### BRV-D5 — 状态目录与"进不进版本库"

| 产物 | 位置 | 进版本库? | 理由 |
|---|---|---|---|
| **工具自身的宿主可执行**(`brickie-core` / `brickie-gen` / **自包含入口 ELF `brickie`**) | `build/host/<host-arch>/<host-os>/bin/**` | **否**(`.gitignore` 的 `build/`) | 工具是**宿主**程序 ⇒ 出树, 参考 Android `out/host/...`; 源码树不留 `.o`/可执行(§9.2 的落点细化)。**入口 ELF** = L5 Python 前端 + 模板 + **原生工具**全部嵌入二进制(单文件自包含; ADR `0004` §7) |
| 工具自身的宿主中间产物(对象/静态库) | `build/host/<host-arch>/<host-os>/{obj,lib}/**` | 否 | 同上; `host-arch` ∈ {`x86-64`,`aarch64`,…}, `host-os` ∈ {`linux`,`darwin`,`win`,…} |
| **自举种子**(工具自身的**预编译**可执行) | `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/**` | **是** | 让**没有编译器**的全新 checkout 也能起步, 并作为"brickie 自举编译 brickie"的初始砖(ADR **0004**); 体积可控, 随源码走。其中 `brickie` 是**单文件自包含**(内含 `brickie-gen` 等原生工具), **只拷它一个**即可运行 |
| 生成物(描述符/头文件/注册表 C 代码) | `build/gen/**` | **否**(`.gitignore` 已有 `build/`) | 生成物纪律(C6); 可随时 `brickie gen` 重建 |
| 反向依赖索引 / 求解缓存 | `build/index/**` | 否 | 可重建的派生物 |
| 锁定文件(闭包与版本区间解 + **兼容代快照**) | `brickie.lock`(仓库根) | **是** | 复现性: 闭包解不是派生物, 是决策。**含兼容代(`compat_gen`)**: 每条接口消费/导出记录同时锁 `compat_gen`, 使"当时求解依据的是哪一代"可复现(关闭 `r1/05` §8.4-12——`br.lock` 扩为"闭包解 + 兼容代") |
| 接口面快照(版本文件) | `api/iface/<provider>/<unit>.toml` | **是** | 接口发布的**记录**, 是 `COMPAT_GEN` 与 hash 推导的历史输入 |
| 变更日志 | `api/iface/CHANGELOG.md` | **是** | 衍生物但需人读与评审 |
| 决策记录 | `docs/decisions/NNNN-*.md` | 是 | `1-02` §2.2 |

**倾向: 采纳**。同时关闭 `2-02` Q5(状态目录 = `build/`, 不放 `.brickie/`; 需要进版本库的那两件单独放根与 `api/`)。

> **宿主三元组的分工**(与 Android 的 `out/host` / `out/target` 分家同构): `build/` 下**镜像类**产物(交叉工具链, 来自 `brickie build` 的编排)与**宿主类**产物(工具自身)分居两侧; 后者的一级路径由"宿主是谁"决定 ⇒ 同一份源码在 x86-64/linux 与 aarch64/darwin 上互不覆盖, 也不与镜像产物撞名。映射(如 `x86_64`→`x86-64`、`MINGW*`→`win`)只允许有**一处真值**(登记见 checklist §5.6 **G-6**)。
>
> **`build/host/**` 与 `prebuilts/**` 是两份宿主产物, 不可互相替代**: 前者是"本机/本次构建"的**派生品**(不进库), 后者是"随源码提交的自举**种子**"(进库); 前端查找顺序为 `$BRICKIE_GEN` → `build/host/<triple>/bin`(本机新编的优先) → `prebuilts/seed/brickie/<triple>/bin`(种子)。**种子里 `brickie` 单文件自包含**(Python 前端 + 模板 + `brickie-gen` 等原生工具都嵌在其内), 独立副本 `brickie-gen` 仅作开发态便利。发布/校验命令面 = `make tools-prebuilt` / `make tools-prebuilt-check`; 种子落后于源码即红(登记见 checklist §5.6 **G-7/G-8**, 决策记录 ADR [`0004`](../../decisions/0004-brickie-prebuilts-bootstrap.md))。
>
> ⚠ **目录名辨析**: `prebuilts/toolchain/`(**派生侧**)是外部工具链的**下载缓存**(`fetch-prebuilt.py` 重建, 派生、体积以 GB 计、**不进库**); `prebuilts/seed/`(**进库侧**)才是本节的**自举种子**(进库)。两侧现由**子目录**分开(不再靠两个只差一个 `s` 的顶层目录名), 见 ADR [`0005`](../decisions/0005-prebuilts-single-root.md)。

### BRV-D6 — 版本号模型 `COMPAT_GEN.MAJOR.MINOR.REVISE`(详见 §5)

**已采纳**(需求方规则给定, 非备选):

- 段 = `COMPAT_GEN . MAJOR . MINOR . REVISE`; **`COMPAT_GEN` 与 `MAJOR` 正交**(首版未解耦, 是三个 P0 的共同根因)。
- `COMPAT_GEN` **仅由"解冻 → 改/删已有接口 → 重新冻结"更新**; **新增接口不动它**。
- 依赖**必须精确指定 `COMPAT_GEN`**; `>=`/`>`/`=` 只比较 `MAJOR.MINOR.REVISE`。
- 单调: 段只增不减, 右段随左段增量清零; **跨 `COMPAT_GEN` 比较无意义**(§5.5)。

### BRV-D11 — 冻结语义与解冻机制(需求方规则 + 术语澄清)

**已采纳**:

- **`frozen` 语义不修改** —— `1-02` §2.1 本就写明"**新增=轻量**; 语义/签名变更=决策记录+弃用周期", 与本篇"新增免解冻"**逐字一致** ⇒ 无需重开既有决策。补一句 gloss: `frozen` = **单向冻结(append-only 保护)**。
- **段名用 `compat_gen` 而非 `frozen_version`** —— 后者会被读成"冻结时的版本"而诱导**跨代比较**(§5.5 明令无意义)。
- **`unfreezing` 是单元级瞬态**(`freeze_state`), 不是第 4 个条目 `status`; `unfreeze` 需最高门槛 RFC, `refreeze` 仅在"确有改/删已冻结条目"时 `COMPAT_GEN+1`(空解冻不 bump)。
- **append vs modify 必须按条目内部结构判定**(§5.4): "给已冻结结构体加字段"= **修改** ⇒ 须解冻(对齐 `1-01` D22"后补字段 = 布局破坏")。
- **"两个完成点"是相位的机械语义**(`1-01` §6.2 的 EARLY / CORE / LATE / APP 四相与回调的映射): **每个插件有两个独立的完成点** —— ① `early_init` 完成后已"注册可用"(全部插件都在 EARLY 跑 `early_init`); ② `init` 完成后"能力可用"(非 Service/Interface 插件在 CORE 完成, Service/Interface 在 LATE 完成)。**声明面的 `[plugin].phase` 只表达第 ② 个完成点所在的相**(取值 `early|core|late|app`; `early` 是"无 `init` 钩子, 第 ① 个完成点即终态"的特例); `1-01` §6.1 的 `br_dep_t.phase` 因此是依赖方对第 ② 个完成点的**断言**(§7.2)。这条映射必须写进 `1-01` §6.1/§6.2, 否则"单值 `phase`"会被读成"只有一个完成点"而误杀合法边(评审 S-1)。详见备忘 [`r1/06`](comment/v0.1-review/r1/06-frozen-semantics-terminology.md)。

### BRV-D7 — 接口面真值的 v0.1 过渡态与 v0.x 迁移

| 备选 | 说明 | 权衡 |
|---|---|---|
| A 声明文件永远是接口面真值 | v0.1–v0.x 一致, 无迁移 | 与 `1-02` §2.6.1(C3: 头文件声明真值 / 构建产物机器真值)冲突 |
| B v0.1 声明 → v0.x 头文件/golden | 与既有治理对齐 | 需要显式迁移窗口与 `hash_scope` 字段 |
| **C B + `truth` 字段显式声明** | 每个接口单元声明 `truth = "decl" \| "header"`, 让"谁是真值"成为**可读的、可校验的**元数据; v0.1 只允许 `decl`, v0.x 起允许 `header` 并强制 `brickie iface check` 双算一致 | 多一个字段 |

**倾向 C**(详见 §6.6)。理由: 迁移不可避免(C10), 与其让它隐性发生, 不如把它变成一个字段与一条 CI 门禁。

### BRV-D8 — 诊断模型: 编号化 + 位置化 + 可机读

**倾向: 采纳**(无备选争议)。每条诊断 = `{code, severity, target, file, span, message, hint}`; 编号 `BRV-<域>-NNNN`(域: `MF` manifest / `DEP` 依赖 / `VER` 版本 / `IFACE` 接口 / `TAX` 分类学 / `PRIV` 特权 / `GEN` 生成 / `PROTO` 子进程协议); `brickie check` 的**环报错**输出完整边路径(满足 C4 的 M0 验收), 且 `--json` 下是结构化边列表而非字符串。

> **严重度与退出码的分工(关闭 G-1/G-5)**: **`severity` 描述"信息有多重", 退出码描述"命令有没有完成"** —— 二者**独立**: `info`/`warning` **不**决定退出码; 只要命令未能完成(用法错/环境错/校验红), 退出码就按 BRV-D9(`2` 用法或环境错 / `1` 校验红)。因此 `BRV-TAX-0014`(模板未交付)可以**是 `info` 而命令仍以退出码 2 结束**(用法错独立成立)。**编号义务**: 凡是**机读消费方需要分类**的诊断都必须编号(含用法/环境错) —— 这正是补 `MF-0001`/`PROTO-0001/0002` 的原因; 仅"无法分类的兜底错误"(如未知子命令)才允许 `code: null`。

**已分配编码表**(v0.1 全集; 首版 **`VER` 域一个码都没有**, 导致 V-7 无码可断 —— 见评审 `r1/03` P1-13):

| 域 | 码 | 含义 | 严重度 |
|---|---|---|---|
| `MF` | `0001` | **必填字段缺失 / 字段形状不符 schema**(如 `[[compat.requires_iface]]` 缺 `id`/`compat_gen`/`range`, 或插件名不符名字契约) —— 关闭 G-2 的"无码用法错" | error |
| `MF` | `0007` | `product.toml` 重复声明插件自述字段(违反 BRV-D4 唯一真值) | error |
| `DEP` | `0009` | 相位单调违例(§7.2) | error |
| `DEP` | `0010` | `[[dep]].phase` 断言与提供方自述相位冲突 | error |
| `DEP` | `0011` | 同插件多版本共存(违反单版本政策) | error |
| `VER` | `0001` | **`compat_gen` 不匹配**(依赖钉 N × 提供方在 M) | error |
| `VER` | `0002` | 同代内 `range` 越界 | error |
| `VER` | `0003` | 依赖未声明 `compat_gen`(必填缺失) | error |
| `VER` | `0004` | **依赖未冻结接口**(`compat_gen = 0`): `dev` ⇒ info; **`release` ⇒ error**(§7.5) | info/error |
| `VER` | `0005` | 版本串段数错误(应为 4 段) | error |
| `VER` | `0006` | `range` 含 4 段(应只含 3 段) | error |
| `VER` | `0007` | 版本回退(`--set` 低于当前版本) | error |
| `IFACE` | `0009` | **单元处于 `unfreezing` 而 `--profile release`**(§5.3.4 规则 3) | error |
| `IFACE` | `0010` | **`SEMANTIC` 变更缺决策记录**(软路径的 `--note` 硬门, §6.3) | error |
| `IFACE` | `0011` | **`truth`/`hash_scope` 不成对**(v0.1 只允许两者都是 `decl`; 出现 `sym`/`header` 即红。§6.6/§10 V-11②) | error |
| `IFACE` | `0012` | **`plugin.toml [[export]].hash` 与 `api/iface/**` 快照 hash 不等**(同一真值的两处记录不一致。§9.1.1 规则 4 的例外/§10 V-11④) | error |
| `TAX` | `0013` | 新增插件名不符推荐 namespace 形态 | warning |
| `TAX` | `0014` | 所选 `api_type` 模板尚未交付(v0.x) | info |
| `TAX` | `0015` | `subkind` 推导与显式声明冲突 | error |
| `TAX` | `0016/0017/0018` | 导出面分类不变量 1/2/3(§3.5) | error |
| `TAX` | `0019` | `form != "skin"` 却声明 `reexport_of`/`symbols`(反向不变量, 评审 P2-2; 与不变量 3 对称, 见 §3.5 不变量 4) | error |
| `PRIV` | `0001` | 声明的特权面**越出** `[privileged].level` 允许的档位 | error |
| `PRIV` | `0002` | (ops × granularity × region) 组合非法(§3.4 正交性) | error |
| `GEN` | `0002` | 生成目标已存在且非生成物目录 | error |
| `PROTO` | `0001` | **子进程 JSON `protocol_version` 不匹配**(L5 与 L0/L1/L2 的握手失败; §9.1 "分发与协议版本") | error |
| `PROTO` | `0002` | **用例/快照的 `protocol_version` 与当前工具不一致**(快照测试的红线; 不自动迁移) | error |

### BRV-D9 — 退出码与输出契约

**倾向: 完全继承 `2-02` BR-D7**, 且明确 v0.1 只会用到前三档:

| 码 | 含义 | v0.1 |
|---|---|---|
| 0 | 成功 | ✅ |
| 1 | 校验红(环/冲突/预算/布局/版本/分类学/特权声明) | ✅ |
| 2 | 用法或环境错(TOML 语法/schema 不符/路径不存在) | ✅ |
| 3 | 编译或运行失败 | ✗(v0.3+) |

### BRV-D10 — 导出面分类 = `api_type`(需求方硬规则, 非备选)

**采纳**(无备选): 插件抛出的接口分类与 `api_type` 同域且必须相等; `third_party` 不抛接口(§3.5 三条不变量 + `BRV-TAX-0016/0017/0018`)。

- **代价**: `third_party` 的能力面脱离接口治理(记 RV-9), 且 `requires_iface` 必须带 `api_iface` 字段(§5.3)。
- **收益**: 一个"不变量"取代了一个独立分类维度 —— 分类不会漂移; 且 `native ↛ runtime_adapter` 禁则获得了接口粒度的执法点(原先只在插件粒度)。
- **需一并确认**: `skin` 边豁免(§3.5 消费方 3)、三方件注册表发布归属(BRV-Q13)。

## 5. 版本管理 `COMPAT_GEN.MAJOR.MINOR.REVISE`

> **本节口径来源**: 需求方版本号规则 + 备忘 [`r1/05`](comment/v0.1-review/r1/05-version-model-frozen.md)(F1/F2/F3 已定)与 [`r1/06`](comment/v0.1-review/r1/06-frozen-semantics-terminology.md)(术语与解冻机制)。
> **相对首版的变化**: **推翻**首版的 `va.b.c.d` 推进矩阵与 `^`/`~` 区间语义。首版把第一段定义为"冻结面兼容代数"但**未解耦主版本**, 导致该段与主版本恒同步、四段退化为三段(评审 `r1/01` P0-1/P0-2、`r1/03` P0-4)。

### 5.1 存储与展示

- **段**: `COMPAT_GEN . MAJOR . MINOR . REVISE`, 四段均为非负整数。
- **存储/比较**: 无 `v` 前缀, 写入 TOML 为字符串 `version = "0.1.0.0"`; **展示**加 `v` 前缀 `v0.1.0.0`。
- **hash 附着**: `iface_ref` = `vF.M.m.r+sha256:<hex>`; hash 是**构建元数据**, **不参与**比较与区间求解(语义同 semver 的 build metadata)。完整 64 位 hex 存于 lock/快照; 展示取前 12 位。
- **段名为什么是 `COMPAT_GEN` 而不是 `frozen_version`**: 该段是"**已承诺面被破坏的次数**"(代数), 不是"冻结发生在哪个版本"(序列上的指针)。`frozen_version` 会主动诱导**跨 `COMPAT_GEN` 比较** —— 而这恰恰是 §5.5 明令无意义的操作。详见 `r1/06` §3。
- **命名决策(`r1/06` T1, 已关闭)**: 在 `compat_gen`(语义最准)与 `frozen_gen`(保留"冻结"身份)之间, **需求方选 `compat_gen`**。代价是丢掉了"冻结"这一来源线索、与三态的联系变弱 —— 由 `status`(条目级治理态)+ `freeze_state`(单元级冻结态)**两条轴补足**(§5.3.1), 故该代价可接受。
- **大小写约定**: **段显示名用大写 `COMPAT_GEN`**(版本串与叙述), **TOML 字段/API 名用小写 `compat_gen`**。二者同义、不得混用(避免 schema 与文档叙述不一致)。

### 5.2 段语义与「事件 → 段」推进规则(核心)

| 段 | 名称 | 谁决定 | 回答的问题 |
|---|---|---|---|
| `COMPAT_GEN` | **兼容代**(兼容代数) | **工具强制**(解冻/重冻序列) | "已承诺的接口面**被改/删过几次**?" |
| `MAJOR` | 主版本 | **人声明** | "产品能力代际是否变了?" |
| `MINOR` | 次版本 | **人声明** | "是否新增了接口或功能?" |
| `REVISE` | 修订 | **人声明** | "面未动, 只是实现/文档/性能变了?" |

**关键: `COMPAT_GEN` 与 `MAJOR` 正交**(首版让二者恒同步, 是三个 P0 的共同根因)。`COMPAT_GEN` 由**接口契约**驱动, `MAJOR` 由**产品演进**驱动 ⇒ 可同时出现 `F=3,M=1`(接口被破坏过三代但产品大版本没动)与 `F=1,M=2`(产品翻代但接口没动)。

**推进规则**(`brickie iface publish` 判定; `MAJOR`/`MINOR`/`REVISE` 由 `brickie ver bump --rule` 或 `publish --set` 声明):

| 事件 | `COMPAT_GEN` | `MAJOR` | `MINOR` | `REVISE` | 依据 |
|---|---|---|---|---|---|
| **改/删已有 `frozen` 条目**(经解冻 → 重新冻结) | **+1** | 不动 | →0 | →0 | §5.3 硬路径 |
| 改/删已有 `deprecated` 条目(移除) | **+1** | 不动 | →0 | →0 | 仍须满足弃用周期(`1-02` §2.6.5; **v0.1 口径见 §6.4** —— 声明面硬门 + 符号面软门) |
| **新增条目**(append-only) | **不动** | 不动 | **+1** | →0 | 需求方规则"新增接口一般不动" |
| **追加枚举成员 / 预留槽位填充** | **不动** | 不动 | **+1** | →0 | append 语义, 见 §5.4 |
| 治理状态转移(`experimental→frozen` / `→deprecated` / 撤销弃用) | **不动** | 不动 | **+1** | →0 | 面未动(`1-02` §2.6.2) |
| 改已有**全部 `experimental`** 条目 | **不动** | 不动 | **+1** | →0 | 无承诺可破(`1-02` §2.1) |
| 删已有**仅 `experimental`** 条目 | **不动** | 不动 | **+1** | →0 | 同上 |
| **空解冻**(解冻后未改/删任何已冻结条目) | **不动** | 不动 | 不动 | 不动 | `r1/06` §4.3 规则 2 |
| **重大产品版本**(能力代际) | 不动 | **+1** | →0 | →0 | 人工声明, 与接口面无关 |
| 小功能(面未动) | 不动 | 不动 | **+1** | →0 | — |
| 修 bug / 改文档(面未动) | 不动 | 不动 | 不动 | **+1** | 需求方规则 |
| 面完全未变(冗余重跑) | 不动 | 不动 | 不动 | 不动 | **空操作**, 见 §6.5 幂等 |

- **右段清零**: `MAJOR` 增量 ⇒ `MINOR`/`REVISE` 归零; `MINOR` 增量 ⇒ `REVISE` 归零。
- **`COMPAT_GEN` 不清零任何段**(它与产品版本轴正交); 其增量只对"已承诺面"有语义。
- **单调不回退**: 任一段只增不减; 由工具校验(`BRV-VER-0007`)。
- **`REVISE` 的唯一驱动是人工声明**(关闭 `r1/01` P0-3③): v0.1 只承认 `brickie ver bump <plugin> --rule revise`(或 `iface publish --set`)产生 `REVISE+1`; **`brickie iface publish` 自身在任何情况下都不推进 `REVISE`**(面未变 ⇒ 空操作)。v0.1 无编译/符号/测试 ⇒ 工具结构上**不可能**观测"实现变了但面没变" ⇒ "实现身份 hash"这类真实信号源是 **v0.3+** 的对象(`1-03` 的构建面交付后); 在那之前, 谁想把实现变更记进版本, 谁就必须敲这条命令。

> **⚠ "修 bug" 与 "冗余重跑" 的面**完全相同**, 靠「是否显式声明」区分**(这是本节最易误读之处):
>
> | 意图 | 命令 | 结果 |
> |---|---|---|
> | 修了 bug / 改了实现或文档(面不变) | `brickie ver bump <unit> --rule revise` | `REVISE+1` |
> | 无意图地重跑 publish(CI 重跑 / 手工重复) | `brickie iface publish <unit>` | **空操作**(不推进任何段、不写盘、退出 0) |
>
> 理由: v0.1 **无编译、无符号、无测试**(§0 边界)⇒ 工具**看不见**"实现变了但面没变"。若让 `publish` 自行判断, 只能退化为"每次都 `REVISE+1`"(首版正是如此, 导致每次 CI 重跑都烧掉一个版本号 —— 评审 `r1/01` P0-3 / `r1/03` P0-1)。因此 `REVISE` 必须是**人工显式意图**, 而不是工具的猜测。

### 5.3 冻结与解冻机制

#### 5.3.1 两条轴, 缺一不可

| 轴 | 取值 | 粒度 | 回答 |
|---|---|---|---|
| `status`(既有, `1-02` §2.1) | `experimental` \| `frozen` \| `deprecated` | **条目** | "这个条目受不受保护?" |
| `freeze_state`(**新增**) | `unfrozen` \| `frozen` \| **`unfreezing`** | **接口单元** | "这个单元当前在不在冻结窗口里?" |

> `freeze_state = unfreezing` 是**瞬态**: 窗口内条目**仍是我们不打算放弃的承诺**, 只是正在**重新谈判** ⇒ 因此**不**引入第 4 个 `status` 值(否则 `frozen→unfreezing` 会被误读为"放弃承诺")。

#### 5.3.2 `frozen` 的语义 gloss(须与 `1-02` §2.1 对齐)

> `frozen` = **单向冻结(append-only 保护)**: 面**只能追加**, 不能改写/删除已承诺条目。
> 追加 ≠ 破坏承诺 ⇒ **无需解冻**(即"**新增=轻量 ≡ 新增免解冻**", 同一规则的两句话); 改写/删除 = 破坏承诺 ⇒ **必须走解冻 → 重新冻结**。

该语义**与 `1-02` §2.1/§2.2 既有定义逐字一致**(§2.1 行 46"新增=轻量; 语义/签名变更=决策记录+弃用周期"; §2.2 行 75"frozen 新增纯函数 = 轻量") ⇒ 本节**不重开**既有决策, 只是把它形式化并与版本段挂钩。**`frozen` 从来就不意味着"面不再生长"** —— 这与 glibc 的 `soname` 演化同构(`libc.so.6` 数十年不变而期间新增数千符号, soname 只在不兼容时 bump)。

#### 5.3.3 两条改变受保护面的路径

| 路径 | 动作 | `COMPAT_GEN` | 依据 | 适用 |
|---|---|---|---|---|
| **软** | 面内演化: 语义变更 / 弃用 → RFC + 弃用周期(不 unfreeze) | **不变** | `1-02` §2.2 / §2.6.2 | 不破坏源码兼容的演化 |
| **硬** | **`unfreeze` → 改/删已有条目 → `refreeze`** | **+1** | 本节 | 签名变更 / 删除 / 破坏性重排 |

> **两条要求独立**: 删除 frozen 接口走**硬路径**(`COMPAT_GEN+1`), **同时**仍须满足 `1-02` §2.6.5 的弃用周期 —— 一个管兼容代, 一个管通知期。

#### 5.3.4 解冻窗口三条规则

| # | 规则 | 理由 |
|---|---|---|
| 1 | `brickie iface unfreeze <unit>` 需**最高门槛 RFC**(`--note <决策记录>`) | 解冻让**全部依赖方强制重新验证**, 影响面大于单次签名变更 ⇒ 纳入 `1-02` §2.2 已有的"最高门槛"行 |
| 2 | `refreeze` 时**确有"改/删已冻结条目"才 `COMPAT_GEN+1`**; 否则不 bump | 精确对齐需求方"**已有接口发生变动**"—— 空解冻不该让全体依赖方重钉 |
| 3 | **`brickie check --profile release` 在任何单元处于 `unfreezing` 时失败**(`BRV-IFACE-0009`) | 防止 release 停在半谈判状态(与 §7.5 的 release 门禁同轴) |

命令面(并入 §6.5):

| 命令 | 作用 |
|---|---|
| `brickie iface unfreeze <unit> --note <path>` | 进入解冻窗口(`freeze_state: frozen → unfreezing`) |
| `brickie iface refreeze <unit> [--note <path>]` | 退出窗口; **按 §5.4 判定矩阵决定是否 `COMPAT_GEN+1`** |

### 5.4 ⚠ append vs modify: 判定必须落在**条目内部结构**上

**陷阱**: "新增无需解冻"**只在条目级成立**。给一个已冻结的**结构体加字段**, 直觉上是"新增", 但在 D14 的二进制兼容模型里是**修改**:

| 出处 | 原文 |
|---|---|
| `1-01` **D22**(行 132) | "动机 = D14: ops 布局入 golden, **后补字段 = 布局破坏**——**预留即免破坏**" |
| `1-02` §4.1-2 | "内联访问器…其**布局变更按硬门禁处理**" |
| `1-02` §4.1-4 / §2.4 | "**枚举 append-only** 进 golden"; "枚举只追加、**不重排**、不当位标志跨版本扩展" |

因此 `brickie check` **不能只比"条目集合的差"**, 必须比**条目内部结构**, 按 kind 判定:

| 条目 kind | 「追加」(免解冻) | 「修改」(**须解冻**) |
|---|---|---|
| `func` | 新增一个函数 | 签名变更 / `--strict-params` 下形参名变更 / 语义变更 |
| `type`(struct) | 新增一个**独立**结构体 | **给已有结构体加/删/重排字段** ← D22 |
| `enum` | **末尾追加**成员 | **重排** / 复用旧位 / 删除成员 |
| `service` | 新增一个 service 名 | 已有服务的 ops 表**加槽** / 改签名 |
| `macro` / `var` | 新增 | 改已有宏的值/定义 |

> **本条与 §6.2 的 hash 文法缺一不可**: 若 `macro`/`var`/`service` 条目**没有参与 hash 的字段**(评审 `r1/03` P0-2), 则本节这个陷阱**连检测手段都没有**。二者必须一起修。
>
> **D22 的启示**: 既有"**预留槽位**"设计在新模型下价值翻倍 —— 它把"将来必须解冻"变成"现在就已存在", 让 `COMPAT_GEN` 更稳定。建议把"**冻结前应预留扩展槽**"写进冻结评审清单(回灌 A-25)。

### 5.5 三件证据的分工(不可互替)

| 证据 | 回答 | 谁维护 | 频率 |
|---|---|---|---|
| **`COMPAT_GEN`** | "**承诺被破坏过几次**"(权威) | 工具强制(解冻/重冻序列) | 低频, 需 RFC |
| **`iface_hash`** | "**这个面具体长什么样**"(含追加) | 工具计算(IFACE-IR) | 高频 |
| 条目 **`status`** | "**哪些条目受保护**" | 人工声明 + 状态机校验 | 中 |

**由此得到发布报告的判定表**(**替代首版的 `iface_changed` 布尔**, 该布尔已可删除):

> **口径(先读这条, 否则会与 §6.2 规则 6 自相矛盾)**: 下表的 **`hash` 列只表示"进了 hash 的面"**(§6.2 规则 6: `status`/`freeze_state` **不进** hash)。**治理态转移不是面变更** ⇒ 它落在**第 2 行**(`hash` 不变)但结论必须写成"纯追加 **或** 状态转移"——`STATUS`/`FREEZE` 在变更集里是**独立类别**(§6.3), 不伪装成"hash 变"。

| `COMPAT_GEN` | `hash`(仅面) | 变更集 | 结论 | 对应需求方问句 |
|---|---|---|---|---|
| 不变 | 不变 | `NONE` | 面完全未动(**空操作**) | — |
| 不变 | 不变 | 仅 `STATUS` / `FREEZE` | **纯治理态转移**(面未动) | "**是否增加接口?**" → 否 |
| 不变 | **变** | `ADDED` / `EXTENDED` / 仅 `experimental` 条目改动 | **纯追加** | "**是否增加接口?**" → 是 |
| **+1** | 变 | `CHANGED` / `REMOVED`(涉及已 `frozen`/`deprecated` 条目) | **已有 `frozen`/`deprecated` 条目被改或删** | "**是否变更/删除了某个接口?**" → 是 |

**跨 `COMPAT_GEN` 比较无意义**(须写明): 依赖精确匹配 `COMPAT_GEN` ⇒ 求解**永不跨代比较**。`(F,M,m,r)` 字典序**只可用于同一 `F` 内**排序/展示。跨代不存在"更新/更旧": `F=4` 出现在 `F=3` 之后, 但 `F=4` 可能**删掉了** `F=3` 的接口 ⇒ 对依赖方而言是"**不兼容的另一代**"而非"升级"。迁移语义由 `deprecated` + 弃用周期承担, **不由版本序承担**。

### 5.6 版本文件(接口面快照)与兼容信息

需求方点名的"兼容信息"四件, 落点如下:

| 兼容信息 | 落点 | v0.1 |
|---|---|---|
| 版本文件 | `api/iface/<provider>/<unit>.toml`(接口面快照: 条目 + 状态 + `freeze_state` + 四段版本 + hash) | ✅ 生成 |
| 版本 hash | 快照与 `plugin.toml` 各 `[[export]].hash`(**按接口单元**; 插件级单数 `compat.iface_hash` 已删除, 见 §8.1.1) | ✅ |
| 对 core 的版本依赖 | `plugin.toml [compat].core = ">=1.0.0"` | ✅(区间求解参与) |
| 接口依赖(对其他插件接口单元的依赖) | `[compat].requires_iface = [{id, api_iface, compat_gen, range, mode}]` | **预留字段, v0.1 不扫描**(需求方明确"预留在文档中") |

`requires_iface` 的预留语义(写进 schema, v0.2 启用):

```toml
[[compat.requires_iface]]
id         = "framework/vfs-core#file"   # <provider>#<unit>
api_iface  = "native"                   # 被消费单元的分类; 必须与本插件 api_type 相容(§3.5)
compat_gen = 3                          # 必填, **精确匹配**(§5.5: 跨代比较无意义)
range      = ">=1.2.0"                  # 只比较 MAJOR.MINOR.REVISE(固定 3 段)
mode       = "decl"                     # decl(v0.1 语义) | sym(v0.2 符号级)
```

- **`compat_gen` 与 `range` 拆成两个字段**, 而非合成一个字符串: 二者语义不同(`compat_gen` 精确、`range` 范围), 混写无法分别校验(且违反 BRV-D2"禁止隐式")。
- **分类相容规则(v0.2 执法)**: `native` 插件不得 require `api_iface = "runtime_adapter"` 的单元(§3.5 消费方 2)——否则"不链适配基座"的裁剪承诺会从接口依赖这条侧门被绕过。

## 6. 接口发布机制(interface publishing)

### 6.1 对象模型

```
接口单元(iface unit)   = <provider>#<unit>       例: framework/vfs-core#file
  ├── 条目(entry)      = 面(surface)的一个成员
  │     ├── kind       : func | var | macro | type | enum | service | symbol-family
  │     ├── name       : 条目名(函数名/类型名/service 名)
  │     ├── sig        : 规范化签名字符串(仅 func; 见 §6.2)
  │     ├── layout     : 结构布局摘要 | "# opaque"(仅 type)
  │     ├── value      : 宏值 / 枚举成员表 / 常量值(仅 macro|var|enum; 见 §6.2 规则9)
  │     ├── ops        : ops 表槽位摘要(仅 service; 见 §6.2 规则9)
  │     └── status     : experimental | frozen | deprecated
  ├── version          : COMPAT_GEN.MAJOR.MINOR.REVISE(§5.2)
  ├── freeze_state     : unfrozen | frozen | unfreezing   ← 单元级瞬态(§5.3.1)
  ├── api_iface        : native | runtime_adapter | third_party   ← 恒等于提供者的 api_type(§3.5)
  ├── iface_hash       : sha256(canonical(surface))
  ├── hash_scope       : "decl"(v0.1) | "sym"(v0.x)
  └── truth            : "decl"(v0.1) | "header"(v0.x)   ← §6.6; **与 `hash_scope` 成对**(v0.1 只允许 decl/decl, 见 §10 V-11)
```

**粒度对齐 `1-02` D13(双层)**: v0.1 管**接口单元级**(人读层: 单元名/版本/状态/依赖), 并且**允许**在声明中显式列出条目(符号级**意图**); 符号级**真值**由 v0.2 的构建产物提取接管。**v0.1 的 `iface_hash` 是声明面 hash, 不是 ABI hash**——这条必须印在快照文件头, 否则会制造"已经兼容"的假安全感(RV-3)。

> **`value`/`ops` 字段是**评测 `r1/03` P0-2 的修法**: 若不给 `macro`/`var`/`enum`/`service` 定义参与 hash 的字段, 则 `#define BR_MAX 16→4096` 与"已有 service 的 ops 表加槽"都会算出**同一 hash**、变更集报 `NONE`。而这两种变化前者是**面变更**、后者按 §5.4 是**必须解冻的 modify** ⇒ 缺这两个字段会使 §5.4 的判定矩阵**无输入**。

### 6.2 IFACE-IR 规范化规则(定死, 否则 hash 抖动)

| # | 规则 | 理由 |
|---|---|---|
| 1 | 条目按 `(kind, name)` **字节序**升序 | 声明顺序不得影响 hash |
| 2 | 空白归一(单空格/无尾随) | 排版不得影响 hash |
| 3 | **参数名不参与** sig(只留类型与顺序); `--strict-params` 可选开启 | 形参改名是源码兼容的, 不应抖 hash |
| 4 | 类型别名展开到规范名; 依赖 `typedef` 表显式声明(`[iface.typedefs]`) | 避免"同一类型的两种写法"产生两个面 |
| 5 | 注释/文档字符串**不进** hash(但进快照) | 文档改动 ≠ 接口变更 |
| 6 | `status` / `freeze_state` **不进** hash(单列为 `STATUS` / `FREEZE` 变更类别) | 治理属性, 不是面 |
| 7 | 枚举: `append-only`(只追加不重排), 成员**按声明序**输出, 值进 hash | `1-02` §2.4 纪律; 明确输出序才能检测重排 |
| 8 | hash = `SHA-256` over UTF-8 canonical text; 展示 12 hex | 与 `1-02` 的 hash 纪律同族 |
| 9 | **`macro`/`var` 取其值、`enum` 取成员表、`service` 取 ops 槽位摘要**进 hash | 关闭 `r1/03` P0-2(缺字段 ⇒ 面变化不可见) |
| 10 | hash 做**域分隔**(unit id + hash_rev 前缀), 且 `strict_params` **存进快照** | 防跨单元碰撞; 防跨环境 hash 抖动 |

> **规则 4 依赖的 `[iface.typedefs]` 表必须写进 §8.1 schema** —— 首版只在规则里提到它, schema 中没有该表(评审 `r1/01` P1-1), 规则不可实现。

### 6.3 变更集(change set)与"标记接口变更状态 + 依赖的插件"

`brickie iface publish` 对**旧快照 vs 新面**做 diff, 产出变更集, 并**必须**附上受影响者。**判定先分「追加 / 修改」**(§5.4), 再定治理后果:

| 类别 | 判定 | 是否须解冻 | 硬性要求 |
|---|---|---|---|
| `ADDED` | 新增**条目**(新增函数 / 独立结构体 / 新 service 名) | **免** | 无(绿) |
| `EXTENDED` | 已冻结条目的**合法追加**(枚举末尾加成员 / 填充预留槽位) | **免** | 无(绿); 按 §5.4 判定表校验是"追加"而非"修改" |
| `CHANGED` | 已有条目的 **sig/layout/value/ops** 变化(**含结构体加字段**) | **必须**(已 `frozen` 条目) | 必须处于 `unfreezing` 且 `--note <决策记录>` |
| `SEMANTIC` | **纯语义变更**(行为/错误码/ISR 安全性变了, 但 `sig`/`layout`/`value`/`ops` 一个都不动 ⇒ **hash 不变**) | **免**(走**软路径**) | 完整决策记录 + 弃用周期(`1-02` §2.2/§2.6.5); `COMPAT_GEN` **不变**, `MINOR+1` |
| `REMOVED` | 条目消失 | **必须**(已 `frozen` 条目) | 必须已 `deprecated` + 满足弃用周期(`1-02` §2.6.5; **v0.1 口径见 §6.4** —— 声明面硬门 + 符号面软门) |
| `STATUS` | 三态转移 | 免 | 合法性按 `1-02` §2.6.2 状态机校验(如 `experimental→deprecated` 直跳 ⇒ 红) |
| `FREEZE` | `freeze_state` 转移(`unfreezing` 进/出) | — | 进出均需 `--note`; `refreeze` 决定是否 `COMPAT_GEN+1` |
| `NONE` | 面未变 | — | **空操作**(不写盘、退出 0), 见 §6.5 |

> **`SEMANTIC` 为什么必须单列(关闭 `r1/02` P0-3)**: 首版的类别表**没有语义变更通道**, 于是 §5.3.3 的软路径(语义变更 ⇒ `COMPAT_GEN` 不变)与 §5.4 判定表(把 `func` 的"语义变更"列进"须解冻")**互相打架**。现按 §5.4 的口径收口: **`type`/`enum`/`service` 的"语义变更"必然反映为 `layout`/`value`/`ops` 变化 ⇒ 归 `CHANGED`(硬路径)**; 只有 **`func` 的纯行为语义变更**不触碰任何 hash 输入 ⇒ 归 `SEMANTIC`(软路径)。一条"必须解冻"的规则与一条"代不变"的路径因此各归其位。

**组合变更集**: 一次 publish 可同时含多类(如 `ADDED` + `CHANGED`)。**判定取最严**: 存在任一 `CHANGED`/`REMOVED`(涉及已冻结条目)⇒ `COMPAT_GEN+1`(**并强制**该单元处于 `unfreezing`); 否则只要存在 `ADDED`/`EXTENDED`/`STATUS`/`SEMANTIC` ⇒ `MINOR+1`; 全 `NONE` ⇒ 空操作。**`--profile release` 下**: 命中"须解冻"却不在 `unfreezing` ⇒ `BRV-IFACE-0009`(§5.3.4 规则 3); 命中 `SEMANTIC` 但缺决策记录 ⇒ `BRV-IFACE-0010`。

**影响报告(dependents report)**——需求方要求的"标记接口变更状态, 依赖的插件":

```
$ brickie iface publish framework/vfs-core --check --json
{
  "unit": "framework/vfs-core#file",
  "changes": [
    {"kind":"CHANGED","entry":"br_open","from":"…","to":"…","status":"frozen"},
    {"kind":"EXTENDED","entry":"br_open_flags","note":"enum 末尾追加成员"}
  ],
  "compat_gen_changed": true,
  "version": {"from":"1.2.0.0","to":"2.0.0.0",
              "reasons":["COMPAT_GEN+1: 已有 frozen 条目被改","MINOR→0"]},
  "dependents": {
    "direct":   ["fs/tmpfs","app/hello"],
    "transitive": ["app/hello"],
    "unsatisfied": [{"plugin":"fs/tmpfs","requires":{"compat_gen":1,"range":">=1.0.0"},
                     "why":"compat_gen 已由 1 变 2, 需重新确认"}]
  },
  "verdict": "red"
}
```

> **`compat_gen_changed` 替代首版的 `iface_changed` 布尔**: 后者与 `COMPAT_GEN` 是否变动、`hash` 是否变动构成三重冗余(§5.5 的判定表已完整确定三者关系) ⇒ 删除, 少一个真值。

**反向依赖索引**: `build/index/dependents.json`(派生物, 由 `brickie dep index` 全树重建)。接口发布**只读**它, 缺失则先重建并提示——避免接口发布依赖陈旧索引。

### 6.4 三态 + 冻结瞬态在 v0.1 的落地

`1-02` §2.1/§2.6.2 的三态与状态机**原样继承**(语义**不修改** —— §2.1 行 46 本就写明"新增=轻量", 与本篇"新增免解冻"逐字一致), v0.1 实现其**声明与转移校验**部分, 并**新增**解冻瞬态(§5.3):

| 能力 | v0.1 | 说明 |
|---|---|---|
| 在条目上标注三态 | ✅ | `status = experimental\|frozen\|deprecated` |
| 状态转移合法性校验 | ✅ | 按 `1-02` §2.6.2 转换表 |
| **单元级 `freeze_state`**(含 `unfreezing` 瞬态) | ✅ | §5.3.1; **新增机制** —— 已回灌 `1-02` §2.6.2(该篇原无此路径, 本轮补入状态机与图) |
| `unfreeze` / `refreeze` 门钩 | ✅ | 最高门槛 `--note`; `refreeze` 决定是否 `COMPAT_GEN+1` |
| **append vs modify 判定**(§5.4) | ✅ | 按条目 kind 判"是追加还是修改" —— 这是"新增免解冻"能否安全成立的关键 |
| 冻结面变更 ⇒ 需决策记录 | ✅ | `--note` 门钩(§6.3) |
| 弃用周期(两个 minor 无使用)计数 | ◐ | **v0.1 的"使用" = 声明面使用**(该单元出现在某个 `requires_iface` / `reexport_of` 里, §5.6/§3.5); **真实使用统计(谁在代码里调了它)需符号级 ⇒ v0.2**(§6.6 的 `hash_scope` 升级)。**口径: v0.1 的弃用周期 = "声明面硬门 + 符号面软门"** —— **声明面零使用是硬门条件**(可统计、可阻断 `REMOVED`); **符号面零使用在 v0.1 无法验证** ⇒ `release` 下**降级为 warning**(`--profile dev` 下只提示)。(r1/03 P1-5) |
| 编译期 `deprecated` 警告 | ✗ | 需生成头文件与编译 ⇒ v0.3 |

> **⚠ `freeze` 的前置条件缺口(须显式声明)**: `1-02` §2.6.2 规定 `EXPERIMENTAL→FROZEN` 的前置是"**当期已交付调度器的 conformance 矩阵全绿**"(层 2), 且升格产物含"**golden 收录**"。而本篇把 test/conformance 排到 **v0.4**、golden 排到 **v0.6** ⇒ **v0.1 无法完成一次合法冻结**。
> **v0.1 的处理(采纳评审 `r1/02` P0-2 的建议)**: `brickie iface freeze` 在 v0.1 **只能生成"待升格提案"**(写入 `build/gen/proposals/<unit>.toml` + 要求 `--note`), **不得落 `frozen` 快照、不得 bump `COMPAT_GEN`**; 真正的升格在 v0.4+(有矩阵)执行。这条是**显式例外声明**, 不是对 `1-02` §2.6.2 的静默偏离(回灌 A-26)。

### 6.5 发布命令面与幂等

| 命令 | 作用 | 副作用 |
|---|---|---|
| `brickie iface list [--json]` | 列出插件树内全部接口单元与状态 | 无 |
| `brickie iface show <id>` | 展示接口面(条目 + `status` + `freeze_state` + 四段版本 + hash) | 无 |
| `brickie iface diff <id> [--json]` | 旧快照 vs 当前声明的变更集(§6.3) | 无 |
| `brickie iface publish <id> [--check] [--note <path>] [--set <ver>] [--profile dev\|release]` | 计算 → diff → 影响报告 → 版本推进 → 落盘 | **机器只写 `api/iface/**` 与 `brickie.lock`**(`plugin.toml` **只读**, 见 §9.1 写路径纪律); **`--profile release` 启用发布前门钩**(见下) |
| `brickie iface status <id> [--check]` | 独立重算 hash 与快照比对(CI 门禁用; 防手编) | 无 |
| `brickie iface freeze <id>[#entry]` | `status: *→frozen`(承诺升级); 须 `--note` | 写快照 + 要求 `--note` |
| `brickie iface deprecate/undeprecate <id>[#entry]` | `status: frozen↔deprecated`; 须 `--note` | 同上 |
| **`brickie iface unfreeze <unit> --note <path>`** | **进入解冻窗口**(`freeze_state: frozen→unfreezing`); 最高门槛门钩 | 写快照 |
| **`brickie iface refreeze <unit> [--note <path>]`** | **退出窗口**; 按 §5.4 判定矩阵决定是否 `COMPAT_GEN+1` | 写快照 + 可能 bump `COMPAT_GEN` |

**幂等纪律**(修正首版的自相矛盾——首版写"NONE ⇒ `d+1`"却又宣称重复 publish 无 diff, 每次调用都烧掉一个版本号):

> **面内容幂等**: 若 `canonical(new surface) == snapshot.iface_hash` **且** 条目集合/`status`/`freeze_state` 全等 ⇒ `publish` 是**空操作**(不写任何文件、不推进任何段、退出 0; `--check` 逐字节一致)。

要点:
1. **`NONE` 不再 `REVISE+1`** —— 首版的 `NONE ⇒ d+1`(例"修 bug/改文档")在 v0.1 **不可观测**(无编译、无符号、无测试 ⇒ 工具看不见"实现变了但面没变")。`REVISE` 改为**人工声明**(`brickie ver bump --rule revise`)。2. **diff 与 hash 一律忽略** `version` / `hash` / `status` / `freeze_state`(§6.2 规则 6) —— 否则 publish 自己写回的字段会成为下一轮的输入, 形成 `COMPAT_GEN+1` 的**无限升级环**(评审 `r1/03` P0-1)。
3. **`--set` 必须 ≥ 当前版本**(单调性校验 `BRV-VER-0007`)。
4. 重复 `--check` ⇒ 逐字节一致(继承 `docs/render-plantuml.sh` 的"按内容而非 mtime"纪律, `2-02` BR-D5)。

**发布前门钩 `--profile release`**(关闭 `r1/05` §8.4-4 的"门钩缺失"): `publish` 默认按 `dev` 语义放行; 显式传 `--profile release` 时, 落盘前**先跑一次 release 级前置检查**, 任一不满足 ⇒ 拒绝落盘(退出码 1):

| 前置 | 诊断 |
|---|---|
| 单元不处于 `unfreezing`(§5.3.4 规则 3) | `BRV-IFACE-0009` |
| 命中"须解冻"的变更集(`CHANGED`/`REMOVED` 涉及已冻结条目)时, 必须处于 `unfreezing` 且带 `--note` | 缺 `--note` ⇒ 退出码 1(与 V-6 同口径) |
| 命中 `SEMANTIC` 时必须带 `--note`(决策记录) | `BRV-IFACE-0010` |
| 引擎结论与 `--check` 独立重算一致(防手编快照) | `BRV-IFACE-0009` 同族 |
| 变更后依赖方仍满足 `compat_gen` 精确匹配(影响报告里无 `unsatisfied`) | `BRV-VER-0001` |

> 门钩与 `brickie check --profile release`(§7.5)**同轴不同点**: `check` 是"组合能不能过", `publish --profile release` 是"**这次发布能不能把别人弄红**" —— 把"先发布再发现依赖方失配"的顺序倒过来。CI 的 release 阶段两条都跑。

### 6.6 v0.1 → v0.x 的真值迁移(BRV-D7 的落地)

```
v0.1  truth="decl"    声明面 = 真值   →  iface_hash = H(声明面)
        │      迁移窗口: v0.2 引入头文件/符号提取, 两算并行, 强制一致
v0.2+ truth="header"  头文件 = 声明真值, 构建产物 = 机器真值, 声明面降级为"意图"
        →  iface_hash = H(符号面)(hash_scope="sym"); 声明面 hash 保留为 decl_hash
```

**迁移硬约束**: v0.1 的快照文件**必须**带 `hash_scope` 与 `truth` 字段; v0.2 起 `brickie iface check` 对 `truth="header"` 的单元执行 **双算一致**校验(decl_hash 与 sym_hash 的**条目集合差**必须为空), 不一致 ⇒ 红。这样"迁移"是一次可验收的开关, 而不是一次静默的真值偷换。

## 7. 依赖管理与分析

### 7.1 三类依赖(继承 `1-01` §6.5 的两分, 新增 `type`)

| | init 依赖 | runtime 依赖 | **type 依赖**(新增) |
|---|---|---|---|
| 声明 `kind` | `"init"` | `"runtime"` | **`"type"`** |
| 含义 | "你必须先 init 完我才能 init" | "我运行时会调你" | **"我只需要你的头文件/类型可见"** |
| 环政策 | **禁止** ⇒ 拓扑硬错误, 报完整环路径 | **允许** ⇒ 只报 `info` | **允许** ⇒ 只报 `info` |
| 相位约束 | **有**(§7.2) | 无 | 无 |
| 闭包参与 | ✅ | ✅ | ✅(**但不拉入运行期依赖**, 见下) |
| 钉 `compat_gen`? | ✗(依 F3) | ✗(依 F3) | ✗(依 F3) |
| v0.1 执法 | 硬错误 | 只登记 + 影响报告(§6.3) | 只登记 |

> **为什么必须有 `type`**(评审 `r1/03` P0-3③): `dev-core → vfs-core` 是**仅头文件类型依赖**(`8-01` §1.3 / O-S7)。旧模型只有 `init|runtime` 两值 ⇒ 写成 `runtime` 会按"runtime 参与闭包"把 vfs-core **拉进组合**, 从而落入 `8-01` 的"形态 B", **推翻 `1-01` D19 / O-S7**(设备框架本可独立于 VFS 成立)。`type` 边表达"编译期需要类型、运行期不需要该插件在场"。
>
> **与 §7.5 profile 的关系**: 三类结构依赖**都不钉 `compat_gen`**, 故都不参与 "release 不允许依赖未冻结接口" 的判定 —— 这正是 **M0–M2 能继续工作**的原因(其依赖全是结构边)。

### 7.2 init-DAG 与相位一致性(§7.3 之外的第二条硬约束)

`1-01` §6.2 定义四相 `EARLY < CORE < LATE < APP`, 且"同阶段内按依赖拓扑序"。由此得到一条**既有文档没有明说但必须执法**的规则。

**先把"完成点"写死(否则规则无法表达——评审 S-1)**: 每个插件有**两个完成点**;

| 完成点 | 何时到达 | 覆盖谁 |
|---|---|---|
| ① **注册可用** | EARLY 相的 `early_init` 返回之后 | 全部插件(`early_init` 是全体回调) |
| ② **能力可用** | 该插件 `phase` 所声明的相的 `init` 返回之后 | 有 `init` 钩子的插件; 声明 `phase = "early"` 的插件**没有** `init` 钩子, ② 与 ① 重合 |

`[plugin].phase` 声明的是**第 ② 个完成点**; 若某插件**没有** `init` 钩子(纯 `early_init` 注册者), 其 `phase` 必须是 `"early"`(② 落在 EARLY)。`init` 的相由 `plugin_type` 决定, 与声明必须一致: 非 Service/Interface ⇒ CORE; Service/Interface ⇒ LATE(即 `subkind = service` 或 `plugin_type = interface` 的插件声明 `core` ⇒ schema 错)。

> **相位单调规则(两条, 一条都不许省)**: 对每条 `kind = "init"` 的声明边 `A → B`(A 依赖 B),
> **(R1) 完成点单调**: `rank(complete(B)) ≤ rank(complete(A))`, 其中 `rank(EARLY)=0 < rank(CORE)=1 < rank(LATE)=2 < rank(APP)=3`;
> **(R2) 依赖方自检**: `A` 的 `early_init` 完成点(EARLY) 必须早于 `A` 的 `init` → 恒成立, 无需校验; 但 `A` 若声明了 `[[dep]].phase`(§8.1 的断言), 必须满足 `rank(assertion) ≤ rank(complete(B))`。
> 违例 ⇒ 组合期错误 `BRV-DEP-0009`(报"B 在 A 之后完成, 但 A 依赖 B") / `BRV-DEP-0010`(断言与提供方自述冲突)。

**用规则 (R1) 复核真实插件(修复 S-1 的验收用例)**:

| 边 | 完成点 | 判定 |
|---|---|---|
| `sched-coop → platform/qemu-aarch64` | `platform.init` 在 **CORE** 完成(② = CORE); `sched-coop.init` 在 **CORE** 完成(② = CORE) | `CORE ≤ CORE` ✅ **不误杀** |
| `service/* → framework/*`(服务 → 框架件) | Service 的 ② 在 **LATE**; 框架件 ② 在 **CORE**(非 Service ⇒ CORE) | `CORE ≤ LATE` ✅ |
| `iface-posix → svc-posix`(皮肤 → 服务) | `iface-posix` ② 在 **LATE**; `svc-posix`(service)② 在 **LATE** | `LATE ≤ LATE` ✅ |
| 反例: `dev-core(phase=core) → sched-coop(phase=core)` 而 `dev-core` 又在 `sched-coop.early_init` 里被注册? | 该边不是 `init` 边(是运行期注册) ⇒ **规则不管**; 若硬写成 `init` 边, 两边同为 CORE ⇒ 被拓扑序决定, 不由相位报错 | ✅ |

> **规则只覆盖"已声明"的 init 边**(关闭 `r1/01` P1-4 的覆盖边界): 未声明的隐式耦合(判例: trace 的调用链 —— 谁在 `init` 里悄悄用了尚未 `init` 的服务)**不在 v0.1 范围**, 由 v0.2 的符号级接口依赖扫描 + 源码静态分析接手(`1-01` §6.4-3 后半)。这句话必须写在规则旁边, 否则规则会被读成"相位问题已全解"。
>
> **没有这条规则**的后果: 一个无环依赖图仍可能因相位顺序而**无法按声明的顺序初始化**——拓扑排序成功但启动序列失败, 属于典型的"构建期看不出来、运行期必炸"。**有了这条规则**, 组合期就能抓住它, 并且**不会**误杀 `sched-coop → platform` 这类"两边同在 CORE"的合法边。

### 7.3 分类学禁则(依赖方向的可执法形式)

依赖许可 = `api_type` 禁则(§3.1; **只对有面的提供方生效**, `app`/`interface` 的方向由 `plugin_type` 判定) ∧ `plugin_type` 方向(§3.2) ∧ `subkind` 收窄特例(§3.3; 即 `allow_edges` 白名单与 `subkind = scheduler` 恰 1, **不新增规则**)。(r1/03 P1-9)**`third_party` 是"位置"而非新类别**(§3.2 末行), 故单列一列以便回答"谁来消费三方件":

| 消费者 ↓ \ 提供者 → | platform | ability | interface | **third_party**(`api_type = "third_party"`) |
|---|---|---|---|---|
| platform | — | ✗ | ✗ | ✗ |
| ability | ✅ | ✅(单向; 框架件间按 `7-01`/`8-01` 特别清单) | ✗ | ✅(**唯一合法消费者**; `kind = "type"`/`runtime` 均可, 能力经注册表或 `third_party` 接口单元) |
| interface | ✅(仅 native/core 面) | ✅ | ✅(仅再导出) | ✗(第三方面不做皮肤: 皮肤只再导出被治理的单元) |
| app | ✗ | ✗(**不得直调 native**) | ✅ | ✗(`app` 只见 interface; 三方能力由 `ability` 包装后经注册表面向 APP) |

三条硬禁则(v0.1 可全量执法): (1) `app` 不得依赖 `ability`/`platform`; (2) `native` 不得依赖 `runtime_adapter`; (3) `interface` 不得被 `app` 以外的任何插件依赖(严格叶子)。**第四条的精确形式**(不写成第四条硬禁则, 因为它由前三条导出): `native` 插件不得依赖 `api_type = "third_party"` 的插件(§3.1 表的"不得被 `native` 依赖")。

> **"谁来消费三方件"的唯一解**: 上表把消费者收窄到 `ability` 一行 —— 与 `1-01` §7.6 的"由 native 包装件持有接口单元"一致。`svc-posix`(runtime_adapter)对 sqlite 只有 **`kind = "type"` 的编译期类型边**(§3.1 的显式例外); **运行期能力一律经注册表**(BRV-Q13), 不产生接口单元依赖。这条同时关闭 `r1/03` P0-3④(U-1: lwip/sqlite 的 `api_type` 归类)的争议面: `service/lwip`/`service/sqlite` 的 `plugin_type = ability`、`api_type = third_party`(携带上游源码), 它们是**消费者**而非被 native 依赖的提供者。

> **禁则 (2) 的两条精确化补充**(§3.5): (a) 在 v0.2 起, 它同时约束**接口粒度** —— `native` 插件不得 require `runtime_adapter` 分类的接口单元; (b) `form = "skin"` 的**再导出边豁免**该禁则(否则 `iface-posix` → `svc-posix` 被误杀), 豁免必须由 `reexport_of` 显式声明。

> **框架件之间的特例**不写成通用规则: `cdev-core→dev-core`、`bdev-core→dev-core`、`cdev-core→vfs-core(类型)` 这类边由 `7-01`/`8-01` 的显式白名单承载(v0.1 以 `allow_edges` 清单形式内置), 否则通用规则会被迫放宽到无法执法。

### 7.4 版本区间语义(3 段范围 + 精确 `compat_gen`)

**依赖 = `(compat_gen 精确值, range 范围表达式)`**: `compat_gen` **精确匹配**(§5.5: 跨代比较无意义), `range` 只作用于 `MAJOR.MINOR.REVISE`。

| 写法 | 语义 | 备注 |
|---|---|---|
| `=M.m.r` | 精确 | 需求方点名 |
| `>=M.m.r` / `>M.m.r` / `<=M.m.r` / `<M.m.r` | 按 `(M,m,r)` **字典序**比较 | 需求方点名 |
| `~M.m.r` | 允许 `r` 升, `m`/`M` 固定 | 可选语法糖 |
| `^M.m.r` | 允许 `m`/`r` 升, `M` 固定 | 可选语法糖 |
| `*` | 任意(仍受 `compat_gen` 精确匹配约束) | — |
| `,` | 交集(AND) | — |
| **`>=F.M.m.r`(4 段)** | **非法** `BRV-VER-0006` | 直接落实"只比较 `M.m.r`" |

> **`^` 与 `~` 的争议在首版存在、现已消失**: 首版因主版本是第一段的函数, 二者**外延完全相同**(评审 `r1/01` P0-2 实测各覆盖 28 个可达版本)。现 `compat_gen` 已精确钉住, `MAJOR` 恢复独立 ⇒ `^`(跨 `m`)与 `~`(不跨 `m`)**自然分开**。二者是否保留为语法糖属可选优化, 不影响正确性。

**`^` / `~` 的等价区间式(写死, 否则"等价形式已给"是空头宣称——评审 `r1/05` §3.2-1/3.2-2)**: 记当前版本为 `M.m.r`,`range` 只作用于 `(M,m,r)`:

| 语法糖 | 等价的显式区间 | 语义 |
|---|---|---|
| `~M.m.r` | `>=M.m.r,<M.(m+1).0` | 允许 `r` 升, `m`/`M` 固定 |
| `^M.m.r` | `>=M.m.r,<(M+1).0.0` | 允许 `m`/`r` 升, `M` 固定 |

> 二者**都不含** `COMPAT_GEN` 段(`COMPAT_GEN` 由独立字段**精确匹配**, §5.5/§7.4 首行); 求解器只需实现区间交集, 语法糖在解析期**一次性展开为上表的显式区间**(`--json` 输出恒为展开后的形式, 避免两种真值)。

**版本 arity 归一化(关闭评审 `r1/03` P1-13)**: 既有文档混用 2/3/4 段(`1-01` §7.7 现给出归一示例; 首版写 `">=1.0.0"` 与 `">=0.1.0.0"`)。规则: **`range` 固定 3 段, 缺段右补 0**(`">=1.0"` ≡ `">=1.0.0"`), 4 段即错。

**单版本政策(v0.1 定)**: 同一插件名在闭包内**只允许一个版本**; 多版本共存 ⇒ 组合期硬错误 `BRV-DEP-0011`(关闭 `4-04` §3「多版本共存」——v1 明令禁止)。

### 7.5 `profile` 门禁(dev / release)

**规则**(需求方口径):

| profile | 依赖**未冻结**接口(`compat_gen = 0`) | 诊断 |
|---|---|---|
| **`dev`** | **允许** | `BRV-VER-0004` 降为 **info**(仅提示"该依赖未受保护") |
| **`release`** | **禁止** | `BRV-VER-0004` **error** ⇒ 退出码 1 |

- **适用范围**: **接口消费**(`requires_iface` / `[[export]].form="skin"` 的再导出边)。**不适用**于结构依赖(`init`/`runtime`/`type`)——依 F3 结构依赖不钉 `compat_gen`, 故不参与本判定。这条边界正是 **M0–M2 能继续工作**的原因(其依赖全是结构边)。
- **为什么是 profile 开关而非强度分级**: 需求方口径是"**阶段**决定严格度", 而非"同一阶段内可调松紧" ⇒ 二者语义不同, profile 更干净。
- **CLI 与 manifest**: `brickie check --profile dev|release`(默认 `dev`); product manifest 可声明默认值 `[product] stage`, **CLI 覆盖 manifest**。
- **与镜像 profile 的关系(须写明, 否则会被误读为同一件事)**: 本 profile = **组合期检查严格度**(工具侧); 既有 `release 构建`(`1-01` §13 `brickie build --release`; `3-02` IR-15/INV-C 的 debug=panic / release=trace)= **镜像内断言行为**。二者**不同轴**, 但**建议耦合**: `brickie build --release` 应**强制**以 `--profile release` 完成检查, 否则会出现"release 镜像由 dev 级检查放行"的漏洞。
- **CI 落地**: dev 门禁全量跑; release 门禁**唯一多出的一条**就是 `--profile release` 的 `BRV-VER-0004`(加 §5.3.4 规则 3 的 `unfreezing` 阻断)。

> **⚠ 由此照出一条必须补的排期空白**(评审发现的连带问题): "release 不允许依赖未冻结接口"把**冻结排期**变成 release 的**硬前置**, 而首版的 `3-01` §15 冻结计划**只覆盖 core native 组**(`br-sched`/`mem`/`mm`/`irq`/`svc`)。**框架件四件**(`br-devcore` 等, `3-01` §0 行 44 有文件名)、**svc-posix POSIX 面**(`br-svcposix.txt`, §0 行 45)、**crypto/keyring ops** 均**有治理声明却无冻结批次** ⇒ 任何依赖它们的 release 会被 `BRV-VER-0004` **永久阻断且无排期可解**。
>
> **已回灌(原 A-18/A-19, 2026-xx 落定)**: `3-01` §15 冻结计划已补 **第四批(框架件四件, M2/M3 起分批)**、**第五批(`svc-posix`, M3)** 与**第六批(`crypto`/`keyring` ops, M5 随服务契约)**, 且明确**以接口单元为粒度**(A-16); `9-02` §6.2 的 **O-H7 已关闭**(crypto/keyring ops **入 golden**, 与框架件同级), `11-01` §3 对应开放问题已撤。⇒ 现行排期下 release **不再存在无解组合**。

### 7.6 闭包求解(纯函数, 单测友好)

```
求解器签名(概念):
  solve(plugin_tree, product, profile) -> Closure | Diagnostics
  Closure = { plugins: [{name, version, compat_gen, api_type, plugin_type, phase, budget}],
              init_edges, runtime_edges, topo_order, totals, profile }
```

- **求解顺序**: 产品 manifest 选择集 → init+runtime 边闭包 → **`compat_gen` 精确匹配** → `range` 交集(单版本) → 拓扑 + 环检测 → 相位单调 → 分类学/特权声明 → 预算合计 → **profile 判定(§7.5)**。
- **求解器是纯函数**(无 IO), IO 全在 Python 粘合层与 Rust 的 `brickie-model` 加载器——满足 `2-02` §4 建议 1(可单测, 不依赖编译器与 QEMU)。`profile` 作为**入参**传入, 不影响纯度。
- **报错可解释**: 环报完整边路径; **版本冲突报"谁钉了哪一代 `compat_gen` / 提供方在哪一代"**; 缺失报"依赖名 + 哪条边引入"。

> **版本冲突从"不可构造"变为可构造**(关闭评审 `r1/03` P1-14): 首版单版本政策 + 一处 `plugin.toml` ⇒ 求解器**永远看不到第二个候选**, "冲突"实为"缺失"。现因 `compat_gen` **精确匹配**, 冲突有真实来源:
> - 依赖钉 `compat_gen = 3`, 而提供方当前在 `compat_gen = 4`(或 2)⇒ `BRV-VER-0001`
> - 同代内 `range` 越界(提供方 `1.5.0` 而依赖要求 `>=2.0.0`)⇒ `BRV-VER-0002`
>
> 两者都可能因"上游重新冻结"或"上游发新版本"而**在不改动依赖方声明的情况下**发生 —— 这正是 release 门禁要挡的东西。

### 7.7 命令面

| 命令 | 作用 |
|---|---|
| `brickie dep add <plugin> <dep>[@range] [--kind init\|runtime\|type] [--phase early\|core\|late\|app]` | 写 `plugin.toml` 并重算(§7.7.1 的旗标纪律) |
| `brickie dep rm <plugin> <dep>` | 同上 |
| `brickie dep tree [--kind init\|runtime\|type\|all] [--json]` | 依赖树 |
| `brickie dep graph --format dot\|mermaid\|json` | 图导出(复用 `--json` 的边模型) |
| `brickie dep why <a> <b>` | 最短依赖路径解释 |
| `brickie dep index` | 重建反向依赖索引 |
| `brickie dep closure [--json]` | 产品闭包 + 拓扑序 + 预算合计 |
| `brickie check [--deps\|--iface\|--tax\|--priv\|--all] [--profile dev\|release] [--json]` | 组合期校验(v0.1 覆盖 §2 表 + §7.5 profile) |
| `brickie ver show <plugin>` | 展示四段版本 + `compat_gen` 来源(哪个冻结事件) |
| `brickie ver bump <plugin> --rule major\|minor\|revise` | **人工声明** `MAJOR`/`MINOR`/`REVISE`(§5.2: 工具看不见"重大/小/修 bug"这三类事件) |

> `compat_gen` **不在** `brickie ver bump` 的可选 rule 里 —— 它只能由 `unfreeze`/`refreeze` 序列产生(§5.3), 拒绝手工任意指定; 这是"工具强制"与"人声明"的边界。

#### 7.7.1 `dep add` 的旗标纪律(关闭 S-3: 删除 `--frozen-gen` 残留)

| 旗标 | 处置 | 理由 |
|---|---|---|
| ~~`--frozen-gen N`~~ | **删除**(不是改名) | 三重错误: ① 段名已被 T1 定为 `compat_gen`(§5.1), CLI 自己不能用被否决的旧名; ② 它挂在 `dep add` 上, 而**结构依赖(`init`/`runtime`/`type`)一律不钉代**(§7.1 表, 依 F3)—— 该旗标对结构依赖**暗示钉代**; ③ 即便改名 `--compat-gen`, 它服务的对象是**接口依赖**, 而 v0.1 **不扫描接口依赖**(`requires_iface` 只校验 schema) ⇒ 参数面没有一个合法作用域。**`dep` 族的旗标只描述"结构依赖"**: `--kind` 决定边类型, `--phase` 决定 `kind="init"` 的相位断言(缺省 = 依赖方自身相位)。 |
| `--compat-gen N`(若将来需要) | **不引入** | 接口依赖在 v0.2 起由 `plugin.toml [compat.requires_iface]` 的 `compat_gen` 字段声明(§5.6), 由 `brickie iface` 族读写; 不通过 `dep add` 混杂写入口。 |

> 这条同时是 **§8.1 与 §7.1 的 CLI 投影**: 声明面里 `[[dep]]` 没有 `compat_gen` 字段, 命令行里也就不该有对应的旗标。**"CLI 不得比 schema 多一个语义"** —— 反之亦然。

## 8. 声明面 TOML schema(v0.1 定稿草案)

### 8.1 `plugin.toml`(插件级真值)

```toml
schema = 1

[plugin]
name        = "service/crypto"     # 全局唯一; 形态见 §8.3(名字前缀 = namespace, 此处 = subkind = service;
                                   #   名字只给 subkind, **不覆盖**下面的 plugin_type)  (r1/03 P0-7)
plugin_type = "ability"            # app | interface | ability | platform
api_type    = "native"             # native | runtime_adapter | third_party(**提供方视角**, §3.1)
                                   #   注: api_type 与名字无关 —— 名字里的 `service` 是 namespace, 不是 api_type(§8.3 ③)
                                   #   纯消费者(app/interface)照填自己的编码规范(v0.1 一律 native),
                                   #   但**不参与** api_type 依赖禁则(禁则由 plugin_type 判定)
subkind     = "service"            # ability 细分: scheduler|framework|io|fs|service
lang        = "c"                  # c | cxx | rust   (C++: 待定, 见 BRV-Q8)
phase       = "late"               # 本插件的第 ② 个完成点(§7.2): early|core|late|app
                                   #   early = 无 init 钩子(仅 early_init); core/late 由 plugin_type 约束
sched_class = "SAFE_PREEMPT"       # 调度兼容类别(1-01 §5.3): SAFE_PREEMPT|COOP_ONLY|TT_SAFE
                                   #   默认 SAFE_PREEMPT(与 1-01 §5.3 的默认一致); 组合期与调度器互查(§2 第 3 项)
                                     # TT_SAFE 须同时给 [sched.tt] 周期元数据(1-01 §5.3)
version     = "0.1.0.0"            # COMPAT_GEN.MAJOR.MINOR.REVISE(§5.2)
summary     = "密码服务: SHA-256 / HMAC-SHA256 / AES-CBC,CTR / DRBG"
license     = "WTFPL"

# ---- 资源需求(per-plugin; §2 第 5 项与 §7.6 预算合计的输入)----
[[res]]                            # 同一插件可有多条(如"栈"与"堆"), 合计按条目求和
kind     = "ram"                   # ram | stack
used_kib = 48                      # 该插件占用的 RAM(不含栈)或栈大小
# notes  = "crypto 上下文池"       # 可选: 人读说明

[compat]
core          = ">=1.0.0"                  # 对 core 的版本依赖(1-02 层 3; range 3 段)
api_rev       = 1                          # 编码面对的 native API 版本
hash_scope    = "decl"                     # decl(v0.1) | sym(v0.x)
truth         = "decl"                     # decl | header   (§6.6)
abi_id        = ""                         # v0.1 占位; v0.6 由工具链指纹填充
# iface_hash 见 [[export]].hash —— **按单元**而非插件级(§8.1.1 说明)

# ---- 接口依赖(v0.2 启用; v0.1 只校验 schema)----
[[compat.requires_iface]]
id         = "framework/vfs-core#file"   # <provider>#<unit>
api_iface  = "native"                   # 被消费单元分类; 必须与本插件 api_type 相容(§3.5)
compat_gen = 3                          # **必填, 精确匹配**(§5.5)
range      = ">=1.2.0"                  # 只比较 MAJOR.MINOR.REVISE
mode       = "decl"                     # decl | sym(v0.2)

# ---- 类型别名表(IFACE-IR 规则 4 的输入, 见 §6.2)----
[iface.typedefs]                           # 别名 → 规范名; 防"同一类型两种写法"抖 hash
u32 = "uint32_t"
br_thread_t = "struct br_thread"

# ---- 结构依赖(不钉 compat_gen; 依 F3)----
[[dep]]                            # init 依赖 ⇒ DAG 边
name    = "platform/qemu-aarch64"
range   = ">=0.1.0"                # 3 段, 缺段右补 0
kind    = "init"
phase   = "early"                  # 相位断言(可省; 缺省 = 本插件自身相位; §7.2 规则 R2)

[[dep]]
name    = "iface-min"              # runtime 依赖 ⇒ 调用边(允许环); "零开销直通 native"的 Interface 出口
range   = ">=1.0.0"                # 3 段, 缺段右补 0
kind    = "runtime"
symbol  = "br_open"                # 可选: 说明调用面(便于 v0.2 符号级核对)

# [[dep]]                          # type 依赖 ⇒ 仅头文件/类型可见(免于"形态 B"误判)
# name  = "framework/vfs-core"     # 框架件裸名 `vfs-core` 同样合法(§8.3 ②(a))
# kind  = "type"

# ---- 导出面(每单元一条; 单元 = 冻结与版本的基本粒度, 依 F1)----
[[export]]                         # §3.5: api_iface 必须等于 [plugin].api_type
api_iface    = "native"            # native | runtime_adapter | third_party(= api_type)
form         = "service"           # api | skin | service
name         = "crypto"            # 单元名 / 注册表名(form=service)
                                   #   **这是单元名, 不是插件名**: 插件名 = `service/crypto`(§8.3);
                                   #   裸名 `crypto` 作为**插件名**虽合法(§8.3 ②(a)), 但本例不用它  (r1/03 P0-7)
version      = "0.1.0.0"           # 单元自身四段版本(§5.2 按单元推进)
compat_gen   = 0                   # 单元级兼容代(§5.3); 0 = 尚未冻结
freeze_state = "unfrozen"          # unfrozen | frozen | unfreezing(§5.3.1)
hash         = "sha256:…"          # 单元面 hash(§6.2)
status       = "experimental"      # 单元级默认状态(条目可各自覆盖)
# entries: 符号级"意图"清单(§6.1); 展开为子表
# [[export.entries]]
# kind = "func"; name = "br_crypto_hash"; sig = "int (const uint8_t*, size_t, uint8_t*)"
# status = "experimental"
# reexport_of = ["…", "…"]         # 仅 form="skin": 被再导出的单元**列表**(§3.5 不变量 3)
# symbols     = ["…"]              # 仅 form="skin": 占有的符号族(1-01 §7.3 api_syms)

[privileged]                       # §3.4(v0.1 只校验声明合法性)
level = "P2"
[[privileged.memory]]
granularity = "pool"               # 池生命周期 ⇒ P2(正交表见 §3.4); map/protect 属 P4, 纯 ability 不得声明
ops         = ["alloc", "free", "flush", "invalidate"]
regions     = ["heap", "contig", "page"]
[[privileged.resources]]
irq = [32]
dma_channels = [3]
device_names = ["hsm0"]

[build]                            # v0.1 只记录; v0.3 起被消费
sources  = ["src/*.c"]
includes = ["include"]
```

> **`form="skin"` 的旗舰判例(把上面注释里的 `reexport_of` 写成真实样例; S-6 缺口 b 的验收用例)**:
>
> ```toml
> # iface-pkcs11: 一个皮肤同时再导出两个提供者
> [[export]]
> api_iface   = "runtime_adapter"                            # 必须等于 [plugin].api_type
> form        = "skin"
> name        = "pkcs11"
> reexport_of = ["service/crypto#crypto", "service/keyring#keyring"]   # **列表**: 两个提供者
> symbols     = ["C_Initialize", "C_GenerateKey", "C_Sign"]  # 可选: 占有的符号族
> ```

> **本节 `[plugin].sched_class` 与 `[[res]]` 的来历(S-2)**: 首版 §2 声称"调度类别可查""资源预算全量", 但 §8.1 没有对应输入字段(评审 `r1/02` P0-5 与 `r1/03` P0-5/P2-9/R7 独立命中)。现补齐两处输入: **`sched_class`** 是"插件对调度器的要求"(与调度器策略类别 `sched_kind` 不是一回事), **`[[res]]`** 是 per-plugin 的容量需求(求和后与产品预算比对)。**没有这两个字段, §2 的第 3/5 项就名不副实, `dep closure` 的"预算合计"也没有输入。**

> **相位归属已定**(关闭 `4-04` §2「自身相位声明机制缺失」): **自身相位**由 `[plugin].phase` 声明(被依赖者自述"我在哪一相完成", 即 §7.2 的第 ② 个完成点); `[[dep]].phase` 是**可选的断言**(依赖方要求提供方不晚于该相完成)。二者冲突(提供方自述晚于依赖方断言)⇒ 红 `BRV-DEP-0010`。`1-01` §6.1 的 `br_dep_t {name, range, phase}` 因此保留形状、语义收窄为断言(§13.2 A-10)。
>
> **`[[res]]` 与 `[privileged.resources]` 的分工**(资源预算的两类输入, §2 第 5 项): `[[res]]` 是**容量**声明(`ram`/`stack`, 可加总、与产品 `[budget]` 上限比对); `[privileged.resources]` 是**独占**声明(IRQ 线/DMA 通道/引脚/设备名, 只做冲突检测, 不加总)。二者都由 `plugin.toml` 承载(唯一真值, BRV-D4), 组合期由 `brickie check --deps` 汇总与互斥检测。
> **预算上限的真值来源(两份上限的优先级)**: `1-01` §6.4-5 的"Σ ≤ **platform 提供量**"是**物理上限**(platform 插件声明的容量), `product.toml [budget]` 是**产品预算**(产品作者给该产品划的额度)。**两条都要过, 取更严者**: ① Σ `[[res]]` > platform 容量 ⇒ 红(物理不可行, 无豁免); ② Σ `[[res]]` > `[budget]` ⇒ 红并报差值(产品预算可被显式放宽 —— 改 `[budget]` 是一次可评审的产品决策)。`--json` 里带 `{used, platform_capacity, budget}` 三个数, 便于区分"超物理"与"超预算"。
>
> **`[plugin].sched_class` 的组合期语义**: 它是 1-01 §5.3 的三值的**唯一声明处**(`COOP_ONLY` 在选 `sched-preempt` 组合下 ⇒ 组合期硬错误, §2 第 3 项与 `1-01` §6.4-3); 缺省 `SAFE_PREEMPT`, 与 `1-01` §5.3 的默认一致。**`sched_class` 与 `sched_kind` 不是一回事**: 前者是"插件对调度器的要求"(每插件声明), 后者是"调度器策略类别"(`sched-coop`/`sched-preempt`/`sched-tt` 的性质)。

#### 8.1.1 字段级修订说明(相对首版)

| 字段 | 首版 | 现版 | 理由 |
|---|---|---|---|
| `version` | `a.b.c.d` | `COMPAT_GEN.MAJOR.MINOR.REVISE` | §5.2; 段名与语义 |
| `compat.iface_hash` | 插件级单数 | **删除**; 改为 `[[export]].hash`(**按单元**) | 依 F1(`F` 与 hash 的粒度都是**接口单元**); 首版"插件级单数 vs `[[export]]` 可多个"是评审 `r1/03` P1-10 指出的双真值 |
| `requires_iface.version` | 单字符串 | **`compat_gen` + `range` 两字段** | §5.6; 精确与范围语义不同, 混写无法分别校验 |
| `[[dep]].version` | — | 改名 `[[dep]].range`, **不带 `compat_gen`** | 依 F3 结构依赖不钉代 |
| `[[dep]].kind` | `init\|runtime` | 增 **`type`** | 评审 `r1/03` P0-3③: `dev-core→vfs-core` 仅头文件类型依赖, 写 `runtime` 会把 vfs-core 拖进"形态 B"、推翻 D19/O-S7 |
| `[[export]]` | 无冻结字段 | 增 **`compat_gen` / `freeze_state`** | §5.3 |
| `[[export]].entries` | `entries = []` | 展开为 **`[[export.entries]]` 子表** | 评审 `r1/03` P0-2: 数组空表没有字段形状, `macro`/`service` 的面变化无法表达 |
| `[iface.typedefs]` | **缺失** | **新增** | 评审 `r1/01` P1-1 / `r1/03` P0-2: 规则 4 依赖此表, 首版 schema 中不存在 |
| `[plugin].phase` | `early\|core\|late\|app`, 语义为"init 相位", 被读成"只有一个完成点" | 语义**写死**为"**第 ② 个完成点**"(§7.2); `early` 明确为"无 `init` 钩子"的特例; 与 `plugin_type` 的 CORE/LATE 约束挂钩 | 评审 **S-1**(`r1/01` 被推翻#1 + `r1/02` P1-4 + `r1/03` P1-1/I1): 单值语义下按权威文档推导会**误杀 `BRV-DEP-0009`**(`sched-coop → platform`); 回灌 `1-01` §6.1/§6.2 与 `4-04` §2 |
| `[plugin].sched_class` | **缺失**(§2 第 3 项声称"可查"却无输入) | **新增**(`SAFE_PREEMPT`/`COOP_ONLY`/`TT_SAFE`, 缺省 `SAFE_PREEMPT`) | 评审 **S-2**(`r1/02` P0-5 + `r1/03` P0-5): §2 的覆盖度表自我夸大 ⇒ 补输入使第 3 项名副其实 |
| `[[res]]` | **缺失**(§2 第 5 项"ΣRAM/栈 全量"与 `dep closure` 的"预算合计"都无输入) | **新增** per-plugin `{kind = ram\|stack, used_kib}` | 评审 **S-2**/**R7**: `r1/03` P2-9/R7 指出没有 per-plugin RAM/栈字段, "预算合计"与 §6.4-5 都落空 |
| `[[export]].api_iface` | `native \| runtime_adapter`(二值) | **三值**: 加 `third_party`(域闭合) | 评审 **S-6**: `r1/03` P0-3②/I5 的"分类域不闭合" —— 三方件若纳管上游面则无分类可标 |
| `[[export]].reexport_of` | 单值字符串 | **列表**(可容纳多个提供者) | 评审 **S-6**: `iface-pkcs11` 要同时再导出 `service/crypto` 与 `service/keyring`; 与 §3.5 不变量 3/4 配套 |

### 8.2 `product.toml`(产品级选择)

```toml
schema = 1

[product]
name    = "hsm"
version = "0.1.0.0"                # COMPAT_GEN.MAJOR.MINOR.REVISE(§5.2)
app     = "app/hsm"                # 恰一个(§3.2)
core    = ">=1.0.0"
stage   = "dev"                    # dev | release —— §7.5 profile 的 manifest 默认值; CLI 覆盖

[select]
plugins = [                        # 显式选择; 闭包自动补齐依赖
  "platform/qemu-aarch64",
  "sched-coop",                    # 旧式裸名合法(§8.3 ②(a)); 等价推荐形态 = `sched/sched-coop`
  "service/crypto",                # 服务走 `service/`(= subkind), 不是 `ability/*`  (r1/03 P0-7)
]
# mount / budget / trace_id 等产品参数: v0.1 原样透传, 语义归 4-03

[budget]
ram_kib = 512
stack_kib = 16

[lint]
frozen_deps = "inherit"            # inherit(跟随 [product].stage) | allow | deny —— §7.5 的显式覆盖
allow_edges = [                    # §7.3 的框架件白名单特例
  ["framework/cdev-core", "framework/dev-core"],
  ["framework/bdev-core", "framework/dev-core"],
  ["framework/cdev-core", "framework/vfs-core"],   # 类型依赖(§7.3 正文第三例)
]
```

### 8.3 插件名与目录(与 `4-02` §3 的接口)

> **唯一口径(关闭 `r1/03` P0-7)**: 插件名 = `<namespace>/<short>`; **`namespace` 的正式枚举 = `app|iface|platform|sched|framework|io|fs|service`**(即**按 `plugin_type` + `subkind` 组织**)。**`ability` 不是 `namespace`** —— 它只保留一条通配读法(②(b))。首版的三套形态(`ability/crypto` / 裸 `crypto` / `service/crypto`)**统一到 `service/crypto`**: 它是 `plugin_type = "ability"` 的**服务**, 名字取 `subkind = service`(§0 的 `brickie new ability service/crypto` 已是此意)。

- **名字契约**: `name` 匹配 `^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$`, **全局唯一**; 推荐形态 `<namespace>/<short>`。

**① `namespace` 正式枚举(名字前缀 → 分类; 一表定死, 不再有第二种解读)**

| `namespace` | 对应 `plugin_type` | 对应 `subkind` | 样例名 |
|---|---|---|---|
| `app` | `app` | —(无 `subkind`) | `app/hsm` |
| `iface` | `interface` | —(无 `subkind`) | `iface/pkcs11`(既有裸名 `iface-pkcs11`/`iface-min`/`iface-posix` 同样合法) |
| `platform` | `platform` | —(无 `subkind`) | `platform/qemu-aarch64` |
| `sched` | `ability` | `scheduler` | `sched/sched-coop`(或裸名 `sched-coop`) |
| `framework` | `ability` | `framework` | `framework/dev-core`(或裸名 `dev-core`) |
| `io` | `ability` | `io` | `io/virtio-hsm` |
| `fs` | `ability` | `fs` | `fs/tmpfs` |
| `service` | `ability` | `service` | `service/crypto` |

**② 两条兼容读法(合法; v0.1 不强制改名)**

| 读法 | 形态 | 合法性 | v0.1 处置 |
|---|---|---|---|
| (a) **裸名(legacy)** | 无 `/` 的短名 | `svc-posix`/`sched-coop`/`iface-min`/`dev-core`/`cdev-core`/`bdev-core`/`vfs-core` 等**均合法** | 不强制改名; 只对**新增**插件给推荐形态 lint(`BRV-TAX-0013`, warning) |
| (b) **`ability/<short>` 通配读法** | `ability/` 前缀 | **不是**推荐形态: 仅当插件既非 `platform`、其 `subkind` 又**难以判断**时, 作为"无更好的 namespace"的通配读法 | 可识别; `subkind` 一旦可判, 就应换成 ① 的正式 namespace |

- **通配读法不覆盖已有正式归属**: `iface-*` 必须用 `iface/`(或裸名 `iface-min`/`iface-posix`/`iface-pkcs11`), 调度器用 `sched/`(或裸名 `sched-coop`), 框架件用 `framework/`(或裸名 `dev-core`/`cdev-core`/`bdev-core`/`vfs-core`), 服务用 `service/`; 形如 `ability/iface-*`、`ability/sched-*` 的写法**不是**通配读法的适用面。**裸名插件必须显式声明 `[plugin].subkind`**(§8.4): 带 `namespace/` 前缀时 `subkind` 可由名字推导, 裸名无从推导。

**③ 名字的 `namespace` ≠ `api_type`**

- `api_type`(`native|runtime_adapter|third_party`, §3.1)描述"**这个插件抛出的面属于哪套 API 规范**", **与名字无关**, 不构成命名维度; `service/crypto` 里的 `service` 是 `namespace`(= `subkind`), **不是** `api_type`。
- 三方件同理: `service/sqlite` 的名字走本节的 namespace 规则(它是 `subkind = service` 的服务), `api_type = "third_party"` 只写在 `plugin.toml` 的字段里(§3.1/§3.2), **不进名字**。
- 同理, `plugin_type = "ability"` 不会让名字变成 `ability/*`: 名字只回答 `subkind`(或 `app`/`interface`/`platform` 这三个 `plugin_type`), `plugin_type` 由 `[plugin].plugin_type` 声明 —— §8.1 的 `plugin_type = "ability"` + `subkind = "service"` + 名 `service/crypto` 三者并存即判例。

**④ 目录名 = 插件名(顶层即 `namespace` 目录)**

- 物理路径的**最后一段 = `short`, 顶层一段 = `namespace`**: `service/crypto` ⇒ `service/crypto/`(顶层 `service/`), 与 `1-01` §13 的 CLI 示例形态一致; 这也是 `4-02` §3「插件树组织」开放问题的关闭结论。
- 插件目录内布局按 `4-02` §2 —— 本草案只钉三件: `plugin.toml` 在插件根; 人对外的头文件在 `include/`; 生成物一律落仓库级 `build/gen/<plugin>/`。

### 8.4 骨架生成(能力 1)

`brickie new` 生成物清单(`native` × 四类 × `c`):

| 生成物 | 性质 | 说明 |
|---|---|---|
| `plugin.toml` | **人写**(仅首次生成) | 上面 §8.1 的填充版 |
| `src/<short>.c` | **人写** | `early_init`/`init`/`start` 桩 + `ops` 表占位 |
| `include/<short>/<short>.h` | **人写** | 对外面(被 `export` 引用) |
| `tests/smoke.toml` | 人写 | 用例骨架(v0.4 被消费) |
| `README.md` | 人写 | 作者说明模板 |
| `build/gen/<plugin>/plugin_desc.c` | **生成物**(不生成到插件目录) | `BR_PLUGIN(...)` 展开 + `br_plugin_t` 实例 |

- **语言模板**: v0.1 交付 `c`; `rust` 模板预留(Rust 插件能力排 v2.0, 1-03); `cxx` 待 BRV-Q8 拍板。
- **`subkind` 推导**: `ability` 的 `subkind` 可由名字 namespace 推导(`service/`→`service`, `sched/`→`scheduler`, `framework/`→`framework`, `io/`→`io`, `fs/`→`fs`; `namespace` 枚举见 §8.3 ①); 裸名(如 `sched-coop`)必须显式 `--subkind`。推导与显式声明冲突 ⇒ 红 `BRV-TAX-0015`。
- **`api_type` 模板**: v0.1 交付 `native`; `runtime_adapter` / `third_party` 的模板目录预留, 选择时报 `BRV-TAX-0014`(提示"v0.x 交付"), 但**校验**已识别这两类。
- **导出面随 `api_type` 生成**: `native` 模板预置一个 `[[export]]`(`api_iface = "native"`, `form = "api"`); `runtime_adapter` 模板预置 `api_iface = "runtime_adapter"`; **`third_party` 模板不生成任何 `[[export]]`**(§3.5 不变量 2), 生成器对此做自检。
- **不覆盖人写文件**: 已存在的 `plugin.toml`/`src/*` 一律不覆盖, 冲突 ⇒ `BRV-GEN-0002` + 退出码 1(除非 `--force`, 且 `--force` 只对**生成物**目录生效)。
- **`brickie init <product>`**: 生成 `product.toml` + `app/<name>/` 骨架 + `brickie.lock` 初版。

## 9. 实现架构与工程形态

### 9.1 分层与语言(v0.1 可落地形态)

| 层 | 语言 | 模块 | 关键产物 |
|---|---|---|---|
| L5 前端 | **Python3** | `brickie` 包: 子命令/参数/输出格式/退出码/schema 校验/文件编排 | CLI 文本 + `--json` |
| L0 领域模型 | **Rust** | `brickie-model`: `plugin.toml`/`product.toml` → 规范化模型 | 模型 JSON |
| L1 求解器 | **Rust** | `brickie-solve`: 闭包/区间/拓扑/环/相位/分类学/预算 (纯函数) | 闭包或诊断 |
| L1 版本与接口引擎 | **Rust** | `brickie-ver`: 四段版本推进; `brickie-iface`: IFACE-IR 规范化 + SHA-256 + 变更集 + 影响分析 | 变更集/新快照 |
| L2 生成器 | **C++** | `brickie-gen`: 骨架/描述符/头文件代码生成 | C 源文件 |
| 横切 | Rust + Python | 内容 hash 缓存、诊断模型、`brickie.lock` 读写 | — |

**二进制与通信**: `brickie`(Python) 通过 `subprocess` 调用 `brickie-core`(Rust, 含 model/solve/ver/iface)与 `brickie-gen`(C++), 输入/输出 = **JSON over stdio**。Python 侧**禁止**实现任何业务规则(只做编排/校验/呈现), 由 CI 的"粘合层纯度检查"保证 —— **判据 = §10 的 V-18(三条可执行证据), 此处不另立机制**。

> **发布形态(增补, 2026-10-07)**: 上表的 L5 = **Python3 语言**, 但**交付形态不必是"源码 + `python3 -m`"**。需求方要求"brickie 的 python 代码也编译为 elf"且"**brickie-gen 等工具均需要编译到 brickie elf 中**", 故 L5 的宿主产物为一个 **单文件自包含入口 ELF**: C++ 启动器 + 把 `python/brickie/**`、`templates/**` 与**原生子进程工具**(`brickie-gen`; 将来 `brickie-core` 等)作为数据嵌入的载荷, 运行时解包再由系统 `python3` 解释、执行嵌入工具(零新增第三方依赖, 不改变 L5/L2 的语言归属与"禁业务规则"纪律)。落点与语义见 ADR [`0004`](../../decisions/0004-brickie-prebuilts-bootstrap.md) §7。

> **v0.1 交付时 L0/L1 的来源(r1/03 P1-6)**: v0.1 的测试与验收前提**不含 Rust/C++ 源码构建** —— L0/L1 的 Rust 核心(`brickie-core`)与 L2 的 `brickie-gen` 可由 **`prebuilts/seed/brickie/<triple>/bin` 的预编译件**(或本机 `build/host/<triple>/bin`)提供(见 BRV-D5 的"自举种子"); **源码构建在需要它的批次(P1 模型 + 最小求解落地)才引入**。因此 V-9 的"零编译依赖"指**跑测试不需要编译器**, 不指 L0/L1 在 v0.1 不存在。
>
> **分发与协议版本(v0.1 的边界)**(r1/03 P1-8): v0.1 **只交付"宿主单文件自包含 ELF + 种子(`prebuilts/seed`)"这一种形态**(BRV-D5 / ADR [`0004`](../../decisions/0004-brickie-prebuilts-bootstrap.md) §7), **不做** wheel / manylinux / 跨发行版 ABI 承诺 —— 运行前提只有一条: **POSIX 宿主 + 系统 `python3`(≥3.11)在位**; 子进程 JSON 协议**从 v0.1 起带 `protocol_version`**, `brickie`(L5)与 `brickie-core`/`brickie-gen`(L0–L2)在**启动时互校验**, 不匹配 ⇒ **退出码 2 + 诊断**(`BRV-D9` 的"用法或环境错"档)。其余分发形态(Windows/容器/静态链接/wheel)顺延 v0.x, 风险记为 §12 **RV-14**。

#### 9.1.1 写路径纪律: 机器只写"机器拥有的文件"(关闭 `r1/03` P0-8)

**问题**: 首版让 `brickie iface publish` **回写 `plugin.toml` 的 `[compat]`**, 而 `plugin.toml` 是**人写文件**(带作者注释)。用 stdlib `tomllib` 读、自己序列化再写回 ⇒ **抹掉人写注释与排版**, 且引入"工具改人写文件"的信任问题。三条候选里必须选一条:

| 方案 | 内容 | 代价 | 采纳 |
|---|---|---|---|
| **A 只写机器文件(采纳)** | 机器**只写**它拥有的路径: `api/iface/<provider>/<unit>.toml`(单元快照)、`api/iface/CHANGELOG.md`、`brickie.lock`、`build/**`。`plugin.toml`/`product.toml` **只读** —— `publish` 的版本推进**落在单元快照里**, 不回写 TOML | 人要看"当前版本"需读快照或 `brickie ver show`(两条命令本是只读面, 无增量成本) | ✅ |
| B 注释保留型写库 | 引入能保留注释的 TOML 编辑器(如 `tomlkit` 类), 继续回写 `plugin.toml` | **新增第三方依赖**, 与 §9.3"每语言最小依赖集"冲突; 且治不了"工具改人写文件"的语义问题 | ✗ |
| C 双写 | 机器写一个**旁路文件**(如 `plugin.compat.toml`)再由人合并 | 制造**第二真值**(BRV-D4 唯一真值原则的违反) | ✗ |

**由此得到的硬规则**:
1. **`plugin.toml` / `product.toml` 是只读输入**: 任何 `brickie` 子命令都**不得**改写它们(`new`/`init` 只在文件**不存在**时创建, 见 §8.4 的"不覆盖人写文件")。
2. **版本与冻结状态的载体是单元快照**(`api/iface/<provider>/<unit>.toml` 的 `version`/`compat_gen`/`freeze_state`/`hash`)与 `brickie.lock`; `plugin.toml` 的 `[plugin].version` 与 `[[export]].version` 由**人**维护(生成器只在首次创建时给初值)。
3. **`--json` 输出把两者合并呈现**(只读): 插件自述来自 `plugin.toml`, 版本/状态/hash 来自快照 ⇒ 人不需要"某个文件同时是输入与输出"。
4. **校验**: 快照与 `plugin.toml` 的 **`version`** 差异(如人写的 `[[export]].version` 与快照不一致)只出 **info + `--check` 提示**, 不自动改写任何一侧 —— 二者语义不同(声明意图 vs 发布记录), 不属于双真值。**例外**: `[[export]].hash` 不是"意图 vs 记录"而是**同一真值的两处记录**, 不等 ⇒ **红**(§10 V-11 ④)。
5. **TOML 依赖面**: Python 侧仍只需 stdlib `tomllib`(只读); Rust 侧 `toml`(只读); **不引入写库**(方案 B 被否)。

> 这条把 `r1/03` P0-8 的"会抹掉人写注释"从根上消掉: **不是想办法把注释写得更好, 而是不再改写人写文件。**

### 9.2 仓库骨架(v0.1 交付的目录)

```
brickie/                       # 工具自身(与 brickOS 插件树同级或作为其 tools/)
├── pyproject.toml              # Python 包(brickie)与入口点
├── python/brickie/                  # L5 前端
├── rust/                       # Cargo workspace: brickie-model / brickie-solve / brickie-ver / brickie-iface / brickie-core
├── cxx/                        # CMake: libbrickie-gen + brickie-gen 可执行
├── templates/                  # 骨架模板: <api_type>/<plugin_type>/<lang>/
├── schema/                     # plugin.schema.json / product.schema.json / lock.schema.json
├── tests/                      # 求解器属性用例(含"故意造环") + 版本矩阵用例 + 幂等用例
└── docs/                       # 工具用户手册(与设计文档分工: 本文件是设计, 那里是用法)
```

> 与 `2-01` §2 第 4 项(repo 骨架)的关系: 本节只声明**工具自身**的骨架; **插件树/构建入口**的骨架仍归 `2-01`。
>
> **原生件的落点(出树, 参考 Android)**: 上表是**源码**骨架; `rust/` 与 `cxx/` 编出来的宿主可执行/静态库/对象一律**不落源码树**, 而是落 `build/host/<host-arch>/<host-os>/{bin,lib,obj}/`(见 BRV-D5)。于是"源码树里没有 `.o`/可执行文件"成为一条可门禁的纪律(实现侧对应 `make check-build` 的"宿主产物出树"检查项)。映射真值收敛到一个探测脚本(POSIX 原型为 `tools/host-detect.sh`), make、门禁、前端三处共享同一口径, 禁止各自重算。
>
> **另有一份"进库"的宿主产物 —— 自举种子**: `build/host/**` 是派生品, 而 `prebuilts/seed/brickie/<host-arch>/<host-os>/bin/<可执行>` 是**随源码提交**的预编译件, 用于(a)没有编译器的全新 checkout 直接起步, (b)将来自举编译的初始砖。它**不**改写本节的源码骨架(仍只声明源码), 只在仓库级新增 `prebuilts/` 一级目录; 发布/校验 = `make tools-prebuilt` / `make tools-prebuilt-check`。详见 BRV-D5 与 ADR [`0004`](../../decisions/0004-brickie-prebuilts-bootstrap.md)。

### 9.3 依赖纪律(v0.1 的"无第三方依赖"边界)

| 语言 | 允许 | 禁止 |
|---|---|---|
| Python3 | 标准库(`tomllib` ≥3.11)+ `jsonschema` 单一第三方 | 其余一切 |
| Rust | 官方 crates 生态中**无传递依赖或极浅**的少量 crate(`toml`/`serde`/`sha2`/`clap` 级) | 引入运行时/网络/异步栈 |
| C++ | 标准库 + 轻量模板引擎(或自研文本渲染) | 引入完整模板框架/boost 级重依赖 |

三者都要**锁版本**并进 CI 复现检查; 与 `2-02` BR-D3 的"无第三方依赖"纪律的**修订**: 三语言形态下该纪律表述为"**每语言最小依赖集 + 锁版本 + 可离线复现**"。

> **零编译依赖的边界(r1/03 P1-6)**: 上表的 Rust/C++ 依赖纪律约束的是**源码构建侧**; v0.1 的**测试与验收**不依赖 `cargo`/`cmake`/`cc` 在场 —— 跑测试只需 Python3 + 预编译的原生子进程工具(§9.1), 源码构建在需要它的批次才引入(§10 V-9)。

## 10. v0.1 验收标准(DoD)

| # | 验收项 | 判据 |
|---|---|---|
| V-1 | 骨架生成 | `brickie new` 对 `native` × {app, interface, ability, platform} × `c` 各生成一套; 生成的插件立刻通过 `brickie check`(0 错误)。**说明**: `api_type = native` 对 `app`/`interface` 只表示"代码面向 native API 编码"(§3.1 口径收窄), **不**触发任何 `api_type` 依赖禁则——`app` 的合法性由"仅经 interface"、`interface` 的合法性由"严格叶子 + 再导出一致"判定 |
| V-2 | 生成物幂等 | 重复 `brickie gen` 无任何文件 diff; `--check` 逐字节一致 |
| V-3 | 环检测 | 故意造环 ⇒ `brickie check` 报**完整环路径**(结构化边列表)且退出码 1(满足 `1-03` §3 M0) |
| V-4 | **版本引擎(「事件 → 段」)** | §5.2 推进表**逐行**有单测: (a) 解冻+改已有接口+重新冻结 ⇒ **仅 `COMPAT_GEN+1`**; (b) **新增条目 ⇒ `COMPAT_GEN` 不动**; (c) 重大产品版本 ⇒ **仅 `MAJOR+1`**; (d) 小功能 ⇒ `MINOR+1`; (e) 修 bug ⇒ `REVISE+1`; (f) **空解冻 ⇒ 四段全不动**。其中 (a)(c) 必须验证 **`COMPAT_GEN` 与 `MAJOR` 互不牵连**(这正是首版 `a≡b` 缺陷的回归测试) |
| V-5 | 接口发布 | `brickie iface publish` 产出快照+lock+CHANGELOG+影响报告; **面未变时为空操作**(不写盘、退出 0); `--check` 独立重算一致 |
| V-6 | 影响报告 | 变更 frozen 条目时, 报告列出全部直接/传递依赖者, 并标出 `compat_gen`/`range` 失配者; 缺 `--note` ⇒ 退出码 1 |
| V-7 | 依赖求解 | 单版本政策下闭包正确; **版本冲突可构造**: 依赖钉 `compat_gen=3` × 提供方 `COMPAT_GEN=4` ⇒ `BRV-VER-0001`; 同代 `range` 越界 ⇒ `BRV-VER-0002`; **预算合计**: per-plugin `[[res]]` 求和 > `product.toml [budget]` ⇒ 红 |
| V-8 | 分类学执法 | 三条硬禁则(§7.3)各有正/反用例; 相位单调违例有反用例(**且必须有 `sched-coop → platform` 两完成点不误杀的正用例**; 边界: 只覆盖已声明 init 边) |
| V-9 | 零编译依赖(**精确口径**) | **v0.1 的测试套件只覆盖 L5(Python 前端)+ L2(已发布的 `brickie-gen` 预编译件, 经 `prebuilts/seed` 或 `build/host` 提供)**; **Rust/C++ 的源码构建不在 v0.1 的测试前提里** —— CI 的"零编译依赖"指**跑测试**不需要 `cc`/`cargo`/`nm` 在场, 不指"L0/L1 Rust 核心不存在"(§9.1)。**开发态仍可用 Cargo/CMake 构建, 但验收不依赖它们在场**; `--json` schema 稳定(快照测试)。(r1/03 P1-6) |
| V-10 | 接口预留(`requires_iface`) | `requires_iface` 在 v0.1 **必须**: ① 被 schema 校验(缺 `id`/`compat_gen`/`range` 即 `BRV-MF-*` 形状类错误, 退出码 2); ② **不参与闭包求解** —— 用一个含 `requires_iface` 的 fixture 证明**闭包结果与去掉该字段时逐字节相同**(`brickie dep closure --json` 两次输出同字节); ③ `brickie iface show`/`--json` 对其只报 info('v0.1 不扫描声明面接口依赖')。(r1/03 P1-15a) |
| V-11 | hash 语义可读 + `truth` 一致性 | ① 每份快照文件头与 `--json` 输出都带 `hash_scope`/`truth`; ② **`truth = "decl"` 与 `hash_scope = "decl"` 必须成对** —— 出现 `sym`/`header` 而 v0.1 不支持 ⇒ **红**(`BRV-IFACE-*`); ③ `brickie iface show` 输出折叠 `NOT_ABI` 提示("声明面 hash ≠ ABI 兼容证明"); ④ **交叉检查**: 同一 unit 的 `plugin.toml [[export]].hash` 与 `api/iface/<provider>/<unit>.toml` 里的 hash **必须相等**, 不等 ⇒ **红**(`BRV-IFACE-*`); 理由: **`hash` 是同一真值的两处记录**(§9.1.1 规则 4 的 **`version`** 差异只出 info 是另一件事 —— version 是"人写的发布意图", hash 是"面的指纹")。(r1/03 P1-15b) |
| V-12 | 导出分类不变量 | `api_iface ≠ api_type`、`third_party` 抛 `native`/`runtime_adapter` 面、`form="skin"` 缺 `reexport_of`(或列表中**任一项**指向单元分类不符)、`form != "skin"` 却声明 `reexport_of`/`symbols` 四种违例各有反用例, 报错码分别为 `BRV-TAX-0016/0017/0018/0019`; 且 `third_party` 插件在**无 export** 时能正常通过 `brickie check`; **正例三组**(r1/03 P1-15c: 其中 skin 正例必须同时覆盖**一个提供者 + 多个提供者 + 分类相等**三小项): ① `form="skin"` 的**三小项** —— (i) **单提供者**(`reexport_of` 列表一个元素)必须通过; (ii) **多提供者**(§8.1 的 `iface-pkcs11` 同时再导出 `service/crypto#crypto` 与 `service/keyring#keyring`)必须通过; (iii) **分类相等**(每个被再导出单元的 `api_iface` 与皮肤自身相等, 本例均 `runtime_adapter`)必须通过; ② **`app → service/sqlite`**(APP 经 `ability` 包装件或 `interface` 消费三方件)必须通过; ③ **`service/* → service/sqlite`**(`ability` 提供方对三方件声明 `runtime`/`type` 边)必须通过 —— ②③ 是 `r1/01` P0-4③ 要求的正例, 说明"三方件不得被 `native` 依赖其**能力面**"不等于"没人能用它"; 这些码(`BRV-TAX-0016/0017/0018/0019`)**由引擎打印**, 与 `--json` 的诊断形状一致; **schema 校验失败只出 `BRV-MF-*` 形状类错误**(BRV-D2 的裁定, r1/03 P1-12) |
| **V-13** | **版本串与范围格式** | `compat_gen` 缺失 ⇒ `0003`; 4 段版本串 ⇒ `0005`; **4 段 `range` ⇒ `0006`**; `range` 缺段右补 0(`">=1.0"` ≡ `">=1.0.0"`); `--set` 低于当前版本 ⇒ `0007`(单调不回退) |
| **V-14** | **profile 门禁** | **同一输入、两种 profile、两种结论**(各需快照测试): 含未冻结接口依赖的树, `brickie check --profile dev` ⇒ exit 0; `brickie check --profile release` ⇒ exit 1 + `BRV-VER-0004` |
| **V-15** | **append vs modify 判定**(§5.4) | 正例: 新增独立结构体 / 枚举**末尾**追加成员 / 填充预留槽位 ⇒ 报 `ADDED`/`EXTENDED`, **免解冻**。反例: **给已冻结结构体加字段** / 枚举**重排** / service ops 加槽 ⇒ 报 `CHANGED`, **未处于 `unfreezing` 时报红** |
| **V-16** | **解冻窗口** | `unfreeze` 无 `--note` ⇒ 红; 窗口内 `--profile release` ⇒ `BRV-IFACE-0009`; **空解冻后 `refreeze` ⇒ 四段全不动**(§5.3.4 规则 2) |
| **V-17** | hash 输入完整性 | `#define BR_MAX 16→4096` 与"已有 service ops 加槽"**必须各产生不同的 `iface_hash`**(首版的 `NONE` 是错的); 枚举重排必须被 hash 感知 |
| **V-18** | **粘合层纯度**(§9.1) | 判据**三选一可执行**(建议 (a)+(c) 常驻 CI, (b) 作反证测试): (a) `brickie-core --selftest` 在**不加载 Python 包**的前提下独立跑通全部领域用例(无 `python/` 路径、无 Python 解释器参与); (b) **桩替换反证**: 把 `brickie-core`/`brickie-gen` 替换为"返回固定 JSON 的桩", `brickie check` 的**诊断集合(码 + 位置 + 数量)不变** ⇒ 证明业务规则不在 L5; (c) **静态检查**: `python/brickie/**` 不出现业务规则模块 —— **禁止**: 求解(闭包/区间交集/拓扑/环)、版本推进(四段计算)、IFACE-IR 规范化与 hash、分类学与特权判定; **允许**: 参数解析、文件编排、schema 校验、`--json` 序列化、把引擎的码/位置映射为文本呈现。违例 ⇒ 红。(r1/03 P1-7) |
| **V-19** | **特权声明与资源预算**(§3.4 + §8.1) | 四条各有正/反用例: ① `ability` 声明 P3/P4 面, 或声明 `granularity = "page"` 的 `map`/`protect` ⇒ `BRV-PRIV-0001`(**覆盖 §3.4 正交表"`map`/`protect` 属 P4"这一条**); ② `level = "P2"` 的插件声明 `map` ⇒ 红(同码; 正交表该行最低级别 = P4); ③ per-plugin `[[res]]` 求和 > `product.toml [budget]` ⇒ 红**且报差值**(`Σram_kib`/`Σstack_kib` 与上限之差); ④ IRQ/DMA 独占冲突(两插件声明同一 `irq` 线或同一 `dma_channels`)⇒ 红**并报冲突双方**。**必须覆盖 §3.4 正交表"池生命周期(`alloc`/`free`, `granularity = pool`)属 P2"与"`map`/`protect` 属 P4"两条**。(r1/03 P1-15d) |

## 11. 开放问题(待拍板)

| # | 问题 | 结论 / 倾向 | 状态 |
|---|---|---|---|
| **BRV-Q1** | ~~`a` 段语义~~ | **已由需求方规则关闭**: 改为 `COMPAT_GEN`(仅解冻/重新冻结后更新; 新增接口不动), 依赖精确钉代、`range` 只比 3 段(§5.2/§5.6/§7.4)。首版"把 `a` 当冻结面代数但未解耦主版本"⇒ `a≡b`、四段退化(评审 P0), 现由 `COMPAT_GEN` 与 `MAJOR` 正交解掉 | ✅ **已关闭** |
| **BRV-Q2** | 接口面真值迁移(v0.1 声明 → v0.x 头文件/符号)与 `truth`/`hash_scope` 字段设计 | BRV-D7 C 方案 | 待拍 |
| **BRV-Q3** | 分类学收敛: 四类 `plugin_type` + `subkind` 是否接受为 `1-01` §6.3 八类的粗化? 八类去留? | 接受粗化, 八类降为 `subkind` + 特例清单 | ✅ **已回灌**(A-1; 见 `1-01` §6.3 表 A/B/C 与 `4-01` §2) |
| **BRV-Q4** | `third_party` 的依赖许可与消费面(原文被截断): 是否允许依赖 `runtime_adapter`? 谁可以依赖 `third_party`? | **允许**依赖 native + runtime_adapter; **不得被 native 依赖** | 待拍(与 BRV-Q13 / `r1/04` D1–D5 同片) |
| **BRV-Q5** | 特权接口级别划分(P0–P4)与 memory 粒度模型 | 采纳 §3.4 草案 | ✅ **已回灌**(A-7; 见 `3-01` §13.6 与 `4-01` §1) |
| **BRV-Q6** | APP"仅经 interface"是否作为最终口径(推翻"直调 native")? | **是**(由 `iface-min` 提供零开销合规出口) | ✅ **已回灌**(A-2; 见 `1-01` §6.3/§7.4 与 `9-01` §1) |
| **BRV-Q7** | 描述符 `ver[3]` → `ver[4]` + 单元级 hash 的跨文档修订(含 `br_plugin_t` 宏形态 static/非 static 定稿) | **已给出结论**(不再是选项): `ver[4]` 语义即 `COMPAT_GEN.MAJOR.MINOR.REVISE`; hash 按单元; 宏形态随本决策**一次定稿**(生成物按 CA-10 取 `static const` + `.br_plugins`, 见 checklist §5.6 裁定 #3) | ✅ **已闭合并回灌** `1-01` §6.1/§6.2、`4-02` §1/§2、`3-05` §1/§2(见 §13.2 A-3/A-14) |
| **BRV-Q8** | C++ 在 v0.1 的职责边界(生成器)与 `lang = "cxx"` 是否支持 | 生成器归 C++; `cxx` 模板待 `1-03` 的 Rust/C++ 插件能力排期 | 待拍 |
| **BRV-Q9** | 插件名/目录形态最终版(与 `2-02` §8 Q8 / `4-02` §3) | §8.3(顶层 namespace + 名 opaque + 对旧名宽容) | 待拍 |
| **BRV-Q10** | 可选/弱依赖(feature/裁剪变体)是否进 v0.1?(`4-04` §3) | **不进**; schema 预留 `optional = false` | 待拍 |
| **BRV-Q11** | 接口发布的"发布"是否需要远端 registry(本地快照是否够)? | v0.1 仅本地; registry 属 v2+ 生态位 | 待拍 |
| **BRV-Q12** | 自动版本推进与 `1-02` §2.2 决策记录流程的耦合强度: `--note` 是硬门还是警告? | 硬门(触及 frozen 面 / 解冻时) | 待拍 |
| **BRV-Q13** | 三方件的**运行期能力注册**(如 sqlite `br_service_publish("db", …)`)算不算"抛出接口"? | **不算**: 注册表 = 运行期机制(允许), 接口单元 = 契约治理对象 | 待拍(与 `r1/04` D3 同片) |
| **BRV-Q14** | `^`/`~` 是否保留为 `range` 语法糖? | 保留(等价形式已在 §7.4 给出); 不影响正确性 | 可延后 |
| **BRV-Q15** | `crypto`/`keyring` 的 **ops 表是否入 golden**? (`11-01` §3 开放问题) | **入 golden**(与四件框架件同级): `br-crypto.txt`/`br-keyring.txt` 进 `api/frozen/`, 走 1-02 §2.6 同一门禁; 冻结批次见 `3-01` §15 第六批(随 M5 服务契约)。**理由**: v2 换后端/加算法面要求 ops 布局稳定, 而"布局稳定"只有 golden 能强制 | ✅ **已关闭**(原 A-19; 已回灌 `9-02` §6.2 O-H7 与 `11-01` §3, HSM 样例的 release 前置解除) |
| **BRV-Q16** | `third_party`/`upstream` 族是否进 `COMPAT_GEN` 体系? | 倾向**不进**(我们无权冻结上游面 ⇒ upstream 族只有"上游版本 + 面 hash", 无代); 与 `r1/04` D1(拆轴)同片 | 待拍 |

## 12. 风险

| # | 风险 | 缓解 |
|---|---|---|
| **RV-1** | 四段版本偏离 semver 生态习惯 ⇒ 求解器/工具互操作成本 | **`COMPAT_GEN` 不进 `range`**(只精确匹配, §5.5); `^`/`~` 等价形式显式给出(§7.4); 可提供 `MAJOR.MINOR.REVISE` 三段的 semver 视图导出 |
| **RV-2** | 声明面唯一真值(C2)与接口面头文件真值(C3)是**两种真值**, 容易被混为一谈 | 明确切开"插件声明面"与"接口面", 并用 `truth`/`hash_scope` 字段显式化(§6.6) |
| **RV-3** | v0.1 的 `iface_hash` 是**声明面 hash**, 可能被误读为"ABI 兼容证明" | 快照文件头强制声明 `hash_scope="decl"`; CLI 输出带 `NOT_ABI` 提示; V-11 验收 |
| **RV-4** | 分类学改写波及 `1-01`/`4-01`/`4-02`/`4-04`/`3-05` 五处 | §13 待对齐清单一次性回灌; 保留 `subkind` 使旧信息不丢失 |
| **RV-5** | 三语言 = 三套构建/CI/锁版本, 与 `2-02` BR-D3 的"无第三方依赖"纪律冲突 | 每语言最小依赖集 + `brickie --version --deps` 打印全环境指纹; BRV-Q8 复审 |
| **RV-6** | 自动版本推进可能绕过 `1-02` §2.2 的 RFC/PR 门钩("工具替我 bump 了") | `--note` 硬门(BRV-Q12); `publish --check` 进 CI, 人工改动无法伪造 hash |
| **RV-7** | 分类学禁则过严(如 `ability ↛ interface`)可能挡住合理的域标准适配 | 禁则以 `allow_edges` 白名单显式豁免(§7.3), 豁免必须写进 `product.toml` 从而可评审 |
| **RV-8** | v0.1 无编译 ⇒ 生成物(描述符 C 代码)可能"生成了但编不过"直到 v0.3 | 生成器与 `1-01` §6.1 宏形态**同源**并要求形态先定稿(BRV-Q7); v0.1 附"生成物语法自检"(仅括号/宏配对级) |
| **RV-9** | `third_party` 不声明 `[[export]]` 时, 三方件的能力面没有接口单元, 既不进入版本治理, 也无法从接口面反查"谁用了它" | 消费关系仍可从 `[[dep]]` 反查(反向依赖索引); v0.1 在 `brickie dep` 报告中单列"三方件消费方"; 若需要治理,**可选出口是将上游面声明为 `api_iface = "third_party"` 的 `[[export]]`**(§3.5 修订 a, 分类域已闭合), 或"由 native 包装件持有接口单元"(BRV-Q13) |
| **RV-10** | 导出分类不变量的严格性可能与既有 Interface 语义冲突: `iface-posix` 之类的 runtime_adapter **皮肤**是否需要占用 `[[export]]` | 由不变量 3 的 `reexport_of` 承接; 若 `1-01` §7.3 的"再导出不转移所有权"在符号层无法表达为单元引用, 则回退为 v0.2 的符号级校验(A-11) |
| **RV-11** | **跨 `COMPAT_GEN` 比较的心智诱惑**: 使用者会自然地认为"代大 = 更新", 从而写出跨代版本序判断 | §5.5 **明令**跨代比较无意义(字典序仅限同代内); 依赖**必须**精确匹配 ⇒ 求解器结构上不可能跨代比较; `brickie ver show` 输出显式标注"代不参与比较" |
| **RV-12** | **`COMPAT_GEN` 与 `MAJOR` 可能同时变动**, 使用者难以判断"是接口破了还是产品翻代了" | 发布报告给出**分段理由**(§6.3 `reasons`); `brickie ver show` 分别显示"上次解冻事件"与"产品代际"; V-4 要求二者互不牵连的回归测试 |
| **RV-13** | **冻结排期成为 release 的硬前置**(§7.5 连带结论): 框架件/svc-posix/crypto 面若长期无冻结批次, 其 release 会被 `BRV-VER-0004` **永久阻断** | **已关闭**: `3-01` §15 已补框架件四件 / `svc-posix` / `crypto`·`keyring` 三批(M2/M3/M5), `BRV-Q15` 已定(ops 入 golden); 过渡期仍可用 `[lint] frozen_deps = "allow"` **显式**放行(可评审的例外, 而非静默) |
| **RV-14** | **分发形态未定**(wheel / manylinux / 跨发行版 ABI / 容器 / 静态链接 / Windows 全空) | v0.1 **只承诺**"POSIX 宿主 + 系统 `python3`(≥3.11)在位"这一种形态(宿主单文件自包含 ELF + `prebuilts/seed` 种子, §9.1 分发与协议版本 / BRV-D5 / ADR `0004`); **Windows/容器/静态链接顺延 v0.x**; 可验收的最小判据 = **单文件 ELF 在干净环境(无 `cc`/`cargo`/`nm`、无 `pip install`)能跑通 `brickie new`**(与 V-9 同环境); 协议侧由 `protocol_version` 握手兜底(不匹配 ⇒ 退出码 2)。(r1/03 P1-8) |

## 13. 与既有文档的接口与待对齐修订清单

### 13.1 接口

| 对手方 | 接口 |
|---|---|
| `2-02` | v0.1 = L0/L1/L2/L5 纵切; 把 BR-D1/BR-D2/BR-D5/BR-D7 的"倾向"在 v0.1 范围内升为"已定"; 命令面在 §5/§7.7/§6.5 细化。**三语言分工(本篇 BRV-D3 / §9.1)是对 `2-02` 的 BR-D3 的修订 —— 即新增 BR-D3'「三语言 + 子进程 JSON 契约」**(`r1/02` P1-6) |
| `2-01` | 承接 §2 大纲第 1/2/3/5 项中属 v0.1 的部分; 本篇是 `2-01` 第 1 项(manifest 格式定稿)的**工具侧** |
| `4-03` | 本篇给出 `plugin.toml`/`product.toml` 的 v0.1 schema 草案与真值裁定; 语义权威仍在 `4-03`, 拍板后 `4-03` §2/§3 收缩为指针 |
| `4-04` | 本篇给出**带 `compat_gen` 精确匹配的 3 段区间语义**、单版本政策、相位单调规则、分类学禁则; `4-04` §2/§3 据此收敛 |
| `4-02` | 本篇只钉三件(§8.3), 其余仍归 `4-02` |
| `3-05` | 运行期**单向**消费 v0.1 的生成物(init 顺序表/描述符段); 运行期零检测不变 |
| `1-02` | 三态/状态机/门钩/golden 语料原样继承(**语义不修改** —— §2.1 本就写"新增=轻量"); 本篇**新增**解冻瞬态与两条改面路径, 并补上"声明面 hash"这一**过渡真值**及其迁移门禁 |
| `1-03` | 本篇是 §5 DoD 第 4/5 项的**工具侧交付**; M0 的"环检测验收"由 V-3 承接 |

### 13.2 待对齐修订清单(拍板后回灌)

> **⚠ 编号纪律(S-7: 跨文档撞号)**: 下表的 `A-*` 是**本篇的规范编号**(唯一真值)。备忘 [`r1/06`](comment/v0.1-review/r1/06-frozen-semantics-terminology.md) §8 用过**另一套** `A-*` 编号, 与下表**撞号**(最典型: `r1/06` 的 `A-22` = 本篇 **A-13**; `r1/06` 的 `A-24` = 本篇 **A-22**) ⇒ **下工单时只认下表编号**, 对照关系见"备忘编号"列。凡引 `r1/05`/`r1/06` 的具体条目, 一律以"文件 + 小节"定位, 不以编号定位。
>
> **回灌状态**(本表同时是台账): **✅** = 已回灌到目标文档; **◐** = 部分/只回灌了主文档侧; **⬜** = 仍待回灌。回灌动作见 checklist §4 的 W-* 工单。

| # | 修订 | 位置 | 备忘编号(`r1/05`/`r1/06`) | 状态 |
|---|---|---|---|---|
| A-1 | 八类 → `plugin_type`(四类) + `subkind`, 并新增 `api_type` 维度 | `1-01` §6.3 / `4-01` §2 / `3-05` §1 / `4-02` §1+§2 / `4-04` §1 / `README` | — | ✅(已回灌 `1-01` §6.3 表 A/B/C + `4-01` §2 + `3-05` §1 + `4-02` + `4-04` + `README`) |
| A-2 | APP "仅经 interface"(删除"直调 native"口径; 由 `iface-min` 承接) | `1-01` §6.3 / `4-01` §2 / `9-01` / `1-02` §1 / `1-03` §1 / `9-02` §3.2+§10 / `8-01` §1 | — | ✅(已回灌 `9-01`/`1-02` §1/`1-03` §1/`9-02` §3.2+§10/`8-01` §1) |
| A-3 | 描述符 `ver[3]` → `ver[4]`; **`br_plugin_t` 宏形态同批定稿**(非 static vs `static const`); **hash 的粒度 = 接口单元**(`[[export]].hash`; 插件级单数 `compat.iface_hash` 已删除, 见 §8.1.1) | `1-01` §6.1 / `3-05` §2 / `4-02` §1+§2 | `r1/02` P1-3 | ✅(`1-01` §6.1 + `3-05` §2 + `4-02` §1) |
| A-4 | 表达格式确定 TOML(关闭开放问题) | `4-03` §3 / `3-05` §3 / `2-01` §2 / `2-02` §2 C6 | — | ✅(`4-03` §3 定 TOML + `3-05` §3 + `2-02` §2 C6) |
| A-5 | 版本区间语义 = 四段 + `^`/`~` 等价区间式; 单版本政策 | `4-04` §2/§3 | — | ✅回灌 `1-01` §7.7 + `4-04` §1·§2·§3 已含全部要素 |
| A-6 | 新增相位单调规则(含两完成点口径与"只覆盖已声明 init 边"边界) | `4-04` §2(新条目) / `3-05` §2 第 4 项 | — | ✅回灌 `1-01` §6.2/§6.5 + `4-04` §1·§2 + `3-05` §1·§2 第 4/5 项(含两码 `BRV-DEP-0009/0010`) |
| A-7 | 新增特权接口(P0–P4)与 memory 粒度模型 + **`(ops × granularity × region)` 正交表**; **术语辨析**: P0–P4 = **API 子集分级**, 不是硬件特权级 | `3-01` §13.6(规范落点) / `3-04` §1 / `4-01` §1+§2 / **`1-01` §2.2 + §15**(术语辨析) | — | ✅(`3-01` §13.6 含 P0–P4 + 三轴 + 正交表指针; 本篇 §3.4 含规范正交表 + 池级示例; `3-04`/`4-01` 指针; **`1-01` §2.2 已加"硬件特权级 vs 特权接口分级"辨析, §15 行改标"特权级(硬件)") |
| A-8 | 状态目录定为 `build/`(生成物/索引), 新增 `brickie.lock`(含兼容代)与 `api/iface/**`; 与 `api/frozen/**` 的分工写明 | `2-02` §3 BR-D5 / **§8** Q5 | — | ✅(`2-02` BR-D5 进库三件 + 与 `api/frozen/**` 分工; Q5 关闭) |
| A-9 | `--json` 与 manifest schema 同源(关闭 Q7) | `2-02` §8 | — | ✅(`2-02` §8 Q7 标"已关闭"; 同源口径已写入) |
| A-10 | `br_dep_t.phase` 语义收窄为"相位断言"; 新增自身相位声明 `[plugin].phase`(两完成点) | `1-01` §6.1 + §6.2 / `4-04` §2 / `3-05` §2 第 4 项 | — | ✅(`1-01` §6.1 形状 + §6.2 两完成点/断言 + `4-04` + `3-05`) |
| A-11 | 导出面分类规则(`export.api_iface ≡ api_type`; 分类三值; `third_party` 不抛异类面; `skin` 边豁免 **+ `reexport_of` 为列表**); 改名映射 `reexports→reexport_of`、`api_syms→symbols` | `1-01` §7.3(`api_syms`/再导出) / `10-01` §2 第 1 项 / `4-03` §2 / `11-01` / `1-02` §3.2+§3.3 | — | ✅(`1-01` §7.3/§7.4 含三种皮肤分类表 + `10-01` §1/§2 + `11-01` §1 + `1-02` §3.2 + `4-03` §1) |
| **A-12** | **版本模型**: 段为 `COMPAT_GEN.MAJOR.MINOR.REVISE`; **`COMPAT_GEN` 与 `MAJOR` 解耦**(接口契约 vs 产品演进); 依赖钉精确 `compat_gen` + `range` 只比 3 段 | `1-01` §6.1 / `4-04` §2 / `1-02` §2.3 层 3 | — | ✅(`1-01` §6.1 + §7.7 + `4-04` §1·§2 + `1-02` §2.3 层 3/§2.6.6) |
| **A-13** | **新增解冻/重新冻结机制**: 单元级 `freeze_state`(含 `unfreezing` 瞬态); 两条改面路径(软: 面内 RFC+弃用, 代不变 / 硬: 解冻重冻, 代 +1); `1-02` §2.6.2 补该转移 + release 阻断 | `1-02` §2.6.2 / §2.2 阈值表 / §2.6.5 | `r1/06` §8 的 ~~A-22~~(=本篇 A-13) | ✅(已回灌 `1-02` §2.6.2/§2.2/§2.6.5 + 状态机图) |
| **A-14** | `br_dep_t` 增 `compat_gen` 字段(结构依赖不带); `ver[3]` → `ver[4]`, 语义即四段新含义 | `1-01` §6.1 / `3-05` §2 | — | ✅(`1-01` §6.1 四字段 + `3-05` §2) |
| **A-15** | 依赖 `range` 表达式统一 3 段(旧写法 `">=1.0"`/`">=1.0.0"` 归一; 4 段非法) | `4-04` §2 / **`1-01` §7.7** / `1-02` §2.3 | `r1/05` X-3 | ✅(`1-01` §7.7 + `4-04` §1·§2 + `1-02` §2.3 层 3) |
| **A-16** | **`COMPAT_GEN` 的粒度 = 接口/冻结批次单元**; 插件级取 `max(F_u)`, 且该值**仅**用于注册表/显示/结构依赖 | `3-01` §15 / `1-02` §2.6.4 / §3.2 | `r1/05` 4-F1b/8.1-3 | ✅(`3-01` §15 + `1-02` §2.6.4) |
| **A-17** | `VER` 域错误码 7 个 + `IFACE-0009`/`IFACE-0010`(见 BRV-D8 编码表) | `BRV-D8`(本篇) | — | ✅(本篇自足) |
| **A-18** | **冻结计划补齐非 core 组批次**: 框架件四件、`svc-posix` POSIX 面、`crypto`/`keyring` ops —— 以接口单元为粒度 | `3-01` §1 表 + §15 / `1-02` §2.3 层 1 / `7-01`/`7-02`/`8-01` 治理段 | `r1/05` 8.4-10 | ✅(已回灌 `3-01` §0/§1/§15 + 各域文档) |
| **A-19** | 明确 `crypto`/`keyring` **ops 表入 golden**(关闭 `11-01` §3 开放问题与 `9-02` O-H7) | `11-01` §3 / `9-02` §6.2 O-H7 / `README` | `r1/05` 8.4-11 | ✅(已回灌 `11-01`/`9-02`; `BRV-Q15` 关闭) |
| **A-20** | `frozen` 补 gloss: "**单向冻结(append-only 保护)**"; 明确"新增=轻量"与"新增免解冻"是同一规则 | `1-02` §2.1 | `r1/06` P2/S2-1 | ✅(已回灌 `1-02` §2.1) |
| **A-21** | 段名 `frozen_version` → **`compat_gen`**(避免被读成"冻结时的版本"而诱导跨代比较) | 本篇全篇 / `r1/05` §3.1 | —— | ✅(本篇自足) |
| **A-22** | `append vs modify` 的**条目级判定表**(§5.4): 明确"给已冻结结构体加字段 = **修改** ⇒ 须解冻", 与 D22"后补字段 = 布局破坏"对齐 | `1-02` §2.4 / §4.1 | `r1/06` §8 的 ~~A-24~~(=本篇 A-22) | ✅(已回灌 `1-02` §2.4/§4.1) |
| **A-23** | "解冻/重新冻结"纳入 `1-02` §2.2 的**最高门槛**行 | `1-02` §2.2 | `r1/06` S4-3 | ✅(已回灌 `1-02` §2.2) |
| **A-24** | `IFACE-IR` 增 `macro`/`var`/`enum`/`service` 的 hash 输入字段(`value`/`ops`)+ `typedef` 表 + 枚举输出序 + hash 域分隔 | `1-02` §2.6.4 / 本篇 §6.2 | `r1/06` S5-4 | ✅(已回灌 `1-02` §2.6.4) |
| **A-25** | 冻结评审清单增"**冻结前应预留扩展槽**"(D22 的推广: 预留即免将来解冻) | `1-02` **§2.4** / `3-01` §15 | `r1/06` S5-5 | ✅(已回灌两处) |
| **A-26** | **v0.1 `freeze` 只能生成"待升格提案"**的显式例外(因 `1-02` §2.6.2 要求 conformance 矩阵全绿, 而 test 排 v0.4) | `1-02` §2.6.2 / 本篇 §6.4 | — | ✅(已回灌 `1-02` §2.6.2) |
| **A-27** | **v0.1 命令面 = §8.4 + §7.7 + §6.5 + §5.3.4 的 23 条叶子命令 + 2 条全局开关**; `new <plugin_type> <name>` 取**两个位置参数**(`2-02` 的 `new <kind>` 是草图); 该面的里程碑**前移到 v0.1** | `2-02` §5 命令面(改 `add`→`dep add`; 标 `show`/`env` 被 `dep closure`/`ver show` 取代; 补 `dep *`/`ver *`/`iface *`/`gen`/`new`/`init`)/ `2-02` §7 里程碑表(`new`/`init` 前移)/ `4-02` §2 第 6 项 | — | ✅(`2-02` §5 命令面 + §7 里程碑 + `4-02` §2 第 6 项; X-8 取"两种位置都接受") |
| **A-28** | **运行期依赖"必须声明"的口径收口**(关闭 `r1/02` P0-1): 三类能力面依赖走 manifest 声明(`[[dep]]`), 运行期注册表取用**不产生依赖边**; 二者互不替代、互不豁免; `svc-posix` 与 `littlefs` 之间**没有直接符号面调用边**(走 vfs-core 契约)⇒ 无需声明该边, 但"零声明直调"不成立 | `1-01` §7.5 论证 4 / `4-04` §3 两行 | — | ✅(已回灌 `1-01` §7.5 论证 4 的四条展开 + `4-04` §3 的判据/判例/执行边界) |
| **A-29** | **三语言分工是对 `2-02` BR-D3 的修订**(关闭 `r1/02` P1-6): `2-02` 倾向"Python 3 先行", 本篇定为 **L5 Python + L0/L1 Rust + L2 C++ 以 JSON over stdio 分进程**(§9.1/BRV-D3) | `2-02` §3 BR-D3 / §8 Q3 | — | ⬜(待把 BR-D3 的"Python 先行"改写为三语言分工; 见 checklist §4 的 `W-35`) |
| **A-30** | **声明面 hash 的过渡真值独立登记**(关闭 `r1/02` P1-10): v0.1 的接口面真值 = 声明文件, 每个接口单元显式带 `truth`/`hash_scope`; 头文件真值的**目标态**与 v0.2 的**迁移门禁**(双算一致 + 条目不匹配 ⇒ 红)写明 | `1-02` §2.6.1 | — | ✅(已回灌 `1-02` §2.6.1 的四条过渡规则 + 本篇 §6.6/V-11) |

**汇总(本次 A 清单全量回灌后)**: ✅ **30**(**A-1…A-30 全部**; 含 A-17/A-21 原本自足项) / ◐ **0** / ⬜ **0**。回灌覆盖: `1-01`(§2.2/§6.1–§6.3/§6.5/§7.3/§7.4/§7.5/§7.6/§7.7/§15)、`1-02`(§1/§2.1–§2.6/§3.2)、`2-01` §2、`2-02`(BR-D3/BR-D5/§2/§5/§7/§8)、`3-01`(§0/§1/§10/§13.6/§15)、`3-04`/`3-05`/`4-01`–`4-04`/`7-01`/`7-02`/`8-01`/`9-01`/`9-02`/`10-01`/`11-01`/`README`。**验收口径见 `checklist.md` §1/§1.1**。

## 14. 演进路线(v0.1 → v0.x, 与 M 里程碑对齐)

| 版本 | 新增能力 | 对应里程碑 | 入口条件 |
|---|---|---|---|
| **v0.1** | 骨架生成 + 依赖管理与分析 + 版本管理(`COMPAT_GEN.MAJOR.MINOR.REVISE`)+ 接口发布(含解冻/重新冻结) | 原型 v1.0 工具首发(M0/M1 之间) | 本篇拍板; **BRV-Q1 已关闭**; 余 BRV-Q3/Q6/Q7 有结论 |
| v0.2 | **接口依赖扫描检查**(符号级; `truth="header"`, `hash_scope="sym"`) | M1 | 头文件形态定稿(`3-01` §13 可见性宏) |
| v0.3 | **编译**(构建编排 + 描述符/头文件/链接脚本生成物) | M1/M2 | 构建后端选型(2-02 BR-D4) |
| v0.4 | **test**(conformance 运行器, host 平台)+ **合法 `freeze` 解锁**(A-26 的例外解除) | M3 | host 平台插件(1-03 §5 第 6 项) |
| v0.5 | **run**(host-native + QEMU 后端) | M2/M3 | BR-D6 target 抽象拍板 |
| v0.6 | **兼容性检查**(golden / api-dump / abidiff / 版本矩阵)+ 框架件/`svc-posix` 面纳入冻结(**A-18 的落点**) | M3 | 构建产物符号表 + `abi_id` |

> **与需求方清单的对齐**: "编译/test/run/兼容性检查/接口依赖扫描检查"五件全部落在 v0.2–v0.6; 次序按"**声明面 → 符号面 → 构建面 → 运行面 → 门禁面**"的依赖方向排, 其中"接口依赖扫描检查"提前到 v0.2 是因为它是 v0.1 接口发布的**自然下一跳**(`hash_scope` 升级), 也是后续 golden 门禁的输入。
>
> **v0.1 的 `COMPAT_GEN` 会一直是 0**: 因 `1-02` D15 规定 M3 前不冻结任何东西, 而 `freeze` 在 v0.1 只能出"待升格提案"(§6.4 / A-26)⇒ **v0.1 期间不存在合法解冻**, `COMPAT_GEN+1` 这条路径要到 **v0.4 起才可走通**。这不影响 V-4 的验收——V-4 用**构造的 fixture**(声明面 + 人为 `unfreeze`/`refreeze` 序列)测引擎, 不依赖真实冻结历史。

## 15. 本篇"成文"的条件

1. §4 的 **BRV-D1–BRV-D11** 逐条拍板(**含 D11**; D10 已由需求方规则给定, 只需确认其三条不变量与 `skin` 豁免)—— 本节首版写 `BRV-D1–BRV-D10` 是**编号范围错误**(§4 已含 D11, 且小节顺序 D1…D6/`D11`/D7…D10 错位), 已按 checklist §2.3 的 I-1/I-2 修正;
2. §11 的 **BRV-Q2/Q3/Q5/Q6/Q7** 至少给出结论(**Q1/Q15 已关闭; Q7 已给结论待回灌**); 其余可挂"v0.x 再定";
3. §13.2 的 **A-1…A-27** 待对齐修订回灌到对应文档(否则同一契约两套口径, 正是 `comment/README.md` 记录的"元契约漂移"病根)。**S 级收敛(A-13/A-18/A-19/A-20/A-22/A-23/A-24/A-25/A-26 等)已完成回灌**; 其余 ⬜ 项由 checklist §4 的 W-27…W-34 承接; 其中 **A-18/A-19 属"阻塞 release"级, 已优先闭合**;
4. 补两张 PlantUML 图(接口发布时序 / 求解器数据流)并按 README 流程生成 `pics/` —— **本篇仍缺**(注: `1-02` 的状态机图已在本轮 S-4 修复时重渲染, 可作流程样例);
5. `2-01` §2 大纲第 1/2/3 项与 `4-03`/`4-04` 的对应小节收缩为指向本篇的指针(第 5 项保留, 因 golden 排 v0.6 —— checklist §3.2 `P2-7`)。
