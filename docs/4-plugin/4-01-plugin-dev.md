# 4-01 — 插件开发指南(plugin development)

> 章节: **4-plugin**。状态: **骨架**(横切入口; 各类纵深契约在领域文档; 布局/manifest/依赖语义 → 本类 4-02/4-03/4-04, 预留待细化)。
> **本类存在的理由**: 所有功能扩展均依赖插件——驱动、文件系统、服务、接口、平台、调度器、**框架件**、APP 全部是插件。八类各有契约, 但**通用流程与纪律是共享的**——本篇是所有插件作者的入口。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **API** | Application Programming Interface | 应用程序接口 |
| **APP** | Application | 应用(插件类别: 唯一业务逻辑, 恰一个, 不被任何插件依赖) |
| **CA-1–CA-10** | — | core native API 契约决策编号(3-01 §14) |
| **COW** | Copy-On-Write | 写时复制(掉电安全 FS 的提交方式) |
| **DAG** | Directed Acyclic Graph | 有向无环图; init-DAG = 初始化依赖拓扑, 环即组合期硬错误 |
| **DMA** | Direct Memory Access | 直接内存访问(外设不经 CPU 读写内存) |
| **errno(E*)** | error number | POSIX 错误码; core 以**负值**返回 `-EINVAL`/`-EAGAIN`/`-ETIMEDOUT`/`-ENOTSUP`/`-EBUSY`/`-EEXIST`/`-EIO`/`-ENODEV`/`-ENOMEM`/`-ENOSPC` 等, 域用子集 |
| **FS** | File System | 文件系统(插件类别之一) |
| **I/O** | Input/Output | 输入输出; 插件类别 **IO** = 外设驱动 |
| **IRQ** | Interrupt Request | 中断请求(硬件中断线) |
| **ISR** | Interrupt Service Routine | 中断服务例程(中断上下文中的处理函数) |
| **PIC** | Programmable Interrupt Controller | 可编程中断控制器(实现归 Platform 插件) |
| **R1–R10** | — | 主文档 §16 的风险编号 |
| **RAM** | Random Access Memory | 随机访问存储器 |
| **SD-1–SD-15** | — | 存储与设备域决策编号(7-01/7-02/8-01 §决策记录) |

> **编号约定**: `D13`/`D22` = 全局决策; `CA-3`/`CA-10` = ISR 白名单 / 符号导出; `SD-3` = 瞬态 inode; `R4` = 风险。

## 1. 通用开发流程(所有插件)

```
manifest 声明(依赖 / 资源 / RAM 预算)
→ 描述符 BR_PLUGIN(...)(3-05-plugin-mgr)
→ 实现 init(选相位)+ ops 表(类别契约)
→ conformance 套件通过(1-02 §2.3 层 2)
→ golden 面合规(1-02 层 1)+ 版本矩阵(层 3)
```

> 深化落点(本类, 预留待细化): 布局 → `4-02-plugin-layout.md`; manifest → `4-03-plugin-manifest.md`; 依赖 → `4-04-plugin-deps.md`。

- **依赖声明**: 指向插件名 + 版本区间; init-DAG 无环(§7.2, 环 = 组合期硬错误)
- **资源声明**: IRQ / DMA 通道 / 引脚 / RAM 预算(§6.4 组合期冲突检测); 设备名唯一(注册表主键)
- **符号纪律**: `br_*` 前缀 core 保留(§7.2 命名空间独占); 插件自有符号带插件名前缀
- **ISR 纪律**(如涉及中断): 白名单四件之外全部 thread-only(3-01 §11)

## 2. 八类插件的作者视角(路由表)

| 类别 | 你提供 | 依赖 | 纵深契约 | 验证 |
|---|---|---|---|---|
| **Platform** | PIC ops 表 / 早期 console / region / 链接脚本 | — | 主文档 §8(三层模式) | 真硬件 M4 |
| **Scheduler**(恰一) | `br_sched_ops` + sched_class 声明 | core | `3-03-sched` | 三形态 conformance 矩阵 |
| **框架件** | 能力契约 API(注册表/ops 形状) | core / 框架件间单向(cdev-core→dev-core 与 vfs-core, bdev-core→dev-core) | `7-01`–`7-03` + `8-01`(7-01 §2 / 7-02 §1 / 8-01 §2–3) | golden + conformance |
| **IO(驱动)** | 设备 ops 表(cdev/bdev/flash) | cdev-core / bdev-core | `8-01-device` §3/§6 | ISR/DMA 静态扫描 |
| **FS** | ops 四层(super/inode/file/dentry) | vfs-core(挂载) | `7-01-vfs` §2 / `7-03` | 掉电用例 |
| **Service** | init + 运行时 + 注册表发布 | 服务 / 框架件 | `3-06` / `11-01` | 单一主人审查 |
| **Interface** | 再导出皮肤 | svc-posix 等服务或 core(iface-min 直通) | `10-01-interface` | 叶子检查(无被依赖) |
| **APP**(恰一) | main | Interface(或直调 native) | `9-01-app` | 启动链演示 |

(文档编号: 存储域 7-01–7-03 在 `7-storage`, 设备域 8-01 在 `8-device`, 3-03–3-05/3-06 在 `3-os-core`, 9-01 在 `9-app`, 10-01 在 `10-interface`, 11-01 在 `11-service`。)

## 3. 驱动作者契约(IO)—— 摘要

来自 `8-01-device` §6, 全量以彼为准:

| 纪律 | 内容 | 执法 |
|---|---|---|
| ISR 纪律 | ISR 最小工作 → `br_work_submit`; 禁阻塞/malloc/持锁跨 ISR 返回 | conformance + 评审清单 |
| DMA/cache | `br_dma_alloc` 分配; 传输前后 `br_mm_cache_flush/invalidate`(R4) | conformance + 真硬件 M4 |
| 资源声明 | manifest: IRQ/通道/引脚/RAM | 组合期冲突检测 |

- 驱动代码**板级无关**(平台差异全在 Platform 插件数据); 换板只换 Platform
- ops 槽位: NULL = 不支持(-ENOTSUP); ioctl/suspend/resume 全类别预留(D22)

## 4. FS 作者契约 —— 摘要

来自 `7-01-vfs` §2 / `7-03-concrete-fs`, 全量以彼为准:

- 实现 **ops 四层**: `br_fs_ops`(mount→根 inode/free_inode/sync)+ 目录 `br_inode_ops`(lookup 链)+ 文件 `br_file_ops`(open 建会话)+ `br_dentry_ops`(v1 全 NULL 预留)
- inode = vfs-core 公共头 + FS 私有尾; **v1 瞬态**(无缓存, SD-3)
- 挂载经 manifest 挂载计划(7-03 §6); 挂载点父 FS 依赖声明
- 可写 FS 的掉电语义是硬要求(littlefs 型: COW + 元数据对)

## 5. 服务作者契约 —— 摘要

来自 `3-06` / `11-01`, 全量以彼为准:

- init 时序: 被依赖方(服务方)publish 在先、依赖方(消费方)lookup 在后(init-DAG 保证; 3-06-service-mgmt §1)
- **共享状态单一主人**(§7.2): 跨服务共享表必须有唯一 owner(fd 表判例)
- RAM 预算 manifest 声明(arena 记账, v2)
- 三方移植先读 §7.6 双模式(模式 A 直链 svc-posix)

## 6. 测试与合规(所有插件)

| 层 | 内容 | 文档 |
|---|---|---|
| 语义 conformance | 行为测试跑在 host 平台(秒级)+ 目标 | 1-02 §2.3 层 2 / **用例目录: `docs/6-test/6-01-test.md`** |
| 静态扫描 | ISR 白名单(CA-3)/ DMA 纪律(主文档 R4)/ 符号命名(D13/CA-10) | 静态分析 = D10 双保险之一; conformance 矩阵 = R1 执法 |
| golden 面 | API 面冻结合规(abidiff) | 1-02 层 1 |
| 版本矩阵 | `requires: core@>=x.y` × 声称版本全编 | 1-02 层 3 |

## 7. 开放问题

| # | 问题 |
|---|---|
| — | 插件作者脚手架: `brickie new <kind>` 生成模板(manifest + 描述符 + conformance 骨架)[?]——落点: `4-02-plugin-layout.md` §2 第 6 项 |
| — | conformance 用例编写指南(断言集形态、host 适配层) |
| — | 每类插件的样例参考实现盘点(v1.0 已有: uart-pl011/virtio-blk/littlefs/trace) |
