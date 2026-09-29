# 13 — 工具链(toolchain)

> 分类: **7-toolchain**。状态: **骨架**。
> 来源: 主文档 §6(组合期校验)、§13(工具链); 治理执法: `docs/1-architecture/01-api-contract-governance.md` §2.3(三层 CI 门禁); DoD: `docs/1-architecture/02-roadmap.md` §5 第 4/5 项。

## 1. 范围与现状

**契约优先于实现** ⇒ 工具链是持久资产(战略语境, 02 §0): 组合器把"插件组合"变成"可启动镜像", CI 门禁把契约变成执法。

已有决策(待深化时继承):
- **CLI `tg`**(主文档 §13): `tg init`/ `tg add <plugin>`(依赖闭包)/ `tg build`(组合+链接)/ `tg run`(QEMU)/ `tg test`(conformance 运行器, 本篇 §2 第 6 项); `tg check` 等其余命令见 §2 大纲 [?]
- **组合期校验**(§6.4): init-DAG 环检测(硬错误)、资源冲突、设备名唯一、版本区间
- **三层 CI 门禁**(01 §2.3): golden diff(abidiff)/ 语义 conformance 矩阵(×3 调度器)/ 版本矩阵
- **host 平台插件**(02): CI 秒级单测 + 完整 ASan 直通
- **golden 文件划分**(08 §1/§15): tg-sched/mem/mm/irq/svc + 框架件四件 + svc-posix

## 2. 大纲(待成文)

1. **manifest 格式定稿**(与 `docs/2-os-core/12-plugin-mgr.md` 共同交付): schema + 校验器
2. `tg` CLI 命令集: add/remove/build/run/check/flash [?]; 参数与配置文件
3. 组合器: 依赖闭包求解、拓扑输出、生成物(manifest.c/h、链接脚本片段、挂载计划表)
4. 构建系统选型与 **repo 骨架**(DoD 第 5 项): 目录布局(plugins/ core/ apps/ tools/)、构建入口
5. golden 生成器: 构建产物符号表(`nm --defined-only`)→ `api/frozen/*.txt` + abidiff 布局比对(01 §2.6.4/08 §13.4: 真值是构建产物; 头文件解析仅用于 D13 模块→符号映射)
6. conformance 运行器: host 平台 + QEMU 目标、三调度器矩阵、报告格式
7. 镜像产物: 布局、符号表、trace id 表(03)随镜像分发

## 3. 开放问题

| # | 问题 |
|---|---|
| — | 构建系统: make/cmake/自研 [?]——倾向最小依赖(make + 脚本; roadmap §5 第 5 项为中性「构建系统选型论证」, 结论在选型论证时定) |
| — | 插件分发形态: v1 源码树内(monorepo); 二进制分发(.a + 头)是 D14 的远期对象 |
| — | `tg run` 的 QEMU 封装参数(virt 机型/内存/设备) |

## 4. DoD 关联

`docs/1-architecture/02-roadmap.md` §5 第 4/5 项: manifest 格式 + 构建系统/repo 骨架——本篇核心交付。
