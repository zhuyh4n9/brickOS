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
- **插件描述符**(§6.1): `BR_PLUGIN(...)` 宏生成 `br_plugin_t`(元契约, 冻结); 描述符带 `abi_id` 字段(D14: Day1 即存在, v2 二进制分发起生效)
- **生命周期**(§6.2): EARLY → CORE → LATE → APP 四相(回调↔阶段映射见主文档 §6.2); 启动序列见主文档 §9
- **init-DAG**(§6.5): manifest 依赖声明 → 拓扑排序; **环 = 组合期硬错误**(报环路径)
- **组合期校验**(§6.4): 资源冲突(IRQ/DMA/引脚/设备名唯一)、版本区间
- **八类插件分类学**(§6.3): Platform / Scheduler(恰一)/ 框架件 / IO / FS / Service / Interface(严格叶子)/ APP(恰一)

## 2. 大纲(待成文)

1. manifest 格式定稿(DoD 第 4 项)→ **语义已迁至 `docs/4-plugin/4-03-plugin-manifest.md`**(schema/真值/校验输入), 2-01 定工具实现; 本篇保留组合期校验(本节第 5 项)对 manifest 结果的**消费**
2. 描述符二进制细节: 段位置(`.br_plugins`)+ 边界符号枚举(`__br_plugins_start/__br_plugins_stop`, 链接脚本 PROVIDE——段名含 `.` 非 C 合法标识符; 机制见 3-01 §13.3)、abi_id 编码(D14)
3. 生命周期状态机: 注册→early_init→init→start→运行; 错误路径(init 失败 = 启动失败)
4. init-DAG 实现: 拓扑排序、环检测输出、相(EARLY/CORE/LATE/APP)内排序
5. 组合期校验清单(全量规则): 资源冲突 / 符号命名空间 / 设备名唯一 / 版本区间 / sched_class 与调度器组合合法性
6. 符号命名空间: `br_*` 独占(§7.2)、链接期检查手段
7. 动态加载(v3): modload 服务 + ed25519 鉴权(D1 修订)——描述符如何扩展

## 3. 开放问题

| # | 问题 |
|---|---|
| — | manifest 的表达格式(YAML? TOML? 自定义 DSL [?])→ 迁 `docs/4-plugin/4-03-plugin-manifest.md` §3 |
| — | 插件版本区间语义(semver 子集?)→ 迁 `docs/4-plugin/4-04-plugin-deps.md` §3 |
| — | 动态加载的描述符兼容(D14 布局契约的交互) |

## 4. DoD 关联

`docs/1-architecture/1-03-roadmap.md` §5 第 4 项: **描述符 + manifest 格式**——本篇交付描述符侧; manifest 侧 → `docs/4-plugin/4-03-plugin-manifest.md`。
