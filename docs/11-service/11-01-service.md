# 11-01 — 服务层(自研服务与三方移植)

> 章节: **11-service**。状态: **骨架**。
> 来源: 主文档 §7(POSIX 与服务层: D18/依赖宪法/移植双模式)、§11(trace/debug 服务); §1 **D24–D26**(HSM 完整样例的服务组); 版本排期: `docs/1-architecture/1-03-roadmap.md` 各版本插件清单; 样例纵深: `docs/9-app/9-02-hsm-sample.md`。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **DAG** | Directed Acyclic Graph | 有向无环图; init-DAG = 初始化依赖拓扑, 环即组合期硬错误 |
| **ed25519** | Edwards-curve Digital Signature Algorithm (25519) | 签名算法(v2 crypto 后端交付; v3 动态加载鉴权) |
| **fd** | File Descriptor | 文件描述符(runtime/posix 的 fd 表 = 共享状态单一主人判例) |
| **HSM** | Hardware Security Module | 硬件安全模块(第二产品域; v1.x/M5 完整样例 = crypto/keyring/hsm-host/seclog 的服务侧消费者) |
| **lwIP** | lightweight IP | 轻量 TCP/IP 协议栈(v2.0 服务) |
| **mbedTLS** | — | 嵌入式 TLS/密码库(crypto 服务的算法实现来源; v1.x 仅取算法子集) |
| **PKCS#11** | Public-Key Cryptography Standards #11 | 密码 token 接口标准(iface-pkcs11 的语义来源) |
| **POSIX** | Portable Operating System Interface | 可移植操作系统接口 |
| **RAM** | Random Access Memory | 随机访问存储器 |
| **SHA-256** | Secure Hash Algorithm 256 | 摘要算法(crypto v1.x 子集; 亦用于 seclog 摘要链) |
| **sqlite** | — | 嵌入式关系数据库(M2 移植性验证件, 模式 A) |
| **TRNG** | True Random Number Generator | 真随机数发生器(平台熵源契约缺口: 9-02 §6.3 O-H1) |
| **VFS** | Virtual File System | 虚拟文件系统(统一可打开模型 + 挂载表) |

> **编号约定**: `D1`/`D7`/`D18` = 全局决策; `§7.2`/`§7.6` = 依赖宪法 / 移植双模式; 模式 A/B = 三方移植的两种接法。

## 1. 范围与现状

Service = 可依赖、可被依赖的**能力插件**(运行时/协议栈/中间件 wrap)。与框架件的区别: 框架件是"能力基础设施的契约"(dev/cdev/bdev/vfs-core), 服务是"用这些契约干活的运行时"。

已有决策(待深化时继承):
- **D18**: `runtime/posix` = POSIX 运行时**服务**(三方可声明依赖); 接口插件降为薄皮肤。★ **fd 表的归属已收口**: 表本身是 **`framework/file-table`** 框架件(原型 ADR-0012, 按 D19 抽件), `runtime/posix` 是它的**消费者** —— 单一主人规则不变(全树只有 `file-table` 写表)
- **依赖宪法**(§7.2): 服务间可依赖(init-DAG 无环); 共享状态单一主人(fd 表 = `framework/file-table`); 符号命名空间独占
- **移植双模式**(§7.6): 模式 A = os_unix.c 直链 runtime/posix(~0 工作量); 模式 B = native VFS 后端(~600 行)
- **sqlite 模式 A 移植**(M2) = 移植性验证(战略语境: 移植故事是架构生死线)
- 服务清单: trace(M2)/ dbg-bridge(M3)/ runtime/posix(M2, **运行时**而非普通服务 —— 见 `brickie-v0.1` §8.3 ①)/ **crypto(M5 契约, D25; v2 后端扩展 ★)**/ **keyring(M5)**/ **hsm-host(M5)**/ **seclog(M5)**/ lwip(v2 ★)/ ramdump(v2)/ modload(v3)
- **HSM 服务组(v1.x/M5, D24)**: `crypto`(密码运算) · `keyring`(**密钥资产唯一主人**, 对外只给不透明 handle) · `hsm-host`(host 协议面) · `seclog`(只追加 + 摘要链审计)——四者的职责切分与命令集见 `docs/9-app/9-02-hsm-sample.md` §3–§8
- **两个服务接口面的分类(分类不变量; A-11)**: `service/crypto` 与 `service/keyring` 的接口面分类为 **`runtime_adapter`** —— 因为 `iface-pkcs11` 是 `api_type = runtime_adapter` 的再导出皮肤, 被再导出单元的 `api_iface` 必须与皮肤自身相等(硬不变量, `brickie` v0.1 §3.5; 皮肤取法见 `1-01` §7.3/§7.4)

## 2. 大纲(待成文)

1. **runtime/posix**: POSIX 子集清单(fd/stdio/pthread; socket 路由 lwip)、libc stub、errno = -ret(零转换)、fd 表实现
2. **socket 路由**(v2): runtime/posix ↔ lwip 的服务边界(谁拥有 socket 表 [?])
3. 三方移植指南: 模式 A/B 判据、常见坑(信号/ fork 不存在)、sqlite 案例复盘
4. trace / dbg-bridge 服务的接口面(与 `docs/5-debug/5-01-debug.md` 分工: 5-01 定协议, 本篇定服务实现)
5. **crypto 服务(D25)**: v1.x/M5 = **服务契约一次定稿** + mbedTLS **算法子集**(SHA-256 / HMAC-SHA256 / AES-CBC/CTR / DRBG, 不含 ed25519/TLS); v2.0 = 完整算法集 + 恒定时间加固 + 硬件引擎后端(**契约不变**); **算法实现一律引上游, 不自行实现密码原语**; 注册表发布(为 v3 鉴权铺路)
6. **keyring 服务(M5)**: 密钥槽位/生命周期(状态机见 9-02 §5.1)/用途位与计数上限/加密落盘 `/data/keys`; **唯一主人**规则的应用(密钥材料只在 keyring 私有区与 crypto 栈内)
7. **hsm-host 服务(M5)**: host 帧协议(COBS+CRC16)、命令分派与结果回填、逐条审计; host 面与策略面分离(协议在服务, 策略在 `app/hsm`)
8. **seclog 服务(M5)**: 只追加 + SHA-256 摘要链, 写穿 `/data/seclog`; 与 trace 的分工 = **审计(不可关闭)** vs 观测(可整层移除)
9. modload(v3): 动态加载 + ed25519 鉴权流程(D1 修订)
10. 服务编写者契约: init 时序、发布时机、RAM 预算声明

## 3. 开放问题

| # | 问题 |
|---|---|
| **D7** | Service 插件边界判据(与 `docs/3-os-core/3-06-service-mgmt.md` 共同落定) |
| — | socket 表的主人: runtime/posix vs lwip(单一主人规则的应用) |
| — | 三方服务的符号面治理: POSIX 面 golden(br-posix.txt)之外的符号如何约束 |
| ~~—~~ | ~~**crypto/keyring 的 ops 表是否入 golden**(与四件框架件同级)——倾向入(v2 换后端要求布局稳定), 见 9-02 §6.2 O-H7~~ ⇒ **✅ 已关闭(2026-xx)**: **入 golden** —— `api/frozen/br-crypto.txt` / `br-keyring.txt`, 冻结批次 = `3-os-core/3-01-core-api-list.md` §15 **第六批**(M5 随服务契约); 理由: v2 换后端/加算法面不得破坏消费者契约(1-02 §2.6), 而"布局稳定"只有 golden 能强制。v1.x 两服务 API 标 `EXPERIMENTAL`, 随 M5 服务契约定稿升格 |
| — | **平台熵源契约**(平台提供 / crypto 消费)缺失, 建议 M4 真实 SoC 设计时收口——9-02 §6.3 O-H1 |
