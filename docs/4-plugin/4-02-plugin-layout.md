# 4-02 — 插件布局(plugin layout)

> 章节: **4-plugin**。状态: **骨架(预留, 待细化)**。
> 来源: 主文档 §6.1(描述符)/§7.2(符号命名空间)/§13(`brickie add` 的插件路径形态); 3-01 §13.3(描述符段收集, CA-10); 2-01 §2 第 4 项(repo 骨架); 4-01 §7(脚手架开放问题)。
> 分工: 本篇 = **单插件**的布局语义(目录组织/命名/发现约定/物理映射); repo 全局骨架(plugins/ core/ apps/ tools/ 与构建入口)→ `docs/2-toolchain/2-01-toolchain.md`; 描述符二进制细节定稿 → `docs/3-os-core/3-05-plugin-mgr.md` §2 第 2 项; 作者流程入口 → 本类 `4-01-plugin-dev.md`。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **APP** | Application | 应用(插件类别: 唯一业务逻辑, 恰一个, 不被任何插件依赖) |
| **CA-1–CA-10** | — | core native API 契约决策编号(3-01 §14) |
| **CI** | Continuous Integration | 持续集成(三层门禁的落地处) |
| **CLI** | Command-Line Interface | 命令行界面 |
| **FS** | File System | 文件系统(插件类别之一) |
| **gc-sections** | — | 链接器裁剪选项: 未引用的 section 不进镜像(静态组合的裁剪手段) |

> **编号约定**: `CA-10` = 符号导出(描述符段收集); `D*` = 全局决策。

## 1. 范围与现状(预留)

插件 = 组合的基本单元, 布局是作者与组合器(`brickie`)之间的**发现契约**: 作者按约定放文件, 组合器按约定找到并校验。

已有事实(散点, 待收口):

- CLI 已暗示树内路径形态: `brickie add platform/qemu-aarch64 io/uart-pl011 fs/littlefs ...`(主文档 §13)——**按类别组织插件树**的读法**已定稿**(**顶层即 `namespace` 目录**; 见 §3 已答项与 `brickie-v0.1` §8.3 ④; r1/03 P0-7)
- 插件名 = 全局身份: 依赖声明(deps.name)/注册表/描述符 `name` 字段(§6.1)共用; **名字的 `namespace` 集合 = `app|iface|platform|sched|framework|io|fs|service`**(每项 → `plugin_type`/`subkind` 的映射见 `brickie-v0.1` §8.3 ①), **`ability` 只作为"无更好的 namespace 时"的通配读法**(不在枚举里, 不是推荐形态; r1/03 P0-7)
- 符号纪律: `br_*` 为 core 独占; 插件自有符号带插件名前缀(§7.2 规则 4)
- 描述符物理布局: `.br_plugins` 收集段 + `__br_plugins_start/__br_plugins_stop` 边界枚举(段名三处一致: 1-01 §6.1 / 3-01 §13.3 / 3-05 §2.2); **宏形态已定稿(A-3)**: `static const br_plugin_t`(近零导出面, CA-10) —— 本节原记录的"1-01 §6.1 非 static vs 3-01 §13.3 `static const`"矛盾已收敛到 `static const`, 1-01 §6.1 的展开式随之修订。`ver[4]` 字段与 `br_dep_t = {name, range, phase, compat_gen}` 同批定稿
- 脚手架: `brickie new <plugin_type> <name>` 生成模板(4-01 §7 开放问题; **两个位置参数已裁定**: 类 + 名, 落点在本篇 §2 第 6 项; `2-02` 的 `new <kind>` 是草图)
- **分类学口径(A-1)**: 插件的规范分类 = `plugin_type`(四值: `app`/`interface`/`ability`/`platform`)× `api_type`(三值: `native`/`runtime_adapter`/`third_party`)+ 派生列 `subkind`(scheduler/framework/io/fs/service); 数量约束与依赖方向只由 `plugin_type` + `api_type` 决定 —— 完整定义见 `docs/1-architecture/1-01-architecture.md` §6.3 的表 A/B/C
- **`api_type` 的目录维度**: v0.1 交付 `native` 模板; `runtime_adapter`/`third_party` 目录预留(选择时报 `BRV-TAX-0014`, 但校验已识别); **模板维度 = `<api_type>/<plugin_type>/<lang>/`**, 描述符另放 `descriptor/<api_type>/<lang>/`(与 `plugin_type` 正交)

## 2. 大纲(预留, 待细化)

1. **目录布局**: 单插件目录内的文件组织(生成物已钉三件见下) [TODO: manifest 位置 / src / include(对外的暴露面?)/ test / conformance / 平台数据 / 文档]
   - **v0.1 已钉的三件**(生成器侧真值): `plugin.toml` 在插件根; 人对外的头文件在 `include/`; 生成物一律落仓库级 `build/gen/<plugin>/`(不落插件目录)
2. **命名约定**: 插件名规则(字符集/大小写/长度); 目录名 ↔ 插件名 ↔ 符号前缀 ↔ 设备名(如涉)的一致性与推导 [TODO]
   - v0.1 已定: `name` 匹配 `^[a-z][a-z0-9]*(?:[-_/][a-z0-9]+)*$`, **全局唯一**; 推荐形态 `<namespace>/<short>`, **`namespace` 集合 = `app|iface|platform|sched|framework|io|fs|service`**(与 `brickie-v0.1` §8.3 ① 一表定死); 旧的 `svc-posix`/`sched-coop`/`iface-min`/`dev-core` 等**均合法不改名**(只对新增插件给推荐形态 lint)
   - **`ability` 只作通配读法**(r1/03 P0-7): `ability/<short>` **不在**上述枚举里, 仅当插件既非 `platform`、其 `subkind` 又难以判断时可用; `iface-*`/调度器/框架件/服务分别必须用 `iface/`/`sched/`/`framework/`/`service/`(或其既有裸名), 见 `brickie-v0.1` §8.3 ②
   - **名字与 `api_type` 无关**(r1/03 P0-7): `namespace` 表达的是 `subkind`(或 `app`/`interface`/`platform` 三个 `plugin_type`); 三方件 `service/sqlite` 的 `api_type = third_party` 只写在 `plugin.toml` 字段里, 不进名字(§1 分类学口径)
   - **目录名 = 插件名**: 物理路径最后一段 = `short`, **顶层一段 = `namespace`**(§3 已答; `brickie-v0.1` §8.3 ④)
3. **各类别的布局差异(按 `plugin_type` + `subkind`, A-1)**: 各类别的目录增量 [TODO: `platform`(region 表/早期 console/链接数据)、`ability.subkind=fs`(挂载计划)、`ability.subkind=service`(发布面)、`interface`(`reexport_of`/`symbols` 声明)、`app`(入口)]
4. **物理(二进制)布局映射**: 源码布局 → 产物布局(描述符段/裁剪单位 gc-sections/链接产物) [TODO; 与 3-05 §2.2 对齐]
5. **三方插件形态**: 携带上游源码(lwip/sqlite)的目录形态与"移植增量最小化"(§7.6)的布局支撑 [TODO]
6. **脚手架**: `brickie new <plugin_type> <name> --api <api_type> --lang c [--subkind <s>] [--force]` 的模板清单与生成物; **6 件生成物** = `plugin.toml` / `src/<short>.c` / `include/<short>/<short>.h` / `tests/smoke.toml` / `README.md` / `build/gen/<plugin>/plugin_desc.c`(人写文件不覆盖; `--force` 只对生成物目录生效)

## 3. 开放问题(预留)

| # | 问题 | 状态 |
|---|---|---|
| — | 布局强制程度: 组合器按约定扫描 vs 每插件显式 root 清单 | 待定 |
| ~~—~~ | ~~插件树组织: `plugins/<类别>/<名>`? 顶层即类别(现 §13 示例形态)?~~ | **✅ 已答**: **顶层即 `namespace` 目录**(与 `1-01` §13 CLI 示例形态一致; `2-01` §2 第 4 项的 `plugins/` 读法作废)。**目录名 = 插件名**, `namespace` 集合见 §1/§2 第 2 项与 `brickie-v0.1` §8.3 ④(r1/03 P0-7) |
| ~~—~~ | ~~二进制布局(段名/static 语义)在本篇还是仅留 3-05/3-01~~ | **✅ 已答(A-3)**: 段名三处一致(`.br_plugins`), 宏形态 = `static const` + `used`; 细节归 `3-05` §2 / `3-01` §13.3 |
| — | 插件名保留清单与全局唯一性执法(组合期? CI?) | 待定(唯一性 = 组合期; 新增名的推荐形态 lint = `BRV-TAX-0013` warning) |
| — | conformance 用例在插件目录内的位置(与 6-01-test 的 core 用例分立) | 待定(倾向: 插件目录内 `tests/`, 与 core 用例表分立, 由 6-01 §2 的用例表驱动) |

## 4. DoD 关联

`docs/1-architecture/1-03-roadmap.md` §5 第 5 项(repo 骨架/目录布局)的**单插件侧**; 第 4 项(描述符)的物理布局面。
