# 3-06 — 服务管理(service management)

> 章节: **3-os-core**。状态: **骨架**。
> 来源: 主文档 §4.5(**服务注册表 = core 公地**)、§7.2(依赖宪法); 服务层实现(怎么写一个服务)→ `docs/11-service/11-01-service.md`; 注册表 API 已定 → `docs/3-os-core/3-01-core-api-list.md` §9。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **API** | Application Programming Interface | 应用程序接口 |
| **DAG** | Directed Acyclic Graph | 有向无环图; init-DAG = 初始化依赖拓扑, 环即组合期硬错误 |
| **errno(E*)** | error number | POSIX 错误码; core 以**负值**返回 `-EINVAL`/`-EAGAIN`/`-ETIMEDOUT`/`-ENOTSUP`/`-EBUSY`/`-EEXIST`/`-EIO`/`-ENODEV`/`-ENOMEM`/`-ENOSPC` 等, 域用子集 |
| **fd** | File Descriptor | 文件描述符(runtime/posix 的 fd 表 = 共享状态单一主人判例) |
| **MPSC** | Multi-Producer Single-Consumer | 多生产者单消费者(无锁队列形态) |
| **POSIX** | Portable Operating System Interface | 可移植操作系统接口 |

> **编号约定**: `D7`/`D18` = 全局决策; `1-02 §2.3 层 3` = 版本矩阵门禁。

## 1. 范围与现状

core 的**服务注册表**(名字 → 指针)是插件间**无环会合点**——依赖宪法(§7.2)允许服务间依赖, 会合就发生在注册表。core 只管名字与指针, **不解释类型**(类型契约归服务方文档化)。

已有决策(待深化时继承):
- **注册表 API**(3-01 §9): `br_service_publish`(重复 → -EEXIST)/ `br_service_lookup`(NULL = 无)
- **依赖宪法**(§7.2): 服务间可依赖(init-DAG 无环)、**共享状态单一主人**(fd 表)、符号命名空间独占
  - **fd 表主人的归属(2026-xx 收口)**: D18 把 fd 表记在 `runtime/posix` 名下; 原型落地时按 D19("能力框架 = 插件身份")把它抽成 **`framework/file-table`** 框架件, `runtime/posix` 退为**消费者**。规则 3 要保的是"**恰一个主人**"(全树只有 `file-table` 写那张表), 而不是"主人必须是那个服务" —— 见 `1-03` §1 的插件清单与原型 ADR-0012/0014。
- **D18**: runtime/posix = 服务(三方中间件可声明依赖); 接口插件 = 严格叶子
- 发布时机 = 服务方 init; 查找时机 = 依赖方 init(init-DAG 保证顺序)

## 2. 大纲(待成文)

1. 注册表语义: 名字空间规则(`[a-z][a-z0-9-]*` [?])、发布/查找时序、线程安全(发布期单线程)
2. **服务 vs 框架件判据(D7 落点)**: 能力基础设施契约(框架件, 被 core 纪律约束)vs 用契约干活的运行时(服务)——判据成文
3. 共享表主人判例: fd 表(**`framework/file-table`**; `runtime/posix` 是消费者 —— 见上)、socket 表(v2 [?])、trace 环(主人 = trace 服务, 多生产者经 MPSC 无锁写入——所有权与访问协议正交)
4. 服务条目版本化: 注册时声明版本? `requires` 语义与 1-02 §2.3 层 3 的关系
5. 动态服务(v3): modload 加载的服务如何入表/退表

## 3. 开放问题

| # | 问题 |
|---|---|
| **D7** | **Service 插件边界判据**(与 `docs/11-service/11-01-service.md` 共同落定)——最后未定的决策之一 |
| — | socket 表的主人: runtime/posix vs lwip(单一主人规则的应用) |
| — | 服务符号面治理: POSIX 面 golden 之外的符号如何约束 |

## 4. DoD 关联

D7 落定的设计载体之一(另一半在 11-01-service)。
