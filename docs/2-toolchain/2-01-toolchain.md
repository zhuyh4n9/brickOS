# 2-01 — 工具链(toolchain)

> 章节: **2-toolchain**。状态: **骨架**。
> 来源: 主文档 §6(组合期校验)、§13(工具链); 治理执法: `docs/1-architecture/1-02-api-contract-governance.md` §2.3(三层 CI 门禁); DoD: `docs/1-architecture/1-03-roadmap.md` §5 第 4/5 项。
> 分工: 本篇 = 工具链**总纲**(范围/大纲/开放问题/DoD/与各章的接口); **`br` 工具自身的架构设计**(分层/边界/语言与后端选型/状态与增量/命令面)→ `docs/2-toolchain/2-02-br-arch.md`(**讨论稿**, 待对 BR-D1–BR-D8 拍板)。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **ASan** | AddressSanitizer | 内存错误检测器(host 平台插件上白捡, CI 常开) |
| **CI** | Continuous Integration | 持续集成(三层门禁的落地处) |
| **CLI** | Command-Line Interface | 命令行界面 |
| **DAG** | Directed Acyclic Graph | 有向无环图; init-DAG = 初始化依赖拓扑, 环即组合期硬错误 |
| **DoD** | Definition of Done | 完成定义(此仓库指设计阶段收敛清单) |
| **FUNC / STRUCT / ENUM** | — | golden 记录类型: 函数签名 / 结构布局 / 枚举值(3-01 §13.4) |
| **M0–M5** | — | v1.0/v1.x 内部里程碑(1-03 §3; M5 = HSM 完整样例) |
| **QEMU** | Quick Emulator | 开源模拟器(v1 的主要验证平台) |

> **编号约定**: `§6.4` = 主文档组合期校验六项; `1-02 §2.3` = 三层 CI 门禁; `DoD 第 4/5 项` = 1-03 §5。

## 1. 范围与现状

**契约优先于实现** ⇒ 工具链是持久资产(战略语境, 1-03 §0): 组合器把"插件组合"变成"可启动镜像", CI 门禁把契约变成执法。

已有决策(待深化时继承):
- **CLI `br`**(主文档 §13): `br init`/ `br add <plugin>`(依赖闭包)/ `br build`(组合+链接)/ `br run`(QEMU)/ `br test`(conformance 运行器, 本篇 §2 第 6 项); `br check` 等其余命令见 §2 大纲 [?]
- **组合期校验**(§6.4): init-DAG 环检测(硬错误)、资源冲突、设备名唯一、版本区间
- **三层 CI 门禁**(1-02 §2.3): golden diff(abidiff)/ 语义 conformance 矩阵(×3 调度器)/ 版本矩阵
- **host 平台插件**(1-03): CI 秒级单测 + 完整 ASan 直通
- **golden 文件划分**(3-01 §1/§15): br-sched/mem/mm/irq/svc + 框架件四件 + svc-posix

## 2. 大纲(待成文)

1. **manifest 格式定稿**(语义侧 → `docs/4-plugin/4-03-plugin-manifest.md`, 与其共同交付): schema + 校验器
2. `br` CLI 命令集: add/remove/build/run/check/flash [?]; 参数与配置文件
3. 组合器: 依赖闭包求解、拓扑输出、生成物(manifest.c/h、链接脚本片段、挂载计划表)
4. 构建系统选型与 **repo 骨架**(DoD 第 5 项): 目录布局(plugins/ core/ apps/ tools/)、构建入口
5. golden 生成器: 构建产物符号表(`nm --defined-only`)→ `api/frozen/*.txt` + abidiff 布局比对(1-02 §2.6.4/3-01 §13.4: 真值是构建产物; 头文件解析仅用于 D13 模块→符号映射)
6. conformance 运行器: host 平台 + QEMU 目标、三调度器矩阵、报告格式
7. 镜像产物: 布局、符号表、trace id 表(5-01)随镜像分发

> 第 2/3/5/6 项中属于 **`br` 自身架构**的部分(工具形态/声明面/语言与后端边界/状态与增量/命令面/演进)已在 `docs/2-toolchain/2-02-br-arch.md` 展开为待拍板决策(BR-D1–BR-D8); 本篇保留"做什么"与"与各章的接口"。

## 3. 开放问题

| # | 问题 |
|---|---|
| — | 构建系统: make/cmake/自研 [?]——倾向最小依赖(make + 脚本; roadmap §5 第 5 项为中性「构建系统选型论证」, 结论在选型论证时定) |
| — | 插件分发形态: v1 源码树内(monorepo); 二进制分发(.a + 头)是 D14 的远期对象 |
| — | `br run` 的 QEMU 封装参数(virt 机型/内存/设备); **HSM 样例需要透出 host 对端**(`-chardev socket` + `--hsm-peer=`, 9-02 §7.1 O-H4) |

## 4. DoD 关联

`docs/1-architecture/1-03-roadmap.md` §5 第 4/5 项: manifest 格式 + 构建系统/repo 骨架——本篇核心交付。
