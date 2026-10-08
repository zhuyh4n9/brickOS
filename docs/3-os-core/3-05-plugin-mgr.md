# 3-05 — 插件管理器(plugin_manager)

> 章节: **3-os-core**。状态: **骨架**。
> 来源: 主文档 §4.4(plugin_manager)、§6(插件体系: 描述符/生命周期/依赖/组合校验); 治理: `docs/1-architecture/1-02-api-contract-governance.md`。
> 分工: 本篇 = 插件体系的**管理面**(谁是插件/怎么初始化/怎么校验); manifest 格式与语义 → `docs/4-plugin/4-03-plugin-manifest.md`; 依赖语义 → `docs/4-plugin/4-04-plugin-deps.md`; 插件布局 → `docs/4-plugin/4-02-plugin-layout.md`; 服务注册表 → `docs/3-os-core/3-06-service-mgmt.md`; 插件**作者视角**(怎么写一个插件)→ `docs/4-plugin/4-01-plugin-dev.md`; 组合器**工具实现** → `docs/2-toolchain/2-01-toolchain.md`。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **abi_id** | ABI identifier | 工具链 + ABI 影响选项的指纹(描述符字段, D14 二进制分发) |
| **APP** | Application | 应用(插件类别: 唯一业务逻辑, 恰一个, 不被任何插件依赖) |
| **DAG** | Directed Acyclic Graph | 有向无环图; init-DAG = 初始化依赖拓扑, 环即组合期硬错误 |
| **DMA** | Direct Memory Access | 直接内存访问(外设不经 CPU 读写内存) |
| **DSL** | Domain-Specific Language | 领域专用语言(manifest 表达格式候选之一) |
| **EARLY / CORE / LATE / APP** | — | 插件生命周期四相(主文档 §6.2): 关中断无线程 → 堆可用 → 服务/接口 → 进 APP main |
| **ed25519** | Edwards-curve Digital Signature Algorithm (25519) | 签名算法(v3 动态加载鉴权) |
| **FS** | File System | 文件系统(插件类别之一) |
| **I/O** | Input/Output | 输入输出; 插件类别 **IO** = 外设驱动 |
| **IRQ** | Interrupt Request | 中断请求(硬件中断线) |
| **R1–R10** | — | 主文档 §16 的风险编号 |
| **semver** | semantic versioning | 语义化版本(maj.min.pat; 依赖区间语义的候选子集) |
| **TOML** | Tom's Obvious Minimal Language | 配置文件格式(manifest 表达格式候选) |
| **YAML** | YAML Ain't Markup Language | 配置文件格式(manifest 表达格式候选) |

> **编号约定**: `D1`/`D14` = 全局决策; `§6.x` = 主文档插件模型各节。

## 1. 范围与现状

plugin_manager 是 core 的**编排者**: 拥有插件生命周期与 init 拓扑。组合期(构建时)做静态校验, 运行期只做编排——**v1 无热插拔**。

已有决策(待深化时继承):
- **插件描述符**(§6.1): `BR_PLUGIN(...)` 宏生成 `br_plugin_t`(元契约, 冻结); **形态定稿 = `static const` + `.br_plugins` 段**(A-3, 近零导出面 CA-10); 描述符带 `abi_id` 字段(D14: Day1 即存在, v2 二进制分发起生效); **`ver[4]`** = `COMPAT_GEN.MAJOR.MINOR.REVISE`, `br_dep_t = {name, range, phase, compat_gen}`(`compat_gen` 仅接口依赖携带)
- **生命周期**(§6.2): EARLY → CORE → LATE → APP 四相(回调↔阶段映射见主文档 §6.2); 启动序列见主文档 §9。**每插件两个完成点**: ① EARLY 的 `early_init` = 注册可用(全部插件); ② `phase` 所声明相的 `init` = 能力可用(非 Service/Interface ⇒ CORE, Service/Interface ⇒ LATE)
- **init-DAG**(§6.5): manifest 依赖声明 → 拓扑排序; **环 = 组合期硬错误**(报环路径); **相位单调规则**: 对每条 `init` 边 `A → B`(A 依赖 B)必须 `rank(complete(B)) ≤ rank(complete(A))`(`rank(EARLY)=0 < CORE=1 < LATE=2 < APP=3`), 口径前提是**每插件两个完成点**(① `early_init` 返回 = 注册可用; ② `phase` 所声明相的 `init` 返回 = 能力可用); `br_dep_t.phase` 是依赖方的**断言**(与提供方自述 `[plugin].phase` 冲突 ⇒ 红)。两条校验码: **(R1) 完成点单调**违例 ⇒ `BRV-DEP-0009`; **(R2) 断言一致**冲突 ⇒ `BRV-DEP-0010`。**相内排序由拓扑序决定, 相间由本条规则校验**; 规则**只覆盖已声明 init 边**, 隐式耦合归符号级扫描(`brickie` v0.1 §7.2, A-6)。判例: `sched-coop → platform/qemu-aarch64` 两边第 ② 完成点同在 CORE ⇒ `CORE ≤ CORE` **合法, 不误杀**
- **组合期校验**(§6.4): 资源冲突(IRQ/DMA/引脚/设备名唯一)、**资源预算**(Σ per-plugin RAM/栈 ≤ 产品预算, 输入 = `plugin.toml` 的 `[[res]]`)、版本区间(四段 + `compat_gen` 精确匹配 + `range` 3 段)、**`sched_class` 与调度器组合合法性**(输入 = `[plugin].sched_class`)
- **插件分类学(A-1)**: **规范形态 = `plugin_type`(四类: `platform`/`ability`/`interface`/`app`)× `api_type`(三值: `native`/`runtime_adapter`/`third_party`)+ 派生列 `subkind`**(scheduler/framework/io/fs/service); 旧"八类"(Platform/Scheduler/框架件/IO/FS/Service/Interface/APP)只是该组合的**人读视图**, 不再是独立维度。**数量约束与依赖方向只由 `plugin_type` + `api_type` 决定**, `subkind` 只用于授权与检索 —— 权威定义见 `docs/1-architecture/1-01-architecture.md` §6.3 的表 A/B/C
- **导出面分类不变量**(A-11): `export.api_iface == plugin.api_type`(三值域); `form = "skin"` 的 `reexport_of` 是**列表**且每项分类必须与自身相等; `skin` 再导出边豁免 `api_type` 依赖禁则
- **解冻瞬态**: 接口单元的 `freeze_state`(`unfrozen`/`frozen`/`unfreezing`)由组合器侧读写, 运行期不感知(接口发布机制见 `brickie` v0.1 §5.3/§6.4 与 `1-02` §2.6.2)

## 2. 大纲(待成文)

1. manifest 格式定稿(DoD 第 4 项)→ **语义已迁至 `docs/4-plugin/4-03-plugin-manifest.md`**(schema/真值/校验输入), 2-01 定工具实现; 本篇保留组合期校验(本节第 5 项)对 manifest 结果的**消费**
2. 描述符二进制细节: 段位置(`.br_plugins`)+ 边界符号枚举(`__br_plugins_start/__br_plugins_stop`, 链接脚本 PROVIDE——段名含 `.` 非 C 合法标识符; 机制见 3-01 §13.3)、abi_id 编码(D14)、**`ver[4]` 与 `br_dep_t` 的字段序**(A-3/A-14; 头文件名由生成器约定, 见 checklist §5.6 裁定 #6)
3. 生命周期状态机: 注册→early_init→init→start→运行; 错误路径(init 失败 = 启动失败); **两个完成点与相内拓扑序、相间相位单调校验的关系**(主文档 §6.2)
4. init-DAG 实现: 拓扑排序、环检测输出、相(EARLY/CORE/LATE/APP)内排序、**相位单调校验(两条码 `BRV-DEP-0009` 完成点单调 / `BRV-DEP-0010` 断言一致; 相内排序由拓扑序决定、相间由规则校验; 含"只覆盖已声明 init 边"的边界声明)**
5. 组合期校验清单(全量规则): 资源冲突 / 资源预算 / 符号命名空间 / 设备名唯一 / 版本区间 / sched_class 与调度器组合合法性 / **相位单调(`BRV-DEP-0009`/`BRV-DEP-0010`, 只覆盖已声明 init 边)** /**导出面分类不变量**(§6.3 的 `BRV-TAX-0016/0017/0018/0019`)
6. 符号命名空间: `br_*` 独占(§7.2)、链接期检查手段
7. 动态加载(v3): modload 服务 + ed25519 鉴权(D1 修订)——描述符如何扩展

## 3. 开放问题

| # | 问题 | 状态 |
|---|---|---|
| ~~—~~ | ~~manifest 的表达格式(YAML? TOML? 自定义 DSL [?])~~ → 迁 `docs/4-plugin/4-03-plugin-manifest.md` §3 | **✅ 已答(A-4)**: **TOML** |
| ~~—~~ | ~~插件版本区间语义(semver 子集?)~~ → 迁 `docs/4-plugin/4-04-plugin-deps.md` §3 | **✅ 已答(A-5)**: 四段版本 + `compat_gen` 精确匹配 + `range` 3 段 |
| — | 动态加载的描述符兼容(D14 布局契约的交互) | 待定(v3; 与 `1-02` D14 布局硬门禁同轴) |

## 4. DoD 关联

`docs/1-architecture/1-03-roadmap.md` §5 第 4 项: **描述符 + manifest 格式**——本篇交付描述符侧; manifest 侧 → `docs/4-plugin/4-03-plugin-manifest.md`。
