/*
 * brickOS prototype v0.1.0 — 内存映射(MMU / region 表 / cache)契约
 *
 * 权威设计: `docs/3-os-core/3-01-core-api-list.md` §7(br-mm 组, 5 函数)+ §6(页池)
 * + §13.6(特权分级的 memory 三轴: ops × granularity × regions)
 * + `docs/3-os-core/3-04-memory.md`(三池 + region 表 + 虚拟内存政策)。
 *
 * 三层模式(设计 1-01 §8): **接口在 core / 构造在 platform(设计里是 ISA 共享库)/
 * 数据在 platform 的 region 表**。所以本头文件有两半:
 *   ① native 面(§7 的 5 个函数: region_add / map / unmap / cache_flush / cache_invalidate)
 *      —— 设计原样, 签名不改;
 *   ② platform 面(`br_mm_ops_t` + `br_mm_register`, 照 `br_pic_ops` 的同型手法)
 *      —— platform 填"页表构造"这张表; core 只做派发与一致性。
 *
 * v1 的虚拟内存政策 = **恒等映射 + 属性隔离**(3-04 §1): 静态 region 表在 platform
 * early_init 里声明, 页表由 `br_mm_ops.activate()` 一次构造。因此
 *   - `br_mm_map`/`br_mm_unmap` 在 v1 **返回 -ENOTSUP**(签名先行, 实现归 v2 重定位;
 *     用例 `TC-MM-003` 正是这条: "map/unmap(v1) = -ENOTSUP, 主文档风险 R4 的签名先行验证");
 *   - 运行期改属性走 `br_mm_set_attrs()`(→ `ops.set_attrs`)——这是 v2 重定位要用的
 *     同一条机械通路, 在 v1 里由 platform 的一致性用例拿来做 RO 页保护验证。
 */
#ifndef BR_CORE_BR_MM_H
#define BR_CORE_BR_MM_H

#include <br/core/br_types.h>

/* ---------------------------------------------------------------- region 属性
 * 设计 3-01 §7: BR_MM_RO | BR_MM_NX | BR_MM_DEVICE | BR_MM_CACHED(**append-only, D14**)。
 * 下面 4 位是设计既定的; 新增位只许往后加, 不许改语义。
 */
#define BR_MM_RO      0x0001u   /* 只读(写 ⇒ permission fault) */
#define BR_MM_NX      0x0002u   /* 不可执行(取指 ⇒ instruction abort) */
#define BR_MM_DEVICE  0x0004u   /* 设备内存(Device-nGnRnE; 非对齐访问会 fault) */
#define BR_MM_CACHED  0x0008u   /* 可缓存(Normal WB; cache 维护对其有意义) */

/*
 * region **种类**(位 8..11)：把 3-01 §13.6 的 `regions` 轴
 * (heap | dma | contig | page | mmio | reserved)从"声明面"落到"region 表"的可执行形式。
 * 三池划分(3-01 §6 "platform region 表把 RAM 划为 heap / contig 池 / 页池")正是靠它表达：
 * `br_mem_init()` 按种类认领自己的池, 平台只声明"哪块 RAM 是什么池"。
 * ★ 这是原型的**新增**(设计 §13.6 只定义了取值域, 没定义载体), 已在
 *   `docs/decisions/0003-memory-map-heap-and-debug-plugins.md` 登记为增量。
 */
#define BR_MM_KIND_SHIFT   8u
#define BR_MM_KIND_MASK    0x00000F00u
#define BR_MM_KIND_NONE    0u
#define BR_MM_KIND_IMAGE   1u   /* 镜像(text/rodata/data/bss) */
#define BR_MM_KIND_STACK   2u   /* 启动栈 */
#define BR_MM_KIND_HEAP    3u   /* TLSF 堆池(CA-7 之外的字节池) */
#define BR_MM_KIND_CONTIG  4u   /* 物理连续池(CA-7) */
#define BR_MM_KIND_PAGE    5u   /* 页池(CA-8) */
#define BR_MM_KIND_DMA     6u   /* DMA 缓冲池(CA-6: 建于 contig 语义之上) */
#define BR_MM_KIND_MMIO    7u   /* 寄存器窗口(Device) */
#define BR_MM_KIND_RESERVED 8u  /* 保留区(不分配给任何池) */

#define BR_MM_KIND(a)          (((a) & BR_MM_KIND_MASK) >> BR_MM_KIND_SHIFT)
#define BR_MM_WITH_KIND(a, k)  (((a) & ~BR_MM_KIND_MASK) | (((k) << BR_MM_KIND_SHIFT) & BR_MM_KIND_MASK))

/* region 表容量。原型取静态上界(设计 3-04 §2: 静态 region 表; manifest 裁剪归 v1.x)。 */
#define BR_MM_MAX_REGIONS   24u

/* 一条 region 描述(设计 3-01 §7 原样: base/size/attrs) */
typedef struct {
    br_uintptr_t base;
    br_size_t    size;
    br_u32       attrs;     /* BR_MM_RO|NX|DEVICE|CACHED + 种类 */
} br_mm_region_t;

/* ---------------------------------------------------------------- native 面 */

/*
 * platform early_init 声明一条恒等区(设计 3-01 §7 / §10: 归 Platform)。
 * 语义: 纯登记(不碰页表)—— 页表在 `br_mm_ops.activate()` 里按**当时**的表一次构造。
 * 错误: `-EINVAL`(r==NULL / size==0 / base 未页对齐 / size 非页粒度)
 *       `-EEXIST`(与已有 region 重叠或重复 —— 用例 TC-MM-001)
 *       `-ENOSPC`(表满)
 * 激活之后追加的 region 会**顺带**经 `ops.map_region` 落表(表与页表不留第二真值)。
 */
int br_mm_region_add(const br_mm_region_t *r);

/* 运行期映射/改属性(设计 3-01 §7)。v1 签名先行、实现未到 ⇒ 一律 `-ENOTSUP`(TC-MM-003)。 */
int br_mm_map  (const br_mm_region_t *r);
int br_mm_unmap(br_uintptr_t addr, br_size_t size);

/*
 * cache 维护(设计 3-01 §7; 驱动 DMA 前后, 风险 R4)。
 * 语义(按 3-01 §13.6 的正交表, `flush`/`invalidate` 属 P2 且 `granularity` 可为 byte):
 *   - 命中**非缓存**(DEVICE 或未标 CACHED)的 region ⇒ 无操作返回 0(用例 TC-MM-002);
 *   - 未激活 MMU(cache 本就未开)⇒ 无操作返回 0;
 *   - 地址未声明 ⇒ `-EINVAL`(原型刻意不"猜"属性; 见 ADR-0003)。
 */
int br_mm_cache_flush     (void *addr, br_size_t size);
int br_mm_cache_invalidate(void *addr, br_size_t size);

/* ---------------------------------------------------------------- 只读观测面
 * 三个查询函数是**观测契约**(ramdump/dump 插件要枚举 region 表), 不属 3-01 的
 * 48 函数 native 面 —— 已在 ADR-0003 登记为增量(与 3-02 §17.4 对 `br_irq_stats_get`
 * 的处置同型: 消费方是 debug 插件 ⇒ 不能藏在 core 内部)。
 */
br_u32                br_mm_region_count(void);
const br_mm_region_t *br_mm_region_get(br_u32 index);          /* 越界 ⇒ NULL */
const br_mm_region_t *br_mm_region_find(br_uintptr_t addr);    /* 未声明 ⇒ NULL */
br_u32                br_mm_region_count_kind(br_u32 kind);

/* ---------------------------------------------------------------- platform 面
 * 页表构造 ops(照 3-01 §10 的"`br_pic` ops 表(中断控制器实现) | Platform 插件(填表)"
 * 同型): core 定义形状, platform 填实现。
 */
typedef struct br_mm_ops {
    /* 建页表 + 装 TTBR/MAIR/TCR + 开 MMU(SCTLR.M)。regions/count = core 的 region 表快照。 */
    int  (*activate)(const br_mm_region_t *regions, br_u32 count);

    /* 单条 region 落表(激活后 `br_mm_region_add` 会调它; 也供 platform 自用) */
    int  (*map_region)(const br_mm_region_t *r);

    /* 改一段已映射区间的属性(重定位与保护验证共用的一条通路) */
    int  (*set_attrs)(br_uintptr_t addr, br_size_t size, br_u32 attrs);

    /* 读回一段区间当前生效的属性(**页表遍历**, 不是查 region 表) */
    int  (*query)(br_uintptr_t addr, br_size_t size, br_u32 *out_attrs);

    void (*cache_flush)(void *addr, br_size_t size);
    void (*cache_invalidate)(void *addr, br_size_t size);
} br_mm_ops_t;

/* platform early_init 注册一次(重复注册 ⇒ 后者被拒, 返回 -EBUSY)。 */
int br_mm_register(const br_mm_ops_t *ops);

/*
 * 建表并开 MMU(由 platform 在 region 表声明完、`br_mem_init()` 之后调用)。
 * 语义: 把**当前**的 region 表快照交给 `ops.activate()`; 成功(返回 0)后 core 记
 * "已激活", `br_mm_active()` 自此为真。
 * 错误: `-ENOTSUP`(未注册 ops)/ `-EBUSY`(已激活)/ `-ENOMEM`(页表内存不足)/
 *       其它 = ops 的返回值原样上传。
 * 为什么不把"激活"放进 ops: 那是**状态机**(未注册→已注册→已激活)的归属问题 ——
 * 状态归 core, 构造归 platform(三层模式的边界)。
 */
int br_mm_activate(void);

/* MMU 是否已由 `br_mm_activate()` 打开。 */
br_bool br_mm_active(void);

/* 以上 ops 的 core 侧派发(未注册/未激活 ⇒ `-ENOTSUP`)。 */
int br_mm_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs);
int br_mm_query    (br_uintptr_t addr, br_size_t size, br_u32 *out_attrs);

#endif /* BR_CORE_BR_MM_H */
