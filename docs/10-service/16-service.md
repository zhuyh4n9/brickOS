# 16 — 服务层(自研服务与三方移植)

> 分类: **10-service**。状态: **骨架**。
> 来源: 主文档 §7(POSIX 与服务层: D18/依赖宪法/移植双模式)、§11(trace/debug 服务); 版本排期: `docs/1-architecture/02-roadmap.md` 各版本插件清单。

## 1. 范围与现状

Service = 可依赖、可被依赖的**能力插件**(运行时/协议栈/中间件 wrap)。与框架件的区别: 框架件是"能力基础设施的契约"(dev/cdev/bdev/vfs-core), 服务是"用这些契约干活的运行时"。

已有决策(待深化时继承):
- **D18**: `svc-posix` = POSIX 运行时**服务**(fd 表唯一主人; 三方可声明依赖); 接口插件降为薄皮肤
- **依赖宪法**(§7.2): 服务间可依赖(init-DAG 无环); 共享状态单一主人(fd 表 = svc-posix); 符号命名空间独占
- **移植双模式**(§7.6): 模式 A = os_unix.c 直链 svc-posix(~0 工作量); 模式 B = native VFS 后端(~600 行)
- **sqlite 模式 A 移植**(M2) = 移植性验证(战略语境: 移植故事是架构生死线)
- 服务清单: trace(M2)/ dbg-bridge(M3)/ svc-posix(M2)/ lwip(v2 ★)/ crypto(v2)/ ramdump(v2)/ modload+ed25519(v3)

## 2. 大纲(待成文)

1. **svc-posix**: POSIX 子集清单(fd/stdio/pthread; socket 路由 lwip)、libc stub、errno = -ret(零转换)、fd 表实现
2. **socket 路由**(v2): svc-posix ↔ lwip 的服务边界(谁拥有 socket 表 [?])
3. 三方移植指南: 模式 A/B 判据、常见坑(信号/ fork 不存在)、sqlite 案例复盘
4. trace / dbg-bridge 服务的接口面(与 `docs/4-debug/03-debug.md` 分工: 03 定协议, 本篇定服务实现)
5. crypto 服务: mbedTLS wrap、注册表发布(为 v3 鉴权铺路)
6. modload(v3): 动态加载 + ed25519 鉴权流程(D1 修订)
7. 服务编写者契约: init 时序、发布时机、RAM 预算声明

## 3. 开放问题

| # | 问题 |
|---|---|
| **D7** | Service 插件边界判据(与 `docs/2-os-core/17-service-mgmt.md` 共同落定) |
| — | socket 表的主人: svc-posix vs lwip(单一主人规则的应用) |
| — | 三方服务的符号面治理: POSIX 面 golden(tg-svcposix.txt)之外的符号如何约束 |
