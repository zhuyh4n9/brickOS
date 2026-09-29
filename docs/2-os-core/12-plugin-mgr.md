# 12 — 插件管理器(plugin_manager)

> 分类: **2-os-core**。状态: **骨架**。
> 来源: 主文档 §4.4(plugin_manager)、§6(插件体系: 描述符/生命周期/依赖/组合校验); 治理: `docs/1-architecture/01-api-contract-governance.md`。
> 分工: 本篇 = 插件体系的**管理面**(谁是插件/怎么初始化/怎么校验); 服务注册表 → `docs/2-os-core/17-service-mgmt.md`; 插件**作者视角**(怎么写一个插件)→ `docs/3-plugin-dev/18-plugin-dev.md`; 组合器**工具实现** → `docs/7-toolchain/13-toolchain.md`。

## 1. 范围与现状

plugin_manager 是 core 的**编排者**: 拥有插件生命周期与 init 拓扑。组合期(构建时)做静态校验, 运行期只做编排——**v1 无热插拔**。

已有决策(待深化时继承):
- **插件描述符**(§6.1): `TG_PLUGIN(...)` 宏生成 `tg_plugin_t`(元契约, 冻结); 描述符带 `abi_id` 字段(D14: Day1 即存在, v2 二进制分发起生效)
- **生命周期**(§6.2): EARLY → CORE → LATE → APP 四相(回调↔阶段映射见主文档 §6.2); 启动序列见主文档 §9
- **init-DAG**(§6.5): manifest 依赖声明 → 拓扑排序; **环 = 组合期硬错误**(报环路径)
- **组合期校验**(§6.4): 资源冲突(IRQ/DMA/引脚/设备名唯一)、版本区间
- **八类插件分类学**(§6.3): Platform / Scheduler(恰一)/ 框架件 / IO / FS / Service / Interface(严格叶子)/ APP(恰一)

## 2. 大纲(待成文)

1. **manifest 格式定稿**(DoD 第 4 项): schema、依赖声明、资源声明、挂载计划、生成物(`tg_fs_cfg` 等)——**本篇定语义, 13 定工具实现**
2. 描述符二进制细节: 段位置(`.tg_plugins`)+ 边界符号枚举(`__tg_plugins_start/__tg_plugins_stop`, 链接脚本 PROVIDE——段名含 `.` 非 C 合法标识符; 机制见 08 §13.3)、abi_id 编码(D14)
3. 生命周期状态机: 注册→early_init→init→start→运行; 错误路径(init 失败 = 启动失败)
4. init-DAG 实现: 拓扑排序、环检测输出、相(EARLY/CORE/LATE/APP)内排序
5. 组合期校验清单(全量规则): 资源冲突 / 符号命名空间 / 设备名唯一 / 版本区间 / sched_class 与调度器组合合法性
6. 符号命名空间: `tg_*` 独占(§7.2)、链接期检查手段
7. 动态加载(v3): modload 服务 + ed25519 鉴权(D1 修订)——描述符如何扩展

## 3. 开放问题

| # | 问题 |
|---|---|
| — | manifest 的表达格式(YAML? TOML? 自定义 DSL [?]) |
| — | 插件版本区间语义(semver 子集?) |
| — | 动态加载的描述符兼容(D14 布局契约的交互) |

## 4. DoD 关联

`docs/1-architecture/02-roadmap.md` §5 第 4 项: **描述符 + manifest 格式**——本篇核心交付。
