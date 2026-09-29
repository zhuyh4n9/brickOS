# 18 — 插件开发指南(plugin development)

> 分类: **3-plugin-dev**。状态: **骨架**(横切入口; 各类纵深契约在领域文档)。
> **本类存在的理由**: 所有功能扩展均依赖插件——驱动、文件系统、服务、接口、平台、调度器、**框架件**、APP 全部是插件。八类各有契约, 但**通用流程与纪律是共享的**——本篇是所有插件作者的入口。

## 1. 通用开发流程(所有插件)

```
manifest 声明(依赖 / 资源 / RAM 预算)
→ 描述符 TG_PLUGIN(...)(12-plugin-mgr)
→ 实现 init(选相位)+ ops 表(类别契约)
→ conformance 套件通过(01 §2.3 层 2)
→ golden 面合规(01 层 1)+ 版本矩阵(层 3)
```

- **依赖声明**: 指向插件名 + 版本区间; init-DAG 无环(§7.2, 环 = 组合期硬错误)
- **资源声明**: IRQ / DMA 通道 / 引脚 / RAM 预算(§6.4 组合期冲突检测); 设备名唯一(注册表主键)
- **符号纪律**: `tg_*` 前缀 core 保留(§7.2 命名空间独占); 插件自有符号带插件名前缀
- **ISR 纪律**(如涉及中断): 白名单四件之外全部 thread-only(08 §11)

## 2. 八类插件的作者视角(路由表)

| 类别 | 你提供 | 依赖 | 纵深契约 | 验证 |
|---|---|---|---|---|
| **Platform** | PIC ops 表 / 早期 console / region / 链接脚本 | — | 主文档 §8(三层模式) | 真硬件 M4 |
| **Scheduler**(恰一) | `tg_sched_ops` + sched_class 声明 | core | `10-sched` | 三形态 conformance 矩阵 |
| **框架件** | 能力契约 API(注册表/ops 形状) | core / 框架件间单向(cdev-core→dev-core 与 vfs-core, bdev-core→dev-core) | `04`–`07`(04 §2/06 §2–3) | golden + conformance |
| **IO(驱动)** | 设备 ops 表(cdev/bdev/flash) | cdev-core / bdev-core | `06-device` §3/§6 | ISR/DMA 静态扫描 |
| **FS** | ops 四层(super/inode/file/dentry) | vfs-core(挂载) | `04-vfs` §2 / `07` | 掉电用例 |
| **Service** | init + 运行时 + 注册表发布 | 服务 / 框架件 | `17` / `16` | 单一主人审查 |
| **Interface** | 再导出皮肤 | svc-posix 等服务或 core(iface-min 直通) | `15-interface` | 叶子检查(无被依赖) |
| **APP**(恰一) | main | Interface(或直调 native) | `14-app` | 启动链演示 |

(编号为全局文档号: 04–07 在 `6-vfs-device`, 10–12/17 在 `2-os-core`, 14 在 `8-app`, 15 在 `9-interface`, 16 在 `10-service`。)

## 3. 驱动作者契约(IO)—— 摘要

来自 `06-device` §6, 全量以彼为准:

| 纪律 | 内容 | 执法 |
|---|---|---|
| ISR 纪律 | ISR 最小工作 → `tg_work_submit`; 禁阻塞/malloc/持锁跨 ISR 返回 | conformance + 评审清单 |
| DMA/cache | `tg_dma_alloc` 分配; 传输前后 `tg_mm_cache_flush/invalidate`(R4) | conformance + 真硬件 M4 |
| 资源声明 | manifest: IRQ/通道/引脚/RAM | 组合期冲突检测 |

- 驱动代码**板级无关**(平台差异全在 Platform 插件数据); 换板只换 Platform
- ops 槽位: NULL = 不支持(-ENOTSUP); ioctl/suspend/resume 全类别预留(D22)

## 4. FS 作者契约 —— 摘要

来自 `04-vfs` §2 / `07-concrete-fs`, 全量以彼为准:

- 实现 **ops 四层**: `tg_fs_ops`(mount→根 inode/free_inode/sync)+ 目录 `tg_inode_ops`(lookup 链)+ 文件 `tg_file_ops`(open 建会话)+ `tg_dentry_ops`(v1 全 NULL 预留)
- inode = vfs-core 公共头 + FS 私有尾; **v1 瞬态**(无缓存, SD-3)
- 挂载经 manifest 挂载计划(07 §6); 挂载点父 FS 依赖声明
- 可写 FS 的掉电语义是硬要求(littlefs 型: COW + 元数据对)

## 5. 服务作者契约 —— 摘要

来自 `17` / `16`, 全量以彼为准:

- init 时序: 被依赖方(服务方)publish 在先、依赖方(消费方)lookup 在后(init-DAG 保证; 17-service-mgmt §1)
- **共享状态单一主人**(§7.2): 跨服务共享表必须有唯一 owner(fd 表判例)
- RAM 预算 manifest 声明(arena 记账, v2)
- 三方移植先读 §7.6 双模式(模式 A 直链 svc-posix)

## 6. 测试与合规(所有插件)

| 层 | 内容 | 文档 |
|---|---|---|
| 语义 conformance | 行为测试跑在 host 平台(秒级)+ 目标 | 01 §2.3 层 2 / **用例目录: `docs/5-test/19-test.md`** |
| 静态扫描 | ISR 白名单(CA-3)/ DMA 纪律(主文档 R4)/ 符号命名(D13/CA-10) | 静态分析 = D10 双保险之一; conformance 矩阵 = R1 执法 |
| golden 面 | API 面冻结合规(abidiff) | 01 层 1 |
| 版本矩阵 | `requires: core@>=x.y` × 声称版本全编 | 01 层 3 |

## 7. 开放问题

| # | 问题 |
|---|---|
| — | 插件作者脚手架: `tg new <kind>` 生成模板(manifest + 描述符 + conformance 骨架)[?] |
| — | conformance 用例编写指南(断言集形态、host 适配层) |
| — | 每类插件的样例参考实现盘点(v1.0 已有: uart-pl011/virtio-blk/littlefs/trace) |
