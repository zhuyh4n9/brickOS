# 3-04 — 内存管理(memory)

> 章节: **3-os-core**。状态: **骨架**。
> 来源: 主文档 §4.3(内存)、§9(启动: 页表/region); API: `docs/3-os-core/3-01-core-api-list.md` §6/§7; debug 侧: `docs/5-debug/5-01-debug.md` §4(memleak/ASan)。

## 缩略词(abbreviations)

| 缩略词 | 英文全称 | 说明 |
|---|---|---|
| **API** | Application Programming Interface | 应用程序接口 |
| **CA-1–CA-10** | — | core native API 契约决策编号(3-01 §14) |
| **DMA** | Direct Memory Access | 直接内存访问(外设不经 CPU 读写内存) |
| **EARLY / CORE / LATE / APP** | — | 插件生命周期四相(主文档 §6.2): 关中断无线程 → 堆可用 → 服务/接口 → 进 APP main |
| **ISA** | Instruction Set Architecture | 指令集架构 |
| **memleak** | memory leak | 内存泄漏(per-plugin arena 记账, v2.0) |
| **MMU** | Memory Management Unit | 内存管理单元 |
| **MPU** | Memory Protection Unit | 内存保护单元(无 MMU 目标的 region 保护) |
| **OOM** | Out Of Memory | 内存耗尽 |
| **R1–R10** | — | 主文档 §16 的风险编号 |
| **BR_MM_RO / NX / DEVICE / CACHED** | — | region 属性: 只读 / 不可执行 / 设备内存 / 可缓存(append-only, D14) |
| **TLSF** | Two-Level Segregated Fit | O(1) 动态内存分配算法(此仓库的堆池实现) |
| **VA** | Virtual Address | 虚拟地址 |

> **编号约定**: `CA-6`–`CA-8` = 相关契约决策; `R4` = 风险(DMA/cache 签名); `D15` = 冻结节奏。

## 1. 范围与现状

core 提供**三个池(TLSF 堆 / contig 池 / 页池, CA-7/CA-8)+ MMU 接口 + DMA 约定**; 虚拟内存政策 = v1 恒等映射(属性隔离)/ v2 重定位 / vx MPU(v0.4 评审反转后的政策, 主文档 §4.3)。

已有决策(待深化时继承):
- **TLSF 堆**: `br_malloc/calloc/realloc/free`(3-01 §6); `sbrk` 挂接 svc-posix libc stub
- **per-plugin arena**: v1 统计(`br_heap_usage`), v2 归属分配 + memleak 记账(5-01 §4)
- **DMA**(CA-6): `br_dma_buf_t` 含 dma_addr, v1 恒等下 == vaddr(为 v2 重定位预留形状)
- **br_mm**: region 表(静态)、map/unmap 签名 v1 起定稿(R4; 升格 frozen 走 D15)、cache 维护(`br_mm_cache_flush/invalidate`, 驱动 DMA 前后)
- **region 属性**: `BR_MM_RO/NX/DEVICE/CACHED`(append-only, D14)

## 2. 大纲(待成文)

1. 布局: 镜像/链接脚本(`.br_*` 段)、region 表(platform early_init 声明, 1-01 §9 启动序列)
2. TLSF 堆: 初始化(启动早期, §9)、碎片策略、对齐保证
3. arena 模型: per-plugin 预算(manifest)、归属记账(v2)、OOM 策略 [?]
4. DMA 区: `br_dma_alloc` 属性(对齐/一致性)、与驱动契约(8-01 §6 三纪律)
5. cache 一致性协议: 谁维护(驱动, R4)、`br_mm_cache_*` 的 aarch64 实现(ISA 库)
6. 重定位(v2): 镜像加载任意 VA、重定位表、与 A/B 更新的衔接
7. MPU(vx): 无 MMU 目标的 region 替代(实验)

## 3. 开放问题

| # | 问题 |
|---|---|
| R4 | DMA/cache API 签名 M2 起定稿(升格 frozen 走 D15)——实现细节(aarch64 ISA 库归属)待成文 |
| — | 堆的启动时序: 需要在插件 init 前可用 ⇒ 初始化点 = core.init(主文档 §9: 早于 EARLY 相) |
| — | v2 重定位的技术选型(重定位表 vs 位置无关) |

## 4. DoD 关联

`docs/1-architecture/1-03-roadmap.md` §5 第 2 项(D7/D8 之外的收敛项): native API 头文件级规格的内存部分已在 3-01; 本篇承接实现设计。
