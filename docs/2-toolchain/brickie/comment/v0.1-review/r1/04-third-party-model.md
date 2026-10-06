# br-tool v0.1 设计备忘 · third_party 与接口族模型

> **文件性质**: **设计备忘(memo)**, 不是缺陷清单 —— 记录对需求方三问的推演、建议方案与其代价, 供拍板。
> **缘起(需求方三问)**:
> 1. third_party service 的功能如何提供给 APP? 这种情况如何处理 interface 的 api 类型 —— 是否标记为 `runtime_adapter` 但允许抛 native api? `runtime_adapter` 是否改名为 `adapter`?
> 2. third_party 之间的调用如何实现? 是否可要求基于现有机制(device management / IPC 等)实现? 这种情况下依赖如何管理? 是否允许 third_party 抛出 third_party api?
> 3. 1/2 的修改带来哪些挑战?
> **关联缺陷**: 本备忘同时收口 [`r1/03`](03-tech-review.md) P0-3(四个真实插件安放失败)与 [`r1/01`](01-self-review.md) P0-4(`third_party` 消费面死锁); 与 `r1/03` P0-4/`r1/01` P0-1(版本模型)共享同一片手术区(§4.1)。
> **状态**: 待拍板 —— §7 列 5 个待决问题(D1–D5)。
> **基线**: 草案 commit `4dfdcb2`; 既有文档为当前 `brickOS-Design`。

## 0. 一个必须先纠正的前提: 本系统没有 IPC

需求方第 2 问把 IPC 列为可用机制之一。**IPC 不存在, 且是设计上刻意不存在**:

> `1-01` §2.1(行 142): 同一时刻只有一个应用 ⇒ **无进程模型、无 fork/exec、无跨进程 IPC、无地址空间切换**

单地址空间、静态链接、单 APP、EL1 恒特权。因此"进程间"的机制一个都没有。三方件之间可用的**会合点**只有三个, 且全在 native 侧:

| 会合点 | 归属 | 语义 | 出处 |
|---|---|---|---|
| 服务注册表 | core 公地 | 名字 → 指针; **core 不解释类型**(类型契约由服务方文档化) | `3-06` §1 |
| 设备注册表 | dev-core | 设备名 → ops 子分类 | `8-01` §2 |
| 挂载表 | vfs-core | 路径 → FS | `7-01` §2 |

外加一个比任何"机制"都更决定答案的**事实**: **所有插件最终被静态链接进同一镜像**, 符号天然可见。这一个事实使"强制走机制"在技术上无意义 —— 你能阻止的是*声明*, 不是*可达性*。

## 1. 根因: 三值枚举混了两个轴

`native | runtime_adapter | third_party` 中, 前两值回答"**我的 API 是什么形状**"(规范族), 第三值回答"**我的代码从哪来**"(来源)。混轴使每一个棘手的真实插件都落在枚举的缝里:

| 真实插件 | 来源 | 规范族 | 现枚举能否表达 |
|---|---|---|---|
| `svc-posix` | 自研 | POSIX | ✅ `runtime_adapter` |
| `service/sqlite` | **上游** | sqlite3 API | ⚠️ `third_party`(只说了来源, 没说面) |
| `iface-pkcs11` | 自研 | PKCS#11(域标准) | ❌ 既非 native, 也非"运行时适配" |
| `app/*` | 自研 | **无**(纯消费者) | ❌ 只能硬填 `native` ← 这正是评审指出"凭空默认值"的来源 |

**结论**: 不是"给枚举加例外", 而是**拆成两个正交轴**。拆轴后原三值一个不丢:

```
origin : first_party | third_party     # 代码来源 → 治理义务 / 移植增量 / 符号命名空间
family : native | adapter | upstream   # 规范族   → 接口命名 / 治理级别

  原 native plugin          = origin=first_party, family=native
  原 runtime_adapter plugin = origin=first_party, family=adapter
  原 third_party plugin     = origin=third_party, family=(由它暴露什么决定)
```

`family` 的第三值 **`upstream`** = **上游 API 原样暴露, 不受我方治理**(无三态 / 无 `a` 段 / 无 golden)。由此:

```
governance = family == "upstream" ? "registered" : "governed"   # 派生量, 禁止手写
```

- `governed`: 进 golden + 三态 + `a` 段 —— 有既有依据(`1-02` §2.3 层 1: native 面与 svc-posix 的 POSIX 面**都是被治理对象**)
- `registered`: 只登记"上游版本 + 面 hash" —— v0.1 里 `upstream` 族**甚至只登记版本, 不算面 hash**(§4.5)

## 2. Q1: third_party 的能力如何提供给 APP

### 2.1 三条路线(直接回答)

| 路线 | 机制 | 治理级别 | APP 编译期类型依赖 | 评价 |
|---|---|---|---|---|
| **R1 注册表晚绑定** | `br_service_lookup("db")` → 转型调用 | 无单元 | **仍然存在**(见 §2.2) | ⚠️ 不是"零声明" |
| **R2 upstream 单元 + 皮肤再导出** | third_party 抛 `family=upstream` 单元; `iface-*`(`family=adapter`)以 `reexport_of` 薄包装 | registered | 显式(经皮肤) | ✅ **默认路径** |
| **R3 包装成 native 面** | 由一个 **native 包装插件**承载(独立插件, `family=native`) | **governed** | 显式 | ✅ **深度路径** |

### 2.2 R1 的关键陷阱(必须写进文档, 否则误解会扩散)

`4-04` §3 与草案 §3.1 都写过"调用依赖走注册表、零声明"。**在 `third_party` 上这不成立**:

> 静态单地址空间下, 注册表只消除 **init 顺序**依赖, **不消除编译期类型依赖** —— APP 仍需 `db_ops` 的结构定义才能转型。

所以 R1 不是"无依赖", 而是**把依赖藏进了编译器看不见的地方**。它带来的真实代价是: 依赖图不完整 ⇒ `br check` 无输入 ⇒ 版本区间无从约束。**这正是必须允许 `third_party` 抛接口的核心理由**(§2.4)。

### 2.3 回答"是否需要标记为 runtime_adapter 但允许抛 native api"

**不建议走"标记 + 例外"**。理由: 例外会累积 —— `iface-pkcs11` 仍无处安放, `app` 仍要硬填一个不存在的值, 且"某类插件可以抛另一类的面"这个规则本身**没有可执法的边界**(例外之外还能再开例外)。

拆轴后, "`third_party` 抛 `native` 面" 是一个**正常组合**, 而且是**推荐路径**: 它把上游能力经 native 面**纳入治理** —— 这正是 `1-01` §7.6 移植双模式在**接口侧**的镜像(双模式讲的是它怎么**调我们**, 这里讲的是它怎么**给我们用**)。

### 2.4 硬结论: 为了让 Q1 有解, 必须允许 third_party 抛接口

需求方给过的硬规则是"`third_party` **暂不允许**抛出接口"。该规则在拆轴前是**必要的**(否则 `third_party` 抛什么族无法表达); 但拆轴后它**反而堵死了解法**:

- **R2 不成立**: 皮肤的 `reexport_of` 需要一个**单元作为指向对象**; 若 `third_party` 不抛接口, 这个对象不存在 ⇒ 皮肤无从声明
- **R3 也受阻**: R3 只是把面**换成** native 承载, 仍需要一个单元来登记 "这个 native 面之下是 sqlite"

> **建议修订**: `third_party` **允许抛出 `family=upstream` 的接口单元**, 但该单元**不受 `a` 段/三态治理**(governance=registered)。原硬规则的**意图**(上游面不被误当成我们承诺的契约)由 `family=upstream` + governance 派生量**精确表达**, 不再需要"禁止抛出"这剂猛药。

### 2.5 回答"runtime_adapter 是否改名为 adapter api"

**不是改名, 是降级为族名**; 而且成本几乎为零。实测:

```
runtime_adapter 在既有 21 篇设计文档中出现次数: 0
(22 次全部集中在非设计文档: 本轮新草案 br-tool-v0.1.md)
```

该词是**本轮才引入的**, 既有语料里只有散文式的"适配层/适配器"(指 cdev-core 的 `file_ops` 适配、littlefs 的 bdev 适配), 没有任何枚举/类名占用它。因此 `adapter` 作为族名**零冲突**。

**语义澄清(重要)**: `adapter` 族 = "**我方治理的非 native 标准面**", 而不是 "运行时适配器这个角色产出的面"。这条定义同时覆盖:

- POSIX 运行时(`svc-posix`)—— 通用标准
- 域标准皮肤(`iface-pkcs11` PKCS#11) —— 域标准

→ `iface-pkcs11` 因此**随之安放**: `origin=first_party, family=adapter`, 它依赖 `crypto`/`keyring`(native 服务)也落在"adapter 只允许依赖 native"之内, **无需任何例外**。这正是拆轴的收益之一。

## 3. Q2: third_party 之间的调用如何实现

### 3.1 不要强制"必须经现有机制"

**不建议强制走 device management / 注册表中介**, 因为它会杀掉一份战略资产:

| `1-01` §7.6 的移植模式 | 增量 | 强制中介的后果 |
|---|---|---|
| 模式 A: `os_unix.c` 直链 svc-posix | **~0** | **直接杀死**(要求先做 600 行适配才能进组合) |
| 模式 B: `os_brick.c` VFS 后端 | ~600 行 | 可行, 但模式 B 本是"深度路径" |

`1-01` §7.6 明说模式 A 是"快速跑通"路径, 而战略语境称"**移植故事是架构生死线**"。强制中介 = 取消快速路径 = 与战略语境直接冲突。

### 3.2 建议: 允许直连, 但必须声明 + 补 `kind="type"`

单地址空间下三方件互调**物理上必然**(同一镜像, 符号可见)。因此正确做法是**让它可见**, 而不是假装它经了中介。三分法已足够, 只需补一个缺失的值:

| 依赖形态 | 需要的 `kind` | 现状 |
|---|---|---|
| A 调用 B 的符号 | `runtime` | ✅ 已有 |
| A 需要 B 先初始化 | `init` | ✅ 已有 |
| **A 只需要 B 的类型/头文件** | **`type`** | ❌ **缺失** |

补 `kind="type"` 是"一箭双雕": 它**同时**解掉 [`r1/03` P0-3③](03-tech-review.md) —— `dev-core → vfs-core` 是**仅头文件类型依赖**(`8-01` §1.3 / O-S7), 若写成 `runtime`, 按§7.1"runtime 参与闭包"会把 vfs-core 拖进 `8-01` 的"形态 B", **推翻 D19/O-S7**。同一条缺口同时卡住"三方件互调"与"框架件类型依赖"。

### 3.3 环: 三分法已经够用, 但 init 期互调必须降级

- **`init` 边禁环**(拓扑硬错误) —— 三方库的初始化顺序(`sqlite3_initialize`、lwIP `sys_init`)确实需要它
- **`runtime` 边允许环** —— 三方库经 callback 互调是常态(典型: TLS 库让调用者提供 I/O 回调 ⇒ 与网络栈符号级互调)。这正是 `1-01` §6.5"环的四种解法"里的**依赖降级为数据流**
- **`type` 边**: 建议与 `runtime` 同政策(允许环), 因为头文件类型依赖天然双向(两边互引类型定义)

> **应写明的约束**: 若两库**在 init 期就必须互调**, 则必须先经注册表/回调把 init 依赖降级, 否则环无法消解。这是 `1-01` §6.5 既有四解法的应用, 不是新增机制。

### 3.4 回答"是否可以允许 third_party 抛出 third party api"

**必须允许, 而且这正是使边可类型化的前提。** 不允许的话, `third_party → third_party` 这条边**没有可指向的对象**: 你无法声明"我依赖 sqlite 的**哪个面**", 只能声明"我依赖 sqlite 这个插件" —— 粒度退化到插件级, **版本区间也就无从谈起**。

**建议**: 允许, 但该面标记 `family="upstream"`、`governance=registered`(有上游版本 + 面 hash, 无三态、无 `a` 段)。由此三方件在治理谱上是一条**渐进阶梯**:

```
原生 sqlite3_* 直接暴露     → family=upstream   registered   (快, 未治理)
经 native ops 表包装        → family=native     governed     (慢, 进 golden/三态)
```

这条阶梯同时给了"先跑通、后治理"一条**不破坏契约**的演进路径 —— 与 `1-01` §7.6 模式 A→B 的渐进精神一致。

### 3.5 一个与依赖边无关、但会咬人的风险: 符号冲突

两个上游库同镜像, **链接期符号冲突**是真实风险(两者各自可能带 `crc32` / 压缩 / 内存工具函数)。这与"依赖边直连与否"**无关** —— 无论直连还是省耦合, 都要链进同一镜像。

需提前立项的措施(建议进 v0.1 声明面, 构建手段留 v0.3):

| 措施 | 层次 | v0.1 可做? |
|---|---|---|
| manifest 登记每条 `origin=third_party` 的**符号前缀/命名空间** | 声明 | ✅ 纯声明 |
| 组合期检查前缀重叠 | 校验 | ✅ 纯声明 |
| `-ffunction-sections --gc-sections` | 构建 | v0.3 |
| `objcopy --prefix-symbols` | 构建 | v0.3 |
| 依赖上游自带前缀 | 上游 | v0.3 |

注意这与既有 `1-01` §7.2 规则 4"**符号命名空间独占**"(执法机制 = D13 符号级碰撞检测)是**同一规则在三方件上的延伸**; 现有文档只覆盖了一方插件的 `br_*` 前缀纪律, 未覆盖上游库自带符号 —— 这是一个**真实的空白**。

## 4. Q3: 这些修改带来哪些挑战

按影响面排序。

### 4.1 版本模型分裂(最深, 直接冲击 §5)

| 问题 | 说明 |
|---|---|
| `a` 段对 `upstream` 族**无意义** | `a` = **冻结面**兼容代数, 前提是**我们有权冻结**。上游面我们无权冻结 ⇒ upstream 族**没有 `a`** |
| 上游 patch 是否传染皮肤的 `a`? | 3.45.1→3.45.2 让面 hash 变了。**涨**: 我们的接口兼容代数被上游牵着走; **不涨**: 必须按 `governance` 分流。需明确选择 |
| 区间语义要**两套比较器** | `^`/`~` 对上游版本是**上游自己的版本方案**(sqlite 是 `X.Y.Z` 三段), 与我方四段不同 |

**结论**: 需要 `governance` 派生字段 + 明确"`upstream` 族不进 `a` 段"。这与 [`r1/03` P0-4 / `r1/01` P0-1](01-self-review.md)(`a≡b`、`^≡~`)是**同一片手术区** —— 建议合并为一次决策, 否则会出现"两套版本语义各自打补丁"的局面。

### 4.2 三态失效 + D13 所有权悖论

- `1-02` §2.6.2 的三态状态机是 **core-only 的**: 我方无法 `frozen`/`deprecated` 上游的面 ⇒ 需要第 4 态(`unmanaged`), 或把"三态"改称"**治理态**"以示其适用边界
- **D13 所有权悖论**: D13(`1-02` §3.2)说 `reexports` **不转移所有权**、"主人被移除 ⇒ 组合期报错"。但 `upstream` 族的**主人是树外的上游** ⇒ 该报错条件必须改写为"**上游版本变更 ⇒ 需重验**"。D13 的强保证在此**降级**, 必须显式记录而不是默认继承

### 4.3 依赖判定从单值变矩阵

现有禁则(`adapter 只允许依赖 native`、`native 不得依赖 adapter`)必须重写为**显式边表**, 且必须显式允许三条真实边:

| 边 | 现状 | 必须 |
|---|---|---|
| `svc-posix → service/lwip` | ❌ 被"只允许依赖 native"杀死 | **允许**(adapter → third_party) |
| `iface-posix → svc-posix` | ✅ 靠 `skin` 豁免 | 保留, 并把豁免**一般化**为族级规则 |
| `app → 任意` | app 无 `family`, 规则悬空 | 单独规则(§4.4) |

> 另注: `service/lwip` 被当作三方件的**唯一依据**是 `4-02` §2 大纲第 5 项("**携带上游源码(lwip/sqlite)**")—— 那是一条 **`[TODO]` 大纲项, 未成文**。因此"lwip 是 third_party"目前**不是已定结论**; 它也可能是 `origin=first_party, family=adapter`(若 lwIP 被当作我方治理的适配基座)。**这个归类必须先拍**, 因为它决定上表第一行是"允许"还是"不需要"(见 D2)。

### 4.4 APP 依赖面要重新表述

"APP 仅经 interface" 在 `third_party` 下必须二选一:

| 选择 | 代价 |
|---|---|
| 保规则: 要求皮肤 | 谁来写/维护皮肤? 上游 API 一变皮肤就漂(维护成本转移给产品方) |
| 放开: APP 可直依 `family=upstream` 单元 | APP 侧治理义务消失(但可接受, 见下) |

**关键澄清**: 放开**不会**破坏 interface 的"严格叶子"经济学 —— 那个性质约束的是"**谁依赖 interface 插件**"(保证换皮肤不重验服务, `1-01` §7.5 论证 2), **不是**"APP 能依赖什么"。所以放开是安全的, 只是 APP 侧少一层治理。

### 4.5 hash 的输入从哪来(v0.1 无编译 ⇒ 硬冲突)

`upstream` 族的面 = **上游头文件**; 而 v0.1 **无编译、无符号提取**(§0 边界)⇒ 谁提供这个面? 手写必然漂移。这**放大**了 [`r1/03` P0-2](03-tech-review.md)(hash 无文法)。

**建议**: v0.1 对 `upstream` 族**只登记"存在 + 上游版本", 不算面 hash**; 面 hash 推迟到 v0.2(有头文件提取能力时)。即 upstream 族在 v0.1 = **registered 但不 hashed**。这使 `family` 与 `hash_scope` 之间产生一条清晰的时序:**v0.1 只有 native/adapter 有 hash, v0.2 起 upstream 才有**。

### 4.6 回灌成本(这条是好消息)

| 项 | 实测 |
|---|---|
| `runtime_adapter` 在既有 21 篇设计文档的出现次数 | **0** |
| 改动波及的既有设计文档 | **0 篇**(仅需在本轮草案内改造 + 在 A 清单登记) |

因此拆轴**不需要修订任何既有文档**。建议把"分类学拆轴"作为一个**独立决策记录**(`docs/decisions/NNNN`), 而不是混在 `§13.2` 的 A 清单里 —— 否则以后无人能追溯"为什么三值变两轴、`runtime_adapter` 为何消失"。

### 4.7 可裁剪性与预算

`third_party` 带入的体积/RAM 使"极小组合"的边界模糊。需要 per-third_party 的资源声明 + **可选性**。注意: 这与 `BRV-Q10`(可选/弱依赖, 倾向"不进 v0.1")**直接相关** —— 若三方件不能声明可选, 则"裁剪变体"无法表达。**这两个决定应一起拍**。

## 5. 建议的完整模型(供直接改写 §3/§5/§7)

### 5.1 字段

```toml
[plugin]
name    = "service/sqlite"
origin  = "third_party"    # first_party | third_party   (代码来源)
family  = "upstream"       # native | adapter | upstream (规范族)
# governance 为派生量, 禁止手写:
#   family == "upstream" => "registered"   否则 => "governed"

[upstream]                 # 仅 origin="third_party"
version  = "3.45.2"        # 上游版本(方案由上游决定, 不套我方四段)
patches  = ["0001-brickos-vfs.patch"]
symbol_prefix = "sqlite3_" # §3.5 符号冲突检查的输入
```

### 5.2 相容矩阵(替代单值禁则)

| 消费者 \ 提供者 | native | adapter | upstream |
|---|---|---|---|
| **native** | ✅ | ❌ | ✅(经显式声明) |
| **adapter** | ✅ | ✅(仅 `skin` 再导出) | ✅ |
| **third_party** | ✅ | ✅ | ✅ |
| **app** | ❌(须经 interface) | ❌(须经 interface) | ◐ **待 D4 拍板** |

### 5.3 三条真实边的落位

```
svc-posix      (first_party, adapter)  --init-->   service/lwip (?, ?)        ✅ 矩阵允许
iface-posix    (first_party, adapter)  --skin-->   svc-posix   (adapter)      ✅ 豁免, 见下
dev-core       (first_party, native)   --type-->   vfs-core    (native)       ✅ 需新增 kind="type"
```

**`skin` 豁免的一般化表述**(建议同时写进 §7.3):

> `form="skin"` 的再导出边**豁免族间禁则**, 条件是 `reexport_of` 显式声明; 且被再导出单元的
> `governance` **不得高于**皮肤自身(registered 可被 governed 皮肤再导出, 反之不可)。

### 5.4 三条路线的 TOML 落位

```toml
# R2: third_party 抛 upstream 单元, iface-* 薄包装 (family=adapter 皮肤)
[[export]]
family  = "upstream"       # 恒等于 [plugin].family
form    = "api"
name    = "sqlite3"
version = "3.45.2"         # 上游版本
governance = "registered"  # 派生, 回显以便人读

# R3: 由 native 包装插件承载 (governed)
[[export]]
family = "native"
form   = "service"
name   = "db"
version = "0.1.0.0"
```

## 6. 对既有文档的回灌影响

| 文档 | 影响 |
|---|---|
| `1-01` §6.3 | 八类 → `plugin_type` + `subkind`(**原 A-1**); 另需补 `origin`/`family` 两轴 |
| `1-01` §7.2 规则 4 | 符号命名空间独占**扩到上游库自带符号**(§3.5; `1-01`/`1-02` 现行文本未覆盖) |
| `1-02` §2.6.2 | 三态加"适用边界 = governed"(§4.2) |
| `1-02` §3.2 | D13 再导出的"主人被移除 ⇒ 报错"需为 upstream 族改写(§4.2) |
| `4-03` §2 | manifest 增 `origin`/`family`/`[upstream]` |
| `4-04` §2 | 依赖边表取代单值禁则(§5.2); 新增 `kind="type"` |
| `11-01` §1 | lwip 归类(**D2**) |
| 本篇草案 §3/§5/§7 | 主要手术区 |

## 7. 待决问题

| # | 问题 | 倾向 | 阻塞 |
|---|---|---|---|
| **D1** | 是否采纳**拆轴**(`origin` × `family`, `family` 增 `upstream`; `runtime_adapter` 降为 `adapter` 族名)? | **采纳**(§1/§2.5; 回灌成本为 0) | §5 全篇; 本备忘其余各条 |
| **D2** | `service/lwip` 的归类: `third_party/upstream` 还是 `first_party/adapter`? | **倾向先定为 `third_party/upstream`**(唯一文字依据是 `4-02` §2-5 的 `[TODO]`), 但要需求方确认 | §4.3 的边表 |
| **D3** | 是否撤销"`third_party` 不抛接口", 改为"可抛但 `family=upstream`、不受 `a` 段/三态治理"? | **撤销**(否则 R2/R3 均不成立, §2.4) | Q1 的解法 |
| **D4** | APP 可否直依 `family=upstream` 单元? | 二选一(§4.4); 倾向**放开**, 因"严格叶子"约束的是反向 | APP 依赖面 |
| **D5** | 上游 patch 是否传染皮肤的 `a` 段? | 倾向**不传染**(按 `governance` 分流, §4.1) | 版本模型 |

## 附录 · 事实核查表

本备忘所有事实均已回原文核对, 并**记录一处评审引用错误**:

| # | 事实 | 核对结果 |
|---|---|---|
| 1 | 无进程模型/无 IPC | ✅ `1-01` §2.1(行 142)原文 |
| 2 | 服务注册表"core 不解释类型" | ✅ `3-06` §1 原文 |
| 3 | `runtime_adapter` 在既有 21 篇出现 0 次 | ✅ grep 全库; 22 次全在本轮草案 |
| 4 | `1-02` §2.3 层 1 治理 svc-posix 的 POSIX 面 | ✅ 行 94 原文 ⇒ 支持 `governance=governed` 覆盖 adapter 族 |
| 5 | `1-01` §7.2 规则 4 = 符号命名空间独占 | ✅ 行 425; 执法 = D13 符号级碰撞检测 |
| 6 | `1-01` §7.6 模式 A ~0 / 模式 B ~600 行 | ✅ 行 480–481 原文 |
| 7 | **`4-02` §5 不存在** | ⚠️ **评审引用错误**: `4-02` 只有 §1–§4(缩略词/范围/大纲/开放问题/DoD)。[`r1/03`](03-tech-review.md) P0-3① 引的"`4-02` §5"实为 **§2 大纲第 5 项**(行 38), 且该项标 **`[TODO]` 未成文** |
| 8 | "lwip/sqlite 是三方"的唯一依据 | ⚠️ 仅 `4-02` §2-5 的 `[TODO]`("携带上游源码(lwip/sqlite)")⇒ **不是已定结论**(见 D2) |
