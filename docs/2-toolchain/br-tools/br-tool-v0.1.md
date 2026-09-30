# br-tools v0.1 详细设计(草案)

> 章节: **2-toolchain**(旁支子目录 `br-tools/`, 暂不占用章节编号 `2-03`)。状态: **草案(讨论稿)**——能力边界与倾向已列, 结论待拍板。
> 定位: **prototype v1.0 核心交付产物之一**(与 manifest 格式、repo 骨架并列; 1-03 §5 第 4/5 项)。`br-tools` 的 **v0.1** = 原型 v1.0 的第一件工具交付。
> 来源: `2-02`(BR-D1–BR-D8 倾向 / §5 命令面 / §7 演进路线); `2-01` §2 大纲; `4-02`(布局)/`4-03`(manifest)/`4-04`(依赖); `1-01` §6(插件模型)/§13(工作流); `1-02`(三态/golden/三层门禁); `3-05`(运行期管理面); `10-01`(接口插件); 需求方给出的 v0.1 功能清单与本轮硬约束(TOML / 两维分类(+`subkind` 派生) / 三语言 / manifest 字段集 / 导出面分类 ≡ `api_type`)。
> 分工: 本篇 = **`br` 工具 v0.1 的可实现规格**(命令面 / TOML schema / 版本模型 / 接口发布机制 / 依赖分析规则 / 验收标准); `br` 的**全局**架构与选型 → `docs/2-toolchain/2-02-br-arch.md`; 工具链总纲 → `docs/2-toolchain/2-01-toolchain.md`; manifest 与依赖的**语义权威** → `4-03` / `4-04`(本篇给出 v0.1 的具体形态并回填其开放问题)。
> 怎么用: 逐条拍板 **§4 的 BRV-D1–BRV-D11**(倾向已给; **D6 版本模型 / D10 导出面分类 / D11 冻结语义** 已由需求方规则给定), 消化 **§11 的 BRV-Q1–BRV-Q16**; 拍板后本篇转"成文", §13.2 的待对齐修订清单(现已 **A-1…A-26**)回灌各篇。
> **本轮修订(v2)**: 版本模型改为 **`COMPAT_GEN.MAJOR.MINOR.REVISE`**(F 与 M 解耦、依赖精确钉代), 并新增**解冻/重新冻结**机制与 **append vs modify** 条目级判定 —— 依据需求方版本号规则 + 备忘 [`r1/05`](comment/v0.1-review/r1/05-version-model-frozen.md)(F1/F2/F3 已定)与 [`r1/06`](comment/v0.1-review/r1/06-frozen-semantics-terminology.md)(术语与解冻)。此修订**关闭 BRV-Q1**, 并解掉评审 `r1/01` P0-1/P0-2、`r1/03` P0-4(三者同源)。改动集中在 §5 全节、§6.1–§6.5、§7.4–§7.7、§8、§10、§12、§13.2。
> 图: 本篇暂用 ASCII 图与表格(本机无 PlantUML/Graphviz, 待图源环境就绪后按 README 流程补 `plantUML/` + `pics/`)。

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
| **api_iface** | — | 接口单元的分类标签(`native` \| `runtime_adapter`), 恒等于提供者的 `api_type`(§3.5) |
| **init 依赖 / runtime 依赖** | — | 初始化顺序依赖(禁环)/ 运行期调用依赖(允许环, 注册表晚绑定; 1-01 §6.5) |
| **JSON** | JavaScript Object Notation | 结构化数据交换格式(`br --json` 的机器可读输出) |
| **M0–M5** | — | v1.0/v1.x 内部里程碑(1-03 §3; M5 = HSM 完整样例) |
| **native / runtime_adapter / third_party** | — | 插件按 **API 遵守规范** 的三分类(§3.1) |
| **app / interface / ability / platform** | — | 插件按 **架构层级** 的四分类(§3.2) |
| **semver** | semantic versioning | 语义化版本(maj.min.pat); 本篇为四段 `COMPAT_GEN.MAJOR.MINOR.REVISE`(§5.2) |
| **subkind** | — | `ability` 的细分类(scheduler/framework/io/fs/service), 用于承接旧八类信息(§3.3) |
| **TOML** | Tom's Obvious Minimal Language | 配置文件格式(**本篇定稿: manifest 唯一表达格式**) |

> **编号约定**: `BRV-D*` = 本篇决策; `BRV-Q*` = 本篇开放问题; `RV-*` = 风险(§12); `V-*` = 验收项(§10); `4-03 §3` = 他篇来源; 不带上限的 `§x` = 本篇。

## 0. 一句话定位与 v0.1 边界

`br-tools v0.1` = **纯 host 侧、零编译依赖的"声明面闭环"工具**: 从生成一个插件骨架开始, 到把它的接口发布出去、把依赖图算清楚、把版本号机器化地推进一步为止。它**不碰编译器、不碰符号表、不碰 QEMU**。

```
插件作者视角(v0.1 闭环, 全程无编译器):

  br new ability service/crypto --api native --lang c
        │  ① 骨架生成: plugin.toml(真值) + 实现骨架 + 生成器落 build/gen/
        ▼
  br gen / br check [--profile dev|release]
        │  ② 依赖管理: init-DAG 无环 + compat_gen 精确匹配 + range(3段) + 相位一致 + 分类学禁则
        ▼
  br iface publish service/crypto
        │  ③ 接口发布: 接口面 → IFACE-IR 规范化 → hash → 变更集(append/modify)
        │     → 自动版本推进 → 影响报告
        ▼
  br.lock + api/iface/**(版本文件: 面 + status + freeze_state + 四段版本) + CHANGELOG + dependents 索引

  改/删已有 frozen 接口时, 中间多两步(§5.3):
      br iface unfreeze <unit> --note <RFC>   →  改代码  →  br iface refreeze <unit>
                                                        ⇒ COMPAT_GEN+1
  仅新增接口时: 无 unfreeze, 只有 MINOR+1
```

**v0.1 的四件能力**(需求方清单, 逐条落到 §5–§8):

| # | 能力 | 落点 |
|---|---|---|
| 1 | 插件骨架代码生成 | §8.4(`br new` / `br init` / `br gen`) |
| 2 | 插件依赖管理与分析 | §7(`br dep *` / `br check`) |
| 3 | 版本管理 `COMPAT_GEN.MAJOR.MINOR.REVISE` | §5(`br ver *` / `br iface publish` / `unfreeze`·`refreeze`) |
| 4 | 接口发布(变更标记 / 新增 / 自动版本+hash / 三态) | §6(`br iface *`) |

**v0.1 明确不做**(需求方定为 **v0.x 后续**):

| 后续能力 | 归属版本(草案, §14) | 为什么 v0.1 不做 |
|---|---|---|
| 编译 / 链接 / 生成镜像 | v0.3 | 需要构建后端与平台参数(2-02 BR-D4/BR-D6) |
| test(conformance 矩阵) | v0.4 | 需要 host 平台插件与用例表驱动(6-01 §2) |
| run(host-native / QEMU) | v0.5 | 需要镜像与 run 后端 |
| 兼容性检查(golden / abidiff / 版本矩阵) | v0.6 | 需要**构建产物符号表**为真值(1-02 §2.6.4) |
| 接口依赖扫描检查(符号级) | v0.2 | 需要头文件/符号面提取(§6.6 的 `hash_scope` 升级) |

> **边界纪律**: v0.1 的任何命令**不得**要求 `cc`/`cargo`/`nm` 在场; CI 只需 Python3 + 两个原生子进程二进制。这条纪律同时是 v0.1 的测试前提(§10 V-9)。

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
| 3 | 调度类别 + 双保险静态分析(D10) | ◐ 声明面 | `sched_class` 与调度器组合合法性可查; 源码静态分析 → v0.x |
| 4 | 接口校验(APP 声明在闭包内 / 符号族碰撞) | ◐ 模块级 | 接口**单元**级碰撞可查 + **导出面分类不变量全量执法**(§3.5); **符号级** → v0.2(1-02 D13 双层粒度) |
| 5 | 资源预算(ΣRAM/栈) + IRQ/DMA 独占冲突 | ✅ 全量 | 纯声明计算 |
| 6 | `abi_id` 一致性 | ✗ | 需要工具链指纹 ⇒ v0.6 |

**由此得到 v0.1 的产品定义**: `br check` 是一个"**声明面完备性检查器**", 它能保证"这个组合在**逻辑上**自洽", 不保证"这个组合**编得过/跑得对**"。这句话要写进 CLI 的帮助文本与 README, 避免 v0.x 用户误信。

## 3. 术语与两维分类学(第三列 `subkind` 为派生)

### 3.1 维度一: `api_type`(API 遵守规范)

| `api_type` | 定义 | **可抛出的接口分类**(§3.5) | 依赖许可(强约束) | v0.1 |
|---|---|---|---|---|
| `native` | 严格遵守 brickOS **native API** 规范, 不依赖任何 POSIX/三方基座 | 仅 `native` 接口 | **不允许**依赖 `runtime_adapter` 插件(如 `svc-posix`) | 骨架生成 + 校验 |
| `runtime_adapter` | 为 `third_party` 提供接口支持的适配基座(如 POSIX 运行时) | 仅 `runtime_adapter` 接口 | **只允许**依赖 `native`(core / 框架件 / native ability) | 校验可识别; 骨架模板预留 |
| `third_party` | 携带上游源码的三方件(如 sqlite), 移植增量 = 适配层 | **不抛出任何接口**(纯消费者, §3.5) | 可依赖 `native`(能力)与 `runtime_adapter`(基座); **不得被 `native` 依赖** | 校验可识别; 骨架模板预留 |

> `native ↛ runtime_adapter` 这条禁则的**架构动机**: 保证"极小组合"(不链任何适配基座)永远可裁剪——`1-01` §7.5 的复用经济学。它把 `4-04` §3「调用依赖是否需要声明面」的答案锁在"**必须声明**"上: 一旦允许 native 悄悄调 POSIX, 裁剪承诺就失效。**该禁则在 v0.2 起还会投影到接口粒度**: native 插件不得 `require` 分类为 `runtime_adapter` 的接口单元(§3.5/§5.3)。

### 3.2 维度二: `plugin_type`(架构层级)

| `plugin_type` | 定义 | 数量约束 | 接口访问许可 | 允许的最高特权级(§3.4) |
|---|---|---|---|---|
| `app` | 唯一业务逻辑 | **恰 1** | **仅**允许经由 `interface` 插件提供的能力(不得直调 native, 不得声明任何特权接口) | **P0** |
| `interface` | API 皮肤(严格叶子) | 0..n, 可叠加 | 仅允许再导出其提供者的面; 不拥有状态 | **P0** |
| `ability` | 核心能力扩展(scheduler 属此类) | 0..n; `subkind = scheduler` **恰 1** | native API + 按 `subkind` 授予的特权面 | **P2**(scheduler 可 P3) |
| `platform` | 平台管理(SoC 级) | 每镜像**恰 1** | native API + 全特权面 | **P4** |

> **这一列直接改写了既有结论**: `1-01` §6.3 / `4-01` §2 允许 APP"直调 native", 与 `9-01` 的"必须经 Interface"长期矛盾(comment/README P0 清单第 12 项 / review2/05 P0-3)。本草案采纳**需求方的强约束**(APP 仅经 interface)作为唯一口径, 矛盾因此有解——`iface-min` 正是"零开销直通 native"的合规出口(§13 待对齐清单 A-2)。

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

**纪律**: `subkind` 只用于"授权与检索"(特权面授予、依赖禁则的特例、CLI 分组显示), **不参与**数量约束与依赖方向的基本判定——那些只由 `plugin_type` + `api_type` 决定。这样 `1-01` §6.3 的"依赖方向"一列可以在 v0.x 收缩为对 `plugin_type × api_type` 的引用(§13 A-1)。

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

  ops         : alloc | free | map | protect | flush | invalidate
  granularity : byte | page | region | pool        # 越靠右的粒度授权越大
  regions     : heap | dma | mmio | reserved       # 对应 3-04 的三池与 region 表
```

**v0.1 的执法范围**: 只做"**声明合法性**"(如 `app` 声明 P1+ ⇒ 红)与登记; **不**做"实际是否越权调用"的检查——那属于"接口依赖扫描检查"(v0.2)与源码静态分析(1-01 §6.4-3 后半)。

```toml
[privileged]
level = "P2"
[[privileged.memory]]
granularity = "page"
ops         = ["map", "protect", "flush", "invalidate"]
regions     = ["dma"]
[[privileged.resources]]
irq          = [32]
dma_channels = [3]
pins         = []
device_names = ["hsm0"]
```

### 3.5 导出面(`export`): 分类由 `api_type` 决定

需求方要求 manifest 声明"export 接口类型", 并给定**硬规则**: 插件抛出的接口分类与其 `api_type` **同域且必须相等**; `third_party` **暂不允许抛出接口**。

| `[plugin].api_type` | 可抛出的接口分类 | 说明 |
|---|---|---|
| `native` | **仅** `native` 接口 | 严格遵守 native API 规范的面 |
| `runtime_adapter` | **仅** `runtime_adapter` 接口 | 为三方件提供支持的适配面(POSIX 等) |
| `third_party` | **无** — 不得声明任何 `[[export]]` | 纯消费者: 上游 API 不进入 brickOS 的接口契约治理 |

**因此"接口分类"不是第三个独立维度, 而是一条不变量**(C11)。一个 `[[export]]` 表由两个**正交**字段描述:

| 字段 | 取值 | 语义 |
|---|---|---|
| `api_iface` | `native` \| `runtime_adapter` | **接口分类** —— 必须等于 `[plugin].api_type` |
| `form` | `api` \| `skin` \| `service` | **导出形态**(机制), 与分类正交 |

| `form` | 语义 | 附加字段 | 对应既有机制 |
|---|---|---|---|
| `api` | 本插件**自有**的 API 面(头文件 + 符号) | — | `1-02` §2.6 golden 面 |
| `skin` | **再导出**提供者的面(不转移所有权) | `reexport_of = "<provider>#<unit>"`; 可选 `symbols = [...]`(即 `1-01` §7.3 的 `api_syms`) | `1-02` D13 `reexports` |
| `service` | 通过服务注册表发布的**能力名 + ops 契约** | `name`(注册表名) | `3-06` 注册表语义 |

**三条不变量(v0.1 全量执法)**:

| # | 不变量 | 违例 ⇒ |
|---|---|---|
| 1 | `export.api_iface == [plugin].api_type` | `BRV-TAX-0016` |
| 2 | `[plugin].api_type == "third_party"` ⇒ 闭包内不得存在该插件的任何 `[[export]]` | `BRV-TAX-0017` |
| 3 | `form == "skin"` ⇒ 必须有 `reexport_of`, 且被再导出单元的 `api_iface` 与自身相等 | `BRV-TAX-0018` |

**分类的三个消费方**(这条规则为什么有实际后果, 而不是纯声明):

1. **接口单元 id 携带分类**: 单元 `<provider>#<unit>` 的解析结果里带 `api_iface` 字段, 随快照与 `--json` 一起输出(§6.1)。
2. **依赖侧按分类判合法**: `requires_iface`(§5.3)引用一个单元时, 若无脑跨类消费则红 —— **`native` 插件不得 require 分类为 `runtime_adapter` 的接口单元**; 这正是 §3.1 的 `native ↛ runtime_adapter` 禁则在**接口粒度**上的投影(v0.2 起生效, 因为 v0.1 不扫描接口依赖)。
3. **`skin` 边必须豁免 api_type 禁则**: `iface-posix`(runtime_adapter 皮肤)要依赖 `svc-posix`(runtime_adapter 基座), 若照搬"runtime_adapter 只许依赖 native"会被误杀。因此规则精确表述为: **`form = "api"` / `service` 的依赖边受 `api_type` 禁则约束; `form = "skin"` 的再导出边豁免**, 且豁免必须由 `reexport_of` 显式声明方能成立(不可隐式)。

> **`third_party` 的后果要写清楚**: 三方件的能力面**不进入**接口发布与版本治理 —— 它既没有接口单元, 也就没有四段版本、没有 hash、没有三态、没有 dependents 报告。这是有意的记账取舍(上游 API 不受我们治理), 代价是"三方件被谁用了"在 v0.1 只能从 `[[dep]]` 看, 无法从接口面反查 —— 记为 RV-9; 若三方件需要注册表发布能力(如 sqlite 的 `db`), 归 BRV-Q13。
>
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

**倾向: TOML**, 并立三条纪律: (1) **禁止**用 TOML 的隐式特性表达语义(一律显式键); (2) `--json` 输出与 TOML 输入**同源**——同一内部模型的两个序列化, 关闭 `2-02` Q7; (3) schema 以 **JSON Schema** 单一形式描述(TOML 与 JSON 都能校验), 放 `br-tools/schema/`。

### BRV-D3 — 三语言分工与进程边界

| 备选 | 说明 | 权衡 |
|---|---|---|
| A 单语言(Python) | 最快 | 违背需求方硬约束(C9); 求解器/版本引擎长期要嵌进 CI 与 IDE |
| B 三语言 FFI 单进程 | `pyo3` + `pybind11` 单进程 | 构建链最复杂, 三套工具链绑在一个进程里 |
| **C 三语言 + 子进程 JSON 契约** | Python3 = CLI 前端与粘合; Rust = 领域模型/求解/版本/接口面; C++ = 代码生成器; 三者以 **JSON over stdio** 通信 | 多一层序列化开销(静态工具, 无感); 换来每一层都可被 CI 独立调用与单测 |

**倾向 C**。进程边界:

```
   br (Python3 包)                    ← L5 前端: 参数/输出/退出码/文件编排/schema 校验
     ├── br-core (Rust 可执行)         ← L0/L1: 模型 + 求解 + 版本引擎 + IFACE-IR/hash
     └── br-gen  (C++ 可执行)          ← L2: 骨架/描述符/头文件代码生成(模板渲染)
                    └── 二者通过 JSON over stdio 交换"模型/诊断/变更集", 不交换文件句柄
```

**C++ 为什么必须在 v0.1 就有位置**: 它的产物是 **C 源码**(描述符 `BR_PLUGIN(...)` 展开、骨架 `init/start` 桩、对外头文件), 必须与 `1-01` §6.1 的 `br_plugin_t` 宏形态、`3-01` §13 的可见性宏**同源**; 且 v0.2 的"接口依赖扫描检查"注定要接 `libclang` 类的 C/C++ 头文件解析器——v0.1 先用生成器把这条边界建起来, 避免 v0.2 时把生成器从 Python 里考古出来。

### BRV-D4 — 声明文件的两级结构

| 备选 | 说明 |
|---|---|
| A 只有产品 manifest | 插件依赖写在产品 manifest 里 ⇒ 每个产品都要复述一遍插件自述 |
| **B 两级: 插件自述 + 产品选择** | `plugin.toml`(插件级真值: 依赖/资源/导出/特权) + `product.toml`(产品级: 选谁/预算/产品参数) |

**倾向 B**(继承 `2-02` BR-D2 与 `4-03` §3「两级」倾向)。**唯一真值原则**: 插件的 `dep`/`export`/`privileged` **只许**出现在 `plugin.toml`; `product.toml` 引用插件名并只声明"选择与预算", 不得重复插件自述字段——违反 ⇒ `br check` 红(`BRV-MF-0007`)。

### BRV-D5 — 状态目录与"进不进版本库"

| 产物 | 位置 | 进版本库? | 理由 |
|---|---|---|---|
| 生成物(描述符/头文件/注册表 C 代码) | `build/gen/**` | **否**(`.gitignore` 已有 `build/`) | 生成物纪律(C6); 可随时 `br gen` 重建 |
| 反向依赖索引 / 求解缓存 | `build/index/**` | 否 | 可重建的派生物 |
| 锁定文件(闭包与版本区间解) | `br.lock`(仓库根) | **是** | 复现性: 闭包解不是派生物, 是决策 |
| 接口面快照(版本文件) | `api/iface/<provider>/<unit>.toml` | **是** | 接口发布的**记录**, 是 `COMPAT_GEN` 与 hash 推导的历史输入 |
| 变更日志 | `api/iface/CHANGELOG.md` | **是** | 衍生物但需人读与评审 |
| 决策记录 | `docs/decisions/NNNN-*.md` | 是 | `1-02` §2.2 |

**倾向: 采纳**。同时关闭 `2-02` Q5(状态目录 = `build/`, 不放 `.br/`; 需要进版本库的那两件单独放根与 `api/`)。

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
- 详见备忘 [`r1/06`](comment/v0.1-review/r1/06-frozen-semantics-terminology.md)。

### BRV-D7 — 接口面真值的 v0.1 过渡态与 v0.x 迁移

| 备选 | 说明 | 权衡 |
|---|---|---|
| A 声明文件永远是接口面真值 | v0.1–v0.x 一致, 无迁移 | 与 `1-02` §2.6.1(C3: 头文件声明真值 / 构建产物机器真值)冲突 |
| B v0.1 声明 → v0.x 头文件/golden | 与既有治理对齐 | 需要显式迁移窗口与 `hash_scope` 字段 |
| **C B + `truth` 字段显式声明** | 每个接口单元声明 `truth = "decl" \| "header"`, 让"谁是真值"成为**可读的、可校验的**元数据; v0.1 只允许 `decl`, v0.x 起允许 `header` 并强制 `br iface check` 双算一致 | 多一个字段 |

**倾向 C**(详见 §6.6)。理由: 迁移不可避免(C10), 与其让它隐性发生, 不如把它变成一个字段与一条 CI 门禁。

### BRV-D8 — 诊断模型: 编号化 + 位置化 + 可机读

**倾向: 采纳**(无备选争议)。每条诊断 = `{code, severity, target, file, span, message, hint}`; 编号 `BRV-<域>-NNNN`(域: `MF` manifest / `DEP` 依赖 / `VER` 版本 / `IFACE` 接口 / `TAX` 分类学 / `PRIV` 特权 / `GEN` 生成); `br check` 的**环报错**输出完整边路径(满足 C4 的 M0 验收), 且 `--json` 下是结构化边列表而非字符串。

**已分配编码表**(v0.1 全集; 首版 **`VER` 域一个码都没有**, 导致 V-7 无码可断 —— 见评审 `r1/03` P1-13):

| 域 | 码 | 含义 | 严重度 |
|---|---|---|---|
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
| `TAX` | `0013` | 新增插件名不符推荐 namespace 形态 | warning |
| `TAX` | `0014` | 所选 `api_type` 模板尚未交付(v0.x) | info |
| `TAX` | `0015` | `subkind` 推导与显式声明冲突 | error |
| `TAX` | `0016/0017/0018` | 导出面分类不变量 1/2/3(§3.5) | error |
| `TAX` | `0019` | `form != "skin"` 却声明 `reexport_of`/`symbols`(反向不变量, 评审 P2-2) | error |
| `PRIV` | `0001` | 声明的特权面**越出** `[privileged].level` 允许的档位 | error |
| `PRIV` | `0002` | (ops × granularity × region) 组合非法(§3.4 正交性) | error |
| `GEN` | `0002` | 生成目标已存在且非生成物目录 | error |

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
| `COMPAT_GEN` | **冻结代**(兼容代数) | **工具强制**(解冻/重冻序列) | "已承诺的接口面**被改/删过几次**?" |
| `MAJOR` | 主版本 | **人声明** | "产品能力代际是否变了?" |
| `MINOR` | 次版本 | **人声明** | "是否新增了接口或功能?" |
| `REVISE` | 修订 | **人声明** | "面未动, 只是实现/文档/性能变了?" |

**关键: `COMPAT_GEN` 与 `MAJOR` 正交**(首版让二者恒同步, 是三个 P0 的共同根因)。`COMPAT_GEN` 由**接口契约**驱动, `MAJOR` 由**产品演进**驱动 ⇒ 可同时出现 `F=3,M=1`(接口被破坏过三代但产品大版本没动)与 `F=1,M=2`(产品翻代但接口没动)。

**推进规则**(`br iface publish` 判定; `MAJOR`/`MINOR`/`REVISE` 由 `br ver bump --rule` 或 `publish --set` 声明):

| 事件 | `COMPAT_GEN` | `MAJOR` | `MINOR` | `REVISE` | 依据 |
|---|---|---|---|---|---|
| **改/删已有 `frozen` 条目**(经解冻 → 重新冻结) | **+1** | 不动 | →0 | →0 | §5.3 硬路径 |
| 改/删已有 `deprecated` 条目(移除) | **+1** | 不动 | →0 | →0 | 仍须满足弃用周期(`1-02` §2.6.5) |
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

> **⚠ "修 bug" 与 "冗余重跑" 的面**完全相同**, 靠「是否显式声明」区分**(这是本节最易误读之处):
>
> | 意图 | 命令 | 结果 |
> |---|---|---|
> | 修了 bug / 改了实现或文档(面不变) | `br ver bump <unit> --rule revise` | `REVISE+1` |
> | 无意图地重跑 publish(CI 重跑 / 手工重复) | `br iface publish <unit>` | **空操作**(不推进任何段、不写盘、退出 0) |
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
> 追加 ≠ 破坏承诺 ⇒ **无需解冻**; 改写/删除 = 破坏承诺 ⇒ **必须走解冻 → 重新冻结**。

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
| 1 | `br iface unfreeze <unit>` 需**最高门槛 RFC**(`--note <决策记录>`) | 解冻让**全部依赖方强制重新验证**, 影响面大于单次签名变更 ⇒ 纳入 `1-02` §2.2 已有的"最高门槛"行 |
| 2 | `refreeze` 时**确有"改/删已冻结条目"才 `COMPAT_GEN+1`**; 否则不 bump | 精确对齐需求方"**已有接口发生变动**"—— 空解冻不该让全体依赖方重钉 |
| 3 | **`br check --profile release` 在任何单元处于 `unfreezing` 时失败**(`BRV-IFACE-0009`) | 防止 release 停在半谈判状态(与 §7.5 的 release 门禁同轴) |

命令面(并入 §6.5):

| 命令 | 作用 |
|---|---|
| `br iface unfreeze <unit> --note <path>` | 进入解冻窗口(`freeze_state: frozen → unfreezing`) |
| `br iface refreeze <unit> [--note <path>]` | 退出窗口; **按 §5.4 判定矩阵决定是否 `COMPAT_GEN+1`** |

### 5.4 ⚠ append vs modify: 判定必须落在**条目内部结构**上

**陷阱**: "新增无需解冻"**只在条目级成立**。给一个已冻结的**结构体加字段**, 直觉上是"新增", 但在 D14 的二进制兼容模型里是**修改**:

| 出处 | 原文 |
|---|---|
| `1-01` **D22**(行 132) | "动机 = D14: ops 布局入 golden, **后补字段 = 布局破坏**——**预留即免破坏**" |
| `1-02` §4.1-2 | "内联访问器…其**布局变更按硬门禁处理**" |
| `1-02` §4.1-4 / §2.4 | "**枚举 append-only** 进 golden"; "枚举只追加、**不重排**、不当位标志跨版本扩展" |

因此 `br check` **不能只比"条目集合的差"**, 必须比**条目内部结构**, 按 kind 判定:

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

| `COMPAT_GEN` | `hash` | 结论 | 对应需求方问句 |
|---|---|---|---|
| 不变 | 不变 | 面完全未动 | — |
| 不变 | **变** | **纯追加 / 状态转移 / experimental 改动** | "**是否增加接口?**" → 是 |
| **+1** | 变 | **已有 `frozen`/`deprecated` 条目被改或删** | "**是否变更/删除了某个接口?**" → 是 |

**跨 `COMPAT_GEN` 比较无意义**(须写明): 依赖精确匹配 `COMPAT_GEN` ⇒ 求解**永不跨代比较**。`(F,M,m,r)` 字典序**只可用于同一 `F` 内**排序/展示。跨代不存在"更新/更旧": `F=4` 出现在 `F=3` 之后, 但 `F=4` 可能**删掉了** `F=3` 的接口 ⇒ 对依赖方而言是"**不兼容的另一代**"而非"升级"。迁移语义由 `deprecated` + 弃用周期承担, **不由版本序承担**。

### 5.6 版本文件(接口面快照)与兼容信息

需求方点名的"兼容信息"四件, 落点如下:

| 兼容信息 | 落点 | v0.1 |
|---|---|---|
| 版本文件 | `api/iface/<provider>/<unit>.toml`(接口面快照: 条目 + 状态 + `freeze_state` + 四段版本 + hash) | ✅ 生成 |
| 版本 hash | 快照与 `plugin.toml [compat].iface_hash`(按单元) | ✅ |
| 对 core 的版本依赖 | `plugin.toml [compat].core = ">=1.0.0"` | ✅(区间求解参与) |
| 接口依赖(对其他插件接口单元的依赖) | `[compat].requires_iface = [{id, api_iface, compat_gen, range, mode}]` | **预留字段, v0.1 不扫描**(需求方明确"预留在文档中") |

`requires_iface` 的预留语义(写进 schema, v0.2 启用):

```toml
[[compat.requires_iface]]
id         = "ability/vfs-core#file"   # <provider>#<unit>
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
接口单元(iface unit)   = <provider>#<unit>       例: ability/vfs-core#file
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
  ├── api_iface        : native | runtime_adapter   ← 恒等于提供者的 api_type(§3.5)
  ├── iface_hash       : sha256(canonical(surface))
  └── hash_scope       : "decl"(v0.1) | "sym"(v0.x)
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

`br iface publish` 对**旧快照 vs 新面**做 diff, 产出变更集, 并**必须**附上受影响者。**判定先分「追加 / 修改」**(§5.4), 再定治理后果:

| 类别 | 判定 | 是否须解冻 | 硬性要求 |
|---|---|---|---|
| `ADDED` | 新增**条目**(新增函数 / 独立结构体 / 新 service 名) | **免** | 无(绿) |
| `EXTENDED` | 已冻结条目的**合法追加**(枚举末尾加成员 / 填充预留槽位) | **免** | 无(绿); 按 §5.4 判定表校验是"追加"而非"修改" |
| `CHANGED` | 已有条目的 **sig/layout/value/ops** 变化(**含结构体加字段**) | **必须**(已 `frozen` 条目) | 必须处于 `unfreezing` 且 `--note <决策记录>` |
| `REMOVED` | 条目消失 | **必须**(已 `frozen` 条目) | 必须已 `deprecated` + 满足弃用周期(`1-02` §2.6.5) |
| `STATUS` | 三态转移 | 免 | 合法性按 `1-02` §2.6.2 状态机校验(如 `experimental→deprecated` 直跳 ⇒ 红) |
| `FREEZE` | `freeze_state` 转移(`unfreezing` 进/出) | — | 进出均需 `--note`; `refreeze` 决定是否 `COMPAT_GEN+1` |
| `NONE` | 面未变 | — | **空操作**(不写盘、退出 0), 见 §6.5 |

**组合变更集**: 一次 publish 可同时含多类(如 `ADDED` + `CHANGED`)。**判定取最严**: 存在任一 `CHANGED`/`REMOVED`(涉及已冻结条目)⇒ `COMPAT_GEN+1`; 否则只要存在 `ADDED`/`EXTENDED`/`STATUS` ⇒ `MINOR+1`; 全 `NONE` ⇒ 空操作。

**影响报告(dependents report)**——需求方要求的"标记接口变更状态, 依赖的插件":

```
$ br iface publish ability/vfs-core --check --json
{
  "unit": "ability/vfs-core#file",
  "changes": [
    {"kind":"CHANGED","entry":"br_open","from":"…","to":"…","status":"frozen"},
    {"kind":"EXTENDED","entry":"br_open_flags","note":"enum 末尾追加成员"}
  ],
  "compat_gen_changed": true,
  "version": {"from":"1.2.0.0","to":"2.0.0.0",
              "reasons":["COMPAT_GEN+1: 已有 frozen 条目被改","MINOR→0"]},
  "dependents": {
    "direct":   ["ability/fs-tmpfs","app/hello"],
    "transitive": ["app/hello"],
    "unsatisfied": [{"plugin":"ability/fs-tmpfs","requires":{"compat_gen":1,"range":">=1.0.0"},
                     "why":"compat_gen 已由 1 变 2, 需重新确认"}]
  },
  "verdict": "red"
}
```

> **`compat_gen_changed` 替代首版的 `iface_changed` 布尔**: 后者与 `COMPAT_GEN` 是否变动、`hash` 是否变动构成三重冗余(§5.5 的判定表已完整确定三者关系) ⇒ 删除, 少一个真值。

**反向依赖索引**: `build/index/dependents.json`(派生物, 由 `br dep index` 全树重建)。接口发布**只读**它, 缺失则先重建并提示——避免接口发布依赖陈旧索引。

### 6.4 三态 + 冻结瞬态在 v0.1 的落地

`1-02` §2.1/§2.6.2 的三态与状态机**原样继承**(语义**不修改** —— §2.1 行 46 本就写明"新增=轻量", 与本篇"新增免解冻"逐字一致), v0.1 实现其**声明与转移校验**部分, 并**新增**解冻瞬态(§5.3):

| 能力 | v0.1 | 说明 |
|---|---|---|
| 在条目上标注三态 | ✅ | `status = experimental\|frozen\|deprecated` |
| 状态转移合法性校验 | ✅ | 按 `1-02` §2.6.2 转换表 |
| **单元级 `freeze_state`**(含 `unfreezing` 瞬态) | ✅ | §5.3.1; **新增机制**(`1-02` 无此路径) |
| `unfreeze` / `refreeze` 门钩 | ✅ | 最高门槛 `--note`; `refreeze` 决定是否 `COMPAT_GEN+1` |
| **append vs modify 判定**(§5.4) | ✅ | 按条目 kind 判"是追加还是修改" —— 这是"新增免解冻"能否安全成立的关键 |
| 冻结面变更 ⇒ 需决策记录 | ✅ | `--note` 门钩(§6.3) |
| 弃用周期(两个 minor 无使用)计数 | ◐ | v0.1 只能统计**声明面**的使用者数量; 真实使用统计需符号级 ⇒ v0.2 |
| 编译期 `deprecated` 警告 | ✗ | 需生成头文件与编译 ⇒ v0.3 |

> **⚠ `freeze` 的前置条件缺口(须显式声明)**: `1-02` §2.6.2 规定 `EXPERIMENTAL→FROZEN` 的前置是"**当期已交付调度器的 conformance 矩阵全绿**"(层 2), 且升格产物含"**golden 收录**"。而本篇把 test/conformance 排到 **v0.4**、golden 排到 **v0.6** ⇒ **v0.1 无法完成一次合法冻结**。
> **v0.1 的处理(采纳评审 `r1/02` P0-2 的建议)**: `br iface freeze` 在 v0.1 **只能生成"待升格提案"**(写入 `build/gen/proposals/<unit>.toml` + 要求 `--note`), **不得落 `frozen` 快照、不得 bump `COMPAT_GEN`**; 真正的升格在 v0.4+(有矩阵)执行。这条是**显式例外声明**, 不是对 `1-02` §2.6.2 的静默偏离(回灌 A-26)。

### 6.5 发布命令面与幂等

| 命令 | 作用 | 副作用 |
|---|---|---|
| `br iface list [--json]` | 列出插件树内全部接口单元与状态 | 无 |
| `br iface show <id>` | 展示接口面(条目 + `status` + `freeze_state` + 四段版本 + hash) | 无 |
| `br iface diff <id> [--json]` | 旧快照 vs 当前声明的变更集(§6.3) | 无 |
| `br iface publish <id> [--check] [--note <path>] [--set <ver>]` | 计算 → diff → 影响报告 → 版本推进 → 落盘 | 写快照/lock/CHANGELOG/`plugin.toml [compat]` |
| `br iface status <id> [--check]` | 独立重算 hash 与快照比对(CI 门禁用; 防手编) | 无 |
| `br iface freeze <id>[#entry]` | `status: *→frozen`(承诺升级); 须 `--note` | 写快照 + 要求 `--note` |
| `br iface deprecate/undeprecate <id>[#entry]` | `status: frozen↔deprecated`; 须 `--note` | 同上 |
| **`br iface unfreeze <unit> --note <path>`** | **进入解冻窗口**(`freeze_state: frozen→unfreezing`); 最高门槛门钩 | 写快照 |
| **`br iface refreeze <unit> [--note <path>]`** | **退出窗口**; 按 §5.4 判定矩阵决定是否 `COMPAT_GEN+1` | 写快照 + 可能 bump `COMPAT_GEN` |

**幂等纪律**(修正首版的自相矛盾——首版写"NONE ⇒ `d+1`"却又宣称重复 publish 无 diff, 每次调用都烧掉一个版本号):

> **面内容幂等**: 若 `canonical(new surface) == snapshot.iface_hash` **且** 条目集合/`status`/`freeze_state` 全等 ⇒ `publish` 是**空操作**(不写任何文件、不推进任何段、退出 0; `--check` 逐字节一致)。

要点:
1. **`NONE` 不再 `REVISE+1`** —— 首版的 `NONE ⇒ d+1`(例"修 bug/改文档")在 v0.1 **不可观测**(无编译、无符号、无测试 ⇒ 工具看不见"实现变了但面没变")。`REVISE` 改为**人工声明**(`br ver bump --rule revise`)。
2. **diff 与 hash 一律忽略** `version` / `hash` / `status` / `freeze_state`(§6.2 规则 6) —— 否则 publish 自己写回的字段会成为下一轮的输入, 形成 `COMPAT_GEN+1` 的**无限升级环**(评审 `r1/03` P0-1)。
3. **`--set` 必须 ≥ 当前版本**(单调性校验 `BRV-VER-0007`)。
4. 重复 `--check` ⇒ 逐字节一致(继承 `docs/render-plantuml.sh` 的"按内容而非 mtime"纪律, `2-02` BR-D5)。

### 6.6 v0.1 → v0.x 的真值迁移(BRV-D7 的落地)

```
v0.1  truth="decl"    声明面 = 真值   →  iface_hash = H(声明面)
        │      迁移窗口: v0.2 引入头文件/符号提取, 两算并行, 强制一致
v0.2+ truth="header"  头文件 = 声明真值, 构建产物 = 机器真值, 声明面降级为"意图"
        →  iface_hash = H(符号面)(hash_scope="sym"); 声明面 hash 保留为 decl_hash
```

**迁移硬约束**: v0.1 的快照文件**必须**带 `hash_scope` 与 `truth` 字段; v0.2 起 `br iface check` 对 `truth="header"` 的单元执行 **双算一致**校验(decl_hash 与 sym_hash 的**条目集合差**必须为空), 不一致 ⇒ 红。这样"迁移"是一次可验收的开关, 而不是一次静默的真值偷换。

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

`1-01` §6.2 定义四相 `EARLY < CORE < LATE < APP`, 且"同阶段内按依赖拓扑序"。由此得到一条**既有文档没有明说但必须执法**的规则:

> **相位单调规则**: 对每条 init 边 `A → B`(A 依赖 B), 必须 `phase(B) ≤ phase(A)`。违例 ⇒ 组合期错误 `BRV-DEP-0009`(报"B 在 A 之后初始化, 但 A 依赖 B")。

没有这条规则, 一个无环依赖图仍可能因相位顺序而**无法按声明的顺序初始化**——拓扑排序成功但启动序列失败, 属于典型的"构建期看不出来、运行期必炸"。

### 7.3 分类学禁则(依赖方向的可执法形式)

依赖许可 = `api_type` 禁则(§3.1) ∧ `plugin_type` 方向(§3.2) ∧ `subkind` 特例(§3.3):

| 消费者 ↓ \ 提供者 → | platform | ability | interface |
|---|---|---|---|
| platform | — | ✗ | ✗ |
| ability | ✅ | ✅(单向; 框架件间按 `7-01`/`8-01` 特别清单) | ✗ |
| interface | ✅(仅 native/core 面) | ✅ | ✅(仅再导出) |
| app | ✗ | ✗(**不得直调 native**) | ✅ |

三条硬禁则(v0.1 可全量执法): (1) `app` 不得依赖 `ability`/`platform`; (2) `native` 不得依赖 `runtime_adapter`; (3) `interface` 不得被 `app` 以外的任何插件依赖(严格叶子)。

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

**版本 arity 归一化(关闭评审 `r1/03` P1-13)**: 既有文档混用 2/3/4 段(`1-01` §7.6 写 `">=1.0"`, 首版写 `">=1.0.0"` 与 `">=0.1.0.0"`)。规则: **`range` 固定 3 段, 缺段右补 0**(`">=1.0"` ≡ `">=1.0.0"`), 4 段即错。

**单版本政策(v0.1 定)**: 同一插件名在闭包内**只允许一个版本**; 多版本共存 ⇒ 组合期硬错误 `BRV-DEP-0011`(关闭 `4-04` §3「多版本共存」——v1 明令禁止)。

### 7.5 `profile` 门禁(dev / release)

**规则**(需求方口径):

| profile | 依赖**未冻结**接口(`compat_gen = 0`) | 诊断 |
|---|---|---|
| **`dev`** | **允许** | `BRV-VER-0004` 降为 **info**(仅提示"该依赖未受保护") |
| **`release`** | **禁止** | `BRV-VER-0004` **error** ⇒ 退出码 1 |

- **适用范围**: **接口消费**(`requires_iface` / `[[export]].form="skin"` 的再导出边)。**不适用**于结构依赖(`init`/`runtime`/`type`)——依 F3 结构依赖不钉 `compat_gen`, 故不参与本判定。这条边界正是 **M0–M2 能继续工作**的原因(其依赖全是结构边)。
- **为什么是 profile 开关而非强度分级**: 需求方口径是"**阶段**决定严格度", 而非"同一阶段内可调松紧" ⇒ 二者语义不同, profile 更干净。
- **CLI 与 manifest**: `br check --profile dev|release`(默认 `dev`); product manifest 可声明默认值 `[product] stage`, **CLI 覆盖 manifest**。
- **与镜像 profile 的关系(须写明, 否则会被误读为同一件事)**: 本 profile = **组合期检查严格度**(工具侧); 既有 `release 构建`(`1-01` §13 `br build --release`; `3-02` IR-15/INV-C 的 debug=panic / release=trace)= **镜像内断言行为**。二者**不同轴**, 但**建议耦合**: `br build --release` 应**强制**以 `--profile release` 完成检查, 否则会出现"release 镜像由 dev 级检查放行"的漏洞。
- **CI 落地**: dev 门禁全量跑; release 门禁**唯一多出的一条**就是 `--profile release` 的 `BRV-VER-0004`(加 §5.3.4 规则 3 的 `unfreezing` 阻断)。

> **⚠ 由此照出一条必须补的排期空白**(评审发现的连带问题): "release 不允许依赖未冻结接口"把**冻结排期**变成 release 的**硬前置**, 而现行 `3-01` §15 冻结计划**只覆盖 core native 组**(`br-sched`/`mem`/`mm`/`irq`/`svc`)。**框架件四件**(`br-devcore` 等, `3-01` §0 行 44 有文件名)、**svc-posix POSIX 面**(`br-svcposix.txt`, §0 行 45)、**crypto/keyring ops**(仍为开放问题)均**有治理声明却无冻结批次** ⇒ 任何依赖它们的 release 会被 `BRV-VER-0004` **永久阻断且无排期可解**。已在 §13.2 记为 **A-18/A-19**。

### 7.6 闭包求解(纯函数, 单测友好)

```
求解器签名(概念):
  solve(plugin_tree, product, profile) -> Closure | Diagnostics
  Closure = { plugins: [{name, version, compat_gen, api_type, plugin_type, phase, budget}],
              init_edges, runtime_edges, topo_order, totals, profile }
```

- **求解顺序**: 产品 manifest 选择集 → init+runtime 边闭包 → **`compat_gen` 精确匹配** → `range` 交集(单版本) → 拓扑 + 环检测 → 相位单调 → 分类学/特权声明 → 预算合计 → **profile 判定(§7.5)**。
- **求解器是纯函数**(无 IO), IO 全在 Python 粘合层与 Rust 的 `br-model` 加载器——满足 `2-02` §4 建议 1(可单测, 不依赖编译器与 QEMU)。`profile` 作为**入参**传入, 不影响纯度。
- **报错可解释**: 环报完整边路径; **版本冲突报"谁钉了哪一代 `compat_gen` / 提供方在哪一代"**; 缺失报"依赖名 + 哪条边引入"。

> **版本冲突从"不可构造"变为可构造**(关闭评审 `r1/03` P1-14): 首版单版本政策 + 一处 `plugin.toml` ⇒ 求解器**永远看不到第二个候选**, "冲突"实为"缺失"。现因 `compat_gen` **精确匹配**, 冲突有真实来源:
> - 依赖钉 `compat_gen = 3`, 而提供方当前在 `compat_gen = 4`(或 2)⇒ `BRV-VER-0001`
> - 同代内 `range` 越界(提供方 `1.5.0` 而依赖要求 `>=2.0.0`)⇒ `BRV-VER-0002`
>
> 两者都可能因"上游重新冻结"或"上游发新版本"而**在不改动依赖方声明的情况下**发生 —— 这正是 release 门禁要挡的东西。

### 7.7 命令面

| 命令 | 作用 |
|---|---|
| `br dep add <plugin> <dep>[@range] [--kind init\|runtime\|type] [--frozen-gen N] [--phase late]` | 写 `plugin.toml` 并重算 |
| `br dep rm <plugin> <dep>` | 同上 |
| `br dep tree [--kind init\|runtime\|type\|all] [--json]` | 依赖树 |
| `br dep graph --format dot\|mermaid\|json` | 图导出(复用 `--json` 的边模型) |
| `br dep why <a> <b>` | 最短依赖路径解释 |
| `br dep index` | 重建反向依赖索引 |
| `br dep closure [--json]` | 产品闭包 + 拓扑序 + 预算合计 |
| `br check [--deps\|--iface\|--tax\|--priv\|--all] [--profile dev\|release] [--json]` | 组合期校验(v0.1 覆盖 §2 表 + §7.5 profile) |
| `br ver show <plugin>` | 展示四段版本 + `compat_gen` 来源(哪个冻结事件) |
| `br ver bump <plugin> --rule major\|minor\|revise` | **人工声明** `MAJOR`/`MINOR`/`REVISE`(§5.2: 工具看不见"重大/小/修 bug"这三类事件) |

> `compat_gen` **不在** `br ver bump` 的可选 rule 里 —— 它只能由 `unfreeze`/`refreeze` 序列产生(§5.3), 拒绝手工任意指定; 这是"工具强制"与"人声明"的边界。

## 8. 声明面 TOML schema(v0.1 定稿草案)

### 8.1 `plugin.toml`(插件级真值)

```toml
schema = 1

[plugin]
name        = "ability/crypto"     # 全局唯一; 形态见 §8.3
plugin_type = "ability"            # app | interface | ability | platform
api_type    = "native"             # native | runtime_adapter | third_party
subkind     = "service"            # ability 细分: scheduler|framework|io|fs|service
lang        = "c"                  # c | cxx | rust   (C++: 待定, 见 BRV-Q8)
phase       = "late"               # early | core | late | app  (init 相位)
version     = "0.1.0.0"            # COMPAT_GEN.MAJOR.MINOR.REVISE(§5.2)
summary     = "密码服务: SHA-256 / HMAC-SHA256 / AES-CBC,CTR / DRBG"
license     = "WTFPL"

[compat]
core          = ">=1.0.0"                  # 对 core 的版本依赖(1-02 层 3; range 3 段)
api_rev       = 1                          # 编码面对的 native API 版本
hash_scope    = "decl"                     # decl(v0.1) | sym(v0.x)
truth         = "decl"                     # decl | header   (§6.6)
abi_id        = ""                         # v0.1 占位; v0.6 由工具链指纹填充
# iface_hash 见 [[export]].hash —— **按单元**而非插件级(§8.1.1 说明)

# ---- 接口依赖(v0.2 启用; v0.1 只校验 schema)----
[[compat.requires_iface]]
id         = "ability/vfs-core#file"   # <provider>#<unit>
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
phase   = "early"                  # 相位断言(可省; 缺省 = 本插件自身相位)

[[dep]]
name    = "ability/vfs-core"       # runtime 依赖 ⇒ 调用边(允许环)
range   = ">=1.0.0"
kind    = "runtime"
symbol  = "br_open"                # 可选: 说明调用面(便于 v0.2 符号级核对)

# [[dep]]                          # type 依赖 ⇒ 仅头文件/类型可见(免于"形态 B"误判)
# name  = "ability/vfs-core"
# kind  = "type"

# ---- 导出面(每单元一条; 单元 = 冻结与版本的基本粒度, 依 F1)----
[[export]]                         # §3.5: api_iface 必须等于 [plugin].api_type
api_iface    = "native"            # native | runtime_adapter(= api_type)
form         = "service"           # api | skin | service
name         = "crypto"            # 单元名 / 注册表名(form=service)
version      = "0.1.0.0"           # 单元自身四段版本(§5.2 按单元推进)
compat_gen   = 0                   # 单元级冻结代(§5.3); 0 = 尚未冻结
freeze_state = "unfrozen"          # unfrozen | frozen | unfreezing(§5.3.1)
hash         = "sha256:…"          # 单元面 hash(§6.2)
status       = "experimental"      # 单元级默认状态(条目可各自覆盖)
# entries: 符号级"意图"清单(§6.1); 展开为子表
# [[export.entries]]
# kind = "func"; name = "br_crypto_hash"; sig = "int (const uint8_t*, size_t, uint8_t*)"
# status = "experimental"
# reexport_of = "…"                # 仅 form="skin": 指明被再导出的单元(§3.5 不变量 3)
# symbols     = ["…"]              # 仅 form="skin": 占有的符号族(1-01 §7.3 api_syms)

[privileged]                       # §3.4(v0.1 只校验声明合法性)
level = "P2"
[[privileged.memory]]
granularity = "page"
ops         = ["map", "protect", "flush", "invalidate"]
regions     = ["dma"]
[[privileged.resources]]
irq = [32]
dma_channels = [3]
device_names = ["hsm0"]

[build]                            # v0.1 只记录; v0.3 起被消费
sources  = ["src/*.c"]
includes = ["include"]
```

> **相位归属已定**(关闭 `4-04` §2「自身相位声明机制缺失」): **自身相位**由 `[plugin].phase` 声明(被依赖者自述"我在哪一相完成"); `[[dep]].phase` 是**可选的断言**(依赖方要求提供方不晚于该相完成)。二者冲突(提供方自述晚于依赖方断言)⇒ 红 `BRV-DEP-0010`。`1-01` §6.1 的 `br_dep_t {name, range, phase}` 因此保留形状、语义收窄为断言(§13.2 A-10)。

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
  "sched-coop",                    # 旧式裸名合法(§8.3)
  "service/crypto",
]
# mount / budget / trace_id 等产品参数: v0.1 原样透传, 语义归 4-03

[budget]
ram_kib = 512
stack_kib = 16

[lint]
frozen_deps = "inherit"            # inherit(跟随 [product].stage) | allow | deny —— §7.5 的显式覆盖
allow_edges = [                    # §7.3 的框架件白名单特例
  ["ability/cdev-core", "ability/dev-core"],
  ["ability/bdev-core", "ability/dev-core"],
  ["ability/cdev-core", "ability/vfs-core"],   # 类型依赖(§7.3 正文第三例)
]
```

### 8.3 插件名与目录(与 `4-02` §3 的接口)

- **名字契约**: `name` 匹配 `^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$`, **全局唯一**; 推荐形态 `<namespace>/<short>`(`namespace` ∈ `app|iface|platform|sched|framework|io|fs|service`)。
- **兼容既有命名**: 现有文档里的 `svc-posix`/`sched-coop`/`iface-min`/`dev-core` 均**合法**(不强制改名); v0.1 只对**新增**插件给出推荐形态 lint(`BRV-TAX-0013`, warning)。这样避免一次波及全部文档的大改名。
- **物理目录**: 顶层即 `namespace`(与 `1-01` §13 的 CLI 示例形态一致), 插件目录内布局按 `4-02` §2(本草案只钉三件: `plugin.toml` 在插件根; 人对外的头文件在 `include/`; 生成物一律落仓库级 `build/gen/<plugin>/`)。

### 8.4 骨架生成(能力 1)

`br new` 生成物清单(`native` × 四类 × `c`):

| 生成物 | 性质 | 说明 |
|---|---|---|
| `plugin.toml` | **人写**(仅首次生成) | 上面 §8.1 的填充版 |
| `src/<short>.c` | **人写** | `early_init`/`init`/`start` 桩 + `ops` 表占位 |
| `include/<short>/<short>.h` | **人写** | 对外面(被 `export` 引用) |
| `tests/smoke.toml` | 人写 | 用例骨架(v0.4 被消费) |
| `README.md` | 人写 | 作者说明模板 |
| `build/gen/<plugin>/plugin_desc.c` | **生成物**(不生成到插件目录) | `BR_PLUGIN(...)` 展开 + `br_plugin_t` 实例 |

- **语言模板**: v0.1 交付 `c`; `rust` 模板预留(Rust 插件能力排 v2.0, 1-03); `cxx` 待 BRV-Q8 拍板。
- **`subkind` 推导**: `ability` 的 `subkind` 可由名字 namespace 推导(`service/`→`service`, `sched/`→`scheduler`, `framework/`→`framework`, `io/`→`io`, `fs/`→`fs`); 裸名(如 `sched-coop`)必须显式 `--subkind`。推导与显式声明冲突 ⇒ 红 `BRV-TAX-0015`。
- **`api_type` 模板**: v0.1 交付 `native`; `runtime_adapter` / `third_party` 的模板目录预留, 选择时报 `BRV-TAX-0014`(提示"v0.x 交付"), 但**校验**已识别这两类。
- **导出面随 `api_type` 生成**: `native` 模板预置一个 `[[export]]`(`api_iface = "native"`, `form = "api"`); `runtime_adapter` 模板预置 `api_iface = "runtime_adapter"`; **`third_party` 模板不生成任何 `[[export]]`**(§3.5 不变量 2), 生成器对此做自检。
- **不覆盖人写文件**: 已存在的 `plugin.toml`/`src/*` 一律不覆盖, 冲突 ⇒ `BRV-GEN-0002` + 退出码 1(除非 `--force`, 且 `--force` 只对**生成物**目录生效)。
- **`br init <product>`**: 生成 `product.toml` + `app/<name>/` 骨架 + `br.lock` 初版。

## 9. 实现架构与工程形态

### 9.1 分层与语言(v0.1 可落地形态)

| 层 | 语言 | 模块 | 关键产物 |
|---|---|---|---|
| L5 前端 | **Python3** | `br` 包: 子命令/参数/输出格式/退出码/schema 校验/文件编排 | CLI 文本 + `--json` |
| L0 领域模型 | **Rust** | `br-model`: `plugin.toml`/`product.toml` → 规范化模型 | 模型 JSON |
| L1 求解器 | **Rust** | `br-solve`: 闭包/区间/拓扑/环/相位/分类学/预算 (纯函数) | 闭包或诊断 |
| L1 版本与接口引擎 | **Rust** | `br-ver`: 四段版本推进; `br-iface`: IFACE-IR 规范化 + SHA-256 + 变更集 + 影响分析 | 变更集/新快照 |
| L2 生成器 | **C++** | `br-gen`: 骨架/描述符/头文件代码生成 | C 源文件 |
| 横切 | Rust + Python | 内容 hash 缓存、诊断模型、`br.lock` 读写 | — |

**二进制与通信**: `br`(Python) 通过 `subprocess` 调用 `br-core`(Rust, 含 model/solve/ver/iface)与 `br-gen`(C++), 输入/输出 = **JSON over stdio**。Python 侧**禁止**实现任何业务规则(只做编排/校验/呈现), 由 CI 的"粘合层纯度检查"保证(`br-core --selftest` 能独立跑通全部用例)。

### 9.2 仓库骨架(v0.1 交付的目录)

```
br-tools/                       # 工具自身(与 brickOS 插件树同级或作为其 tools/)
├── pyproject.toml              # Python 包(br)与入口点
├── python/br/                  # L5 前端
├── rust/                       # Cargo workspace: br-model / br-solve / br-ver / br-iface / br-core
├── cxx/                        # CMake: libbrgen + br-gen 可执行
├── templates/                  # 骨架模板: <api_type>/<plugin_type>/<lang>/
├── schema/                     # plugin.schema.json / product.schema.json / lock.schema.json
├── tests/                      # 求解器属性用例(含"故意造环") + 版本矩阵用例 + 幂等用例
└── docs/                       # 工具用户手册(与设计文档分工: 本文件是设计, 那里是用法)
```

> 与 `2-01` §2 第 4 项(repo 骨架)的关系: 本节只声明**工具自身**的骨架; **插件树/构建入口**的骨架仍归 `2-01`。

### 9.3 依赖纪律(v0.1 的"无第三方依赖"边界)

| 语言 | 允许 | 禁止 |
|---|---|---|
| Python3 | 标准库(`tomllib` ≥3.11)+ `jsonschema` 单一第三方 | 其余一切 |
| Rust | 官方 crates 生态中**无传递依赖或极浅**的少量 crate(`toml`/`serde`/`sha2`/`clap` 级) | 引入运行时/网络/异步栈 |
| C++ | 标准库 + 轻量模板引擎(或自研文本渲染) | 引入完整模板框架/boost 级重依赖 |

三者都要**锁版本**并进 CI 复现检查; 与 `2-02` BR-D3 的"无第三方依赖"纪律的**修订**: 三语言形态下该纪律表述为"**每语言最小依赖集 + 锁版本 + 可离线复现**"。

## 10. v0.1 验收标准(DoD)

| # | 验收项 | 判据 |
|---|---|---|
| V-1 | 骨架生成 | `br new` 对 `native` × {app, interface, ability, platform} × `c` 各生成一套; 生成的插件立刻通过 `br check`(0 错误) |
| V-2 | 生成物幂等 | 重复 `br gen` 无任何文件 diff; `--check` 逐字节一致 |
| V-3 | 环检测 | 故意造环 ⇒ `br check` 报**完整环路径**(结构化边列表)且退出码 1(满足 `1-03` §3 M0) |
| V-4 | **版本引擎(「事件 → 段」)** | §5.2 推进表**逐行**有单测: (a) 解冻+改已有接口+重新冻结 ⇒ **仅 `COMPAT_GEN+1`**; (b) **新增条目 ⇒ `COMPAT_GEN` 不动**; (c) 重大产品版本 ⇒ **仅 `MAJOR+1`**; (d) 小功能 ⇒ `MINOR+1`; (e) 修 bug ⇒ `REVISE+1`; (f) **空解冻 ⇒ 四段全不动**。其中 (a)(c) 必须验证 **`COMPAT_GEN` 与 `MAJOR` 互不牵连**(这正是首版 `a≡b` 缺陷的回归测试) |
| V-5 | 接口发布 | `br iface publish` 产出快照+lock+CHANGELOG+影响报告; **面未变时为空操作**(不写盘、退出 0); `--check` 独立重算一致 |
| V-6 | 影响报告 | 变更 frozen 条目时, 报告列出全部直接/传递依赖者, 并标出 `compat_gen`/`range` 失配者; 缺 `--note` ⇒ 退出码 1 |
| V-7 | 依赖求解 | 单版本政策下闭包正确; **版本冲突可构造**: 依赖钉 `compat_gen=3` × 提供方 `COMPAT_GEN=4` ⇒ `BRV-VER-0001`; 同代 `range` 越界 ⇒ `BRV-VER-0002` |
| V-8 | 分类学执法 | 三条硬禁则(§7.3)各有正/反用例; 相位单调违例有反用例 |
| V-9 | 零编译依赖 | 在**未安装** cc/cargo/nm 的环境跑完整测试套件全绿; `--json` schema 稳定(快照测试) |
| V-10 | 接口预留 | `requires_iface` 字段能被解析、校验 schema、并在文档中标注"v0.1 不扫描" |
| V-11 | hash 语义可读 | 每份快照文件头与 `--json` 输出都带 `hash_scope`/`truth`; `br iface show` 明示"声明面 hash ≠ ABI 兼容证明" |
| V-12 | 导出分类不变量 | `api_iface ≠ api_type`、`third_party` 声明 `[[export]]`、`form="skin"` 缺 `reexport_of`(或指向单元分类不符)三种违例各有反用例, 报错码分别为 `BRV-TAX-0016/0017/0018`; 且 `third_party` 插件在**无 export** 时能正常通过 `br check` |
| **V-13** | **版本串与范围格式** | `compat_gen` 缺失 ⇒ `0003`; 4 段版本串 ⇒ `0005`; **4 段 `range` ⇒ `0006`**; `range` 缺段右补 0(`">=1.0"` ≡ `">=1.0.0"`); `--set` 低于当前版本 ⇒ `0007`(单调不回退) |
| **V-14** | **profile 门禁** | **同一输入、两种 profile、两种结论**(各需快照测试): 含未冻结接口依赖的树, `br check --profile dev` ⇒ exit 0; `br check --profile release` ⇒ exit 1 + `BRV-VER-0004` |
| **V-15** | **append vs modify 判定**(§5.4) | 正例: 新增独立结构体 / 枚举**末尾**追加成员 / 填充预留槽位 ⇒ 报 `ADDED`/`EXTENDED`, **免解冻**。反例: **给已冻结结构体加字段** / 枚举**重排** / service ops 加槽 ⇒ 报 `CHANGED`, **未处于 `unfreezing` 时报红** |
| **V-16** | **解冻窗口** | `unfreeze` 无 `--note` ⇒ 红; 窗口内 `--profile release` ⇒ `BRV-IFACE-0009`; **空解冻后 `refreeze` ⇒ 四段全不动**(§5.3.4 规则 2) |
| **V-17** | hash 输入完整性 | `#define BR_MAX 16→4096` 与"已有 service ops 加槽"**必须各产生不同的 `iface_hash`**(首版的 `NONE` 是错的); 枚举重排必须被 hash 感知 |

## 11. 开放问题(待拍板)

| # | 问题 | 结论 / 倾向 | 状态 |
|---|---|---|---|
| **BRV-Q1** | ~~`a` 段语义~~ | **已由需求方规则关闭**: 改为 `COMPAT_GEN`(仅解冻/重新冻结后更新; 新增接口不动), 依赖精确钉代、`range` 只比 3 段(§5.2/§5.6/§7.4)。首版"把 `a` 当冻结面代数但未解耦主版本"⇒ `a≡b`、四段退化(评审 P0), 现由 `COMPAT_GEN` 与 `MAJOR` 正交解掉 | ✅ **已关闭** |
| **BRV-Q2** | 接口面真值迁移(v0.1 声明 → v0.x 头文件/符号)与 `truth`/`hash_scope` 字段设计 | BRV-D7 C 方案 | 待拍 |
| **BRV-Q3** | 分类学收敛: 四类 `plugin_type` + `subkind` 是否接受为 `1-01` §6.3 八类的粗化? 八类去留? | 接受粗化, 八类降为 `subkind` + 特例清单 | 待拍 |
| **BRV-Q4** | `third_party` 的依赖许可与消费面(原文被截断): 是否允许依赖 `runtime_adapter`? 谁可以依赖 `third_party`? | **允许**依赖 native + runtime_adapter; **不得被 native 依赖** | 待拍(与 BRV-Q13 / `r1/04` D1–D5 同片) |
| **BRV-Q5** | 特权接口级别划分(P0–P4)与 memory 粒度模型 | 采纳 §3.4 草案 | 待拍 |
| **BRV-Q6** | APP"仅经 interface"是否作为最终口径(推翻"直调 native")? | **是**(由 `iface-min` 提供零开销合规出口) | 待拍 |
| **BRV-Q7** | 描述符 `ver[3]` → `ver[4]` + 单元级 hash 的跨文档修订 | 改为 `ver[4]`, 语义即 `COMPAT_GEN.MAJOR.MINOR.REVISE`; hash 按单元 | 待拍(与 A-3/A-14 合并) |
| **BRV-Q8** | C++ 在 v0.1 的职责边界(生成器)与 `lang = "cxx"` 是否支持 | 生成器归 C++; `cxx` 模板待 `1-03` 的 Rust/C++ 插件能力排期 | 待拍 |
| **BRV-Q9** | 插件名/目录形态最终版(与 `2-02` §8 Q8 / `4-02` §3) | §8.3(顶层 namespace + 名 opaque + 对旧名宽容) | 待拍 |
| **BRV-Q10** | 可选/弱依赖(feature/裁剪变体)是否进 v0.1?(`4-04` §3) | **不进**; schema 预留 `optional = false` | 待拍 |
| **BRV-Q11** | 接口发布的"发布"是否需要远端 registry(本地快照是否够)? | v0.1 仅本地; registry 属 v2+ 生态位 | 待拍 |
| **BRV-Q12** | 自动版本推进与 `1-02` §2.2 决策记录流程的耦合强度: `--note` 是硬门还是警告? | 硬门(触及 frozen 面 / 解冻时) | 待拍 |
| **BRV-Q13** | 三方件的**运行期能力注册**(如 sqlite `br_service_publish("db", …)`)算不算"抛出接口"? | **不算**: 注册表 = 运行期机制(允许), 接口单元 = 契约治理对象 | 待拍(与 `r1/04` D3 同片) |
| **BRV-Q14** | `^`/`~` 是否保留为 `range` 语法糖? | 保留(等价形式已在 §7.4 给出); 不影响正确性 | 可延后 |
| **BRV-Q15** | `crypto`/`keyring` 的 **ops 表是否入 golden**? (`11-01` §3 开放问题) | 倾向**入**(v2 换后端要求布局稳定) | **待拍 —— 它决定 HSM 样例能否 `release`**(§7.5 连带结论 A-19) |
| **BRV-Q16** | `third_party`/`upstream` 族是否进 `COMPAT_GEN` 体系? | 倾向**不进**(我们无权冻结上游面 ⇒ upstream 族只有"上游版本 + 面 hash", 无代); 与 `r1/04` D1(拆轴)同片 | 待拍 |

## 12. 风险

| # | 风险 | 缓解 |
|---|---|---|
| **RV-1** | 四段版本偏离 semver 生态习惯 ⇒ 求解器/工具互操作成本 | **`COMPAT_GEN` 不进 `range`**(只精确匹配, §5.5); `^`/`~` 等价形式显式给出(§7.4); 可提供 `MAJOR.MINOR.REVISE` 三段的 semver 视图导出 |
| **RV-2** | 声明面唯一真值(C2)与接口面头文件真值(C3)是**两种真值**, 容易被混为一谈 | 明确切开"插件声明面"与"接口面", 并用 `truth`/`hash_scope` 字段显式化(§6.6) |
| **RV-3** | v0.1 的 `iface_hash` 是**声明面 hash**, 可能被误读为"ABI 兼容证明" | 快照文件头强制声明 `hash_scope="decl"`; CLI 输出带 `NOT_ABI` 提示; V-11 验收 |
| **RV-4** | 分类学改写波及 `1-01`/`4-01`/`4-02`/`4-04`/`3-05` 五处 | §13 待对齐清单一次性回灌; 保留 `subkind` 使旧信息不丢失 |
| **RV-5** | 三语言 = 三套构建/CI/锁版本, 与 `2-02` BR-D3 的"无第三方依赖"纪律冲突 | 每语言最小依赖集 + `br --version --deps` 打印全环境指纹; BRV-Q8 复审 |
| **RV-6** | 自动版本推进可能绕过 `1-02` §2.2 的 RFC/PR 门钩("工具替我 bump 了") | `--note` 硬门(BRV-Q12); `publish --check` 进 CI, 人工改动无法伪造 hash |
| **RV-7** | 分类学禁则过严(如 `ability ↛ interface`)可能挡住合理的域标准适配 | 禁则以 `allow_edges` 白名单显式豁免(§7.3), 豁免必须写进 `product.toml` 从而可评审 |
| **RV-8** | v0.1 无编译 ⇒ 生成物(描述符 C 代码)可能"生成了但编不过"直到 v0.3 | 生成器与 `1-01` §6.1 宏形态**同源**并要求形态先定稿(BRV-Q7); v0.1 附"生成物语法自检"(仅括号/宏配对级) |
| **RV-9** | `third_party` 不抛接口(§3.5)⇒ 三方件的能力面没有接口单元, 既不入版本治理, 也无法从接口面反查"谁用了它" | 消费关系仍可从 `[[dep]]` 反查(反向依赖索引); v0.1 在 `br dep` 报告中单列"三方件消费方"; 若未来需要治理, 出口是"由 native 包装件持有接口单元"(BRV-Q13) |
| **RV-10** | 导出分类不变量的严格性可能与既有 Interface 语义冲突: `iface-posix` 之类的 runtime_adapter **皮肤**是否需要占用 `[[export]]` | 由不变量 3 的 `reexport_of` 承接; 若 `1-01` §7.3 的"再导出不转移所有权"在符号层无法表达为单元引用, 则回退为 v0.2 的符号级校验(A-11) |
| **RV-11** | **跨 `COMPAT_GEN` 比较的心智诱惑**: 使用者会自然地认为"代大 = 更新", 从而写出跨代版本序判断 | §5.5 **明令**跨代比较无意义(字典序仅限同代内); 依赖**必须**精确匹配 ⇒ 求解器结构上不可能跨代比较; `br ver show` 输出显式标注"代不参与比较" |
| **RV-12** | **`COMPAT_GEN` 与 `MAJOR` 可能同时变动**, 使用者难以判断"是接口破了还是产品翻代了" | 发布报告给出**分段理由**(§6.3 `reasons`); `br ver show` 分别显示"上次解冻事件"与"产品代际"; V-4 要求二者互不牵连的回归测试 |
| **RV-13** | **冻结排期成为 release 的硬前置**(§7.5 连带结论): 框架件/svc-posix/crypto 面若长期无冻结批次, 其 release 会被 `BRV-VER-0004` **永久阻断** | A-18/A-19 补齐排期; 过渡期可用 `[lint] frozen_deps = "allow"` **显式**放行(可评审的例外, 而非静默); `BRV-Q15` 需先定 |

## 13. 与既有文档的接口与待对齐修订清单

### 13.1 接口

| 对手方 | 接口 |
|---|---|
| `2-02` | v0.1 = L0/L1/L2/L5 纵切; 把 BR-D1/BR-D2/BR-D5/BR-D7 的"倾向"在 v0.1 范围内升为"已定"; 命令面在 §5/§7.7/§6.5 细化 |
| `2-01` | 承接 §2 大纲第 1/2/3/5 项中属 v0.1 的部分; 本篇是 `2-01` 第 1 项(manifest 格式定稿)的**工具侧** |
| `4-03` | 本篇给出 `plugin.toml`/`product.toml` 的 v0.1 schema 草案与真值裁定; 语义权威仍在 `4-03`, 拍板后 `4-03` §2/§3 收缩为指针 |
| `4-04` | 本篇给出**带 `compat_gen` 精确匹配的 3 段区间语义**、单版本政策、相位单调规则、分类学禁则; `4-04` §2/§3 据此收敛 |
| `4-02` | 本篇只钉三件(§8.3), 其余仍归 `4-02` |
| `3-05` | 运行期**单向**消费 v0.1 的生成物(init 顺序表/描述符段); 运行期零检测不变 |
| `1-02` | 三态/状态机/门钩/golden 语料原样继承(**语义不修改** —— §2.1 本就写"新增=轻量"); 本篇**新增**解冻瞬态与两条改面路径, 并补上"声明面 hash"这一**过渡真值**及其迁移门禁 |
| `1-03` | 本篇是 §5 DoD 第 4/5 项的**工具侧交付**; M0 的"环检测验收"由 V-3 承接 |

### 13.2 待对齐修订清单(拍板后回灌)

| # | 修订 | 位置 |
|---|---|---|
| A-1 | 八类 → `plugin_type`(四类) + `subkind`, 并新增 `api_type` 维度 | `1-01` §6.3 / `4-01` §2 |
| A-2 | APP "仅经 interface"(删除"直调 native"口径; 由 `iface-min` 承接) | `1-01` §6.3 / `4-01` §2 / `9-01` |
| A-3 | 描述符 `ver[3]` → `ver[4]`, 新增 `iface_hash` | `1-01` §6.1 / `3-05` §2 |
| A-4 | 表达格式确定 TOML(关闭开放问题) | `4-03` §3 / `3-05` §3 / `2-01` §2 |
| A-5 | 版本区间语义 = 四段 + `^`/`~` 定义; 单版本政策 | `4-04` §2/§3 |
| A-6 | 新增相位单调规则 | `4-04` §2(新条目) / `3-05` §2 第 4 项 |
| A-7 | 新增特权接口(P0–P4)与 memory 粒度模型 | `3-01`(API 分组) / `3-04` / 新篇或 `4-01` |
| A-8 | 状态目录定为 `build/`(生成物/索引), 新增 `br.lock` 与 `api/iface/**` | `2-02` §3 BR-D5 / Q5 |
| A-9 | `--json` 与 manifest schema 同源(关闭 Q7) | `2-02` §8 |
| A-10 | `br_dep_t.phase` 语义收窄为"相位断言"; 新增自身相位声明 `[plugin].phase` | `1-01` §6.1 / `4-04` §2 |
| A-11 | 导出面分类规则(`export.api_iface ≡ api_type`; `third_party` 无导出面; `skin` 边豁免 api_type 禁则) | `1-01` §7.3(`api_syms`/再导出) / `10-01` §2 第 1 项 / `4-03` §2 / `11-01` |
| **A-12** | **版本模型**: 段为 `COMPAT_GEN.MAJOR.MINOR.REVISE`; **`COMPAT_GEN` 与 `MAJOR` 解耦**(接口契约 vs 产品演进); 依赖钉精确 `compat_gen` + `range` 只比 3 段 | `1-01` §6.1 / `4-04` §2 / `1-02` §2.3 层 3 |
| **A-13** | **新增解冻/重新冻结机制**: 单元级 `freeze_state`(含 `unfreezing` 瞬态); 两条改面路径(软: 面内 RFC+弃用, 代不变 / 硬: 解冻重冻, 代 +1); `1-02` §2.6.2 补该转移 | `1-02` §2.6.2 / §2.2 阈值表 |
| **A-14** | `br_dep_t` 增 `compat_gen` 字段(结构依赖不带); `ver[3]` → `ver[4]`, 语义即四段新含义 | `1-01` §6.1 / `3-05` §2 |
| **A-15** | 依赖 `range` 表达式统一 3 段(旧写法 `">=1.0"`/`">=1.0.0"` 归一; 4 段非法) | `4-04` §2 / `1-01` §7.6 |
| **A-16** | **`COMPAT_GEN` 的粒度 = 接口/冻结批次单元**; 插件级取 `max(F_u)` | `3-01` §15 / `1-02` §2.6.4 / §3.2 |
| **A-17** | `VER` 域错误码 7 个 + `IFACE-0009`(见 BRV-D8 编码表) | `BRV-D8`(本篇) |
| **A-18** | **冻结计划补齐非 core 组批次**: 框架件四件(`br-devcore`/`br-cdevcore`/`br-vfscore`/`br-bdevcore`)、`svc-posix` POSIX 面(`br-svcposix.txt`)——它们**有治理声明却无冻结批次**, 而 §7.5 使"未冻结"在 release 下成为硬阻断 ⇒ 必须排期; 且**以接口单元为粒度**(依 A-16) | `3-01` §1 表 + §15 / `1-02` §2.3 层 1 |
| **A-19** | 明确 `crypto`/`keyring` **ops 表是否入 golden**(关闭 `11-01` §3 开放问题) | `11-01` §3 / `9-02` §6.2 O-H7 |
| **A-20** | `frozen` 补 gloss: "**单向冻结(append-only 保护)**"; 明确"新增=轻量"与"新增免解冻"是同一规则 | `1-02` §2.1 |
| **A-21** | 段名 `frozen_version` → **`compat_gen`**(避免被读成"冻结时的版本"而诱导跨代比较) | 本篇全篇 / `r1/05` §3.1 |
| **A-22** | `append vs modify` 的**条目级判定表**(§5.4): 明确"给已冻结结构体加字段 = **修改** ⇒ 须解冻", 与 D22"后补字段 = 布局破坏"对齐 | `1-02` §2.4 / §4.1 |
| **A-23** | "解冻/重新冻结"纳入 `1-02` §2.2 的**最高门槛**行 | `1-02` §2.2 |
| **A-24** | `IFACE-IR` 增 `macro`/`var`/`enum`/`service` 的 hash 输入字段(`value`/`ops`)+ `typedef` 表 + 枚举输出序 + hash 域分隔 | `1-02` §2.6.4 / 本篇 §6.2 |
| **A-25** | 冻结评审清单增"**冻结前应预留扩展槽**"(D22 的推广: 预留即免将来解冻) | `1-02` §2.6.2 / `3-01` §15 |
| **A-26** | **v0.1 `freeze` 只能生成"待升格提案"**的显式例外(因 `1-02` §2.6.2 要求 conformance 矩阵全绿, 而 test 排 v0.4) | `1-02` §2.6.2 / 本篇 §6.4 |

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

1. §4 的 **BRV-D1–BRV-D10** 逐条拍板(D10 已由需求方规则给定, 只需确认其三条不变量与 `skin` 豁免);
2. §11 的 **BRV-Q2/Q3/Q5/Q6/Q7** 至少给出结论(**Q1 已关闭**); 其余可挂"v0.x 再定";
3. §13.2 的 **A-1…A-26** 待对齐修订**全部回灌**到对应文档(否则同一契约两套口径, 正是 `comment/README.md` 记录的"元契约漂移"病根); 其中 **A-18/A-19 属"阻塞 release"级**, 应优先;
4. 补两张 PlantUML 图(接口发布时序 / 求解器数据流)并按 README 流程生成 `pics/`;
5. `2-01` §2 大纲第 1/2/3/5 项与 `4-03`/`4-04` 的对应小节收缩为指向本篇的指针。
