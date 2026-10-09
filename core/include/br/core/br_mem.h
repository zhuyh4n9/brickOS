/*
 * brickOS prototype v0.1.0 — 内存(mem)契约: TLSF 堆 / 连续池 / 页池 / DMA
 *
 * 权威设计: `docs/3-os-core/3-01-core-api-list.md` §6(br-mem 组, **11 函数**)
 *   + CA-7(连续内存 = 独立 contig 池, 不走 TLSF)/ CA-8(页分配器 = 静态位图 + 首次适配)
 *   + `docs/3-os-core/3-04-memory.md`(三池由 platform region 表划分;
 *     "堆的启动时序: 初始化点 = core.init"「3-04 §3」)
 *   + `docs/5-debug/5-01-debug.md` §4(红区/毒化/金丝雀 = v1.x 便宜替代;
 *     per-plugin arena 记账 = v2, 本原型以"owner 标签"近似)。
 *
 * 三池从哪来: **core 不拥有 RAM, 只认领 region**(3-01 §6 原文: "platform region 表把
 * RAM 划为 heap / contig 池 / 页池, 比例 = manifest 预算")。所以 `br_mem_init()` 的输入
 * 是 `br_mm.h` 的 region 表: 按 `BR_MM_KIND_HEAP/CONTIG/PAGE/DMA` 认领, 各建一个池。
 *
 * 本头文件的三类内容(CA-5 的账要算清, 详见 ADR-0003):
 *   ① **设计既定的 11 函数**(native 面, 函数名/签名与 3-01 §6 逐字一致):
 *      malloc 族 4 + contig 2 + page 2 + dma 2 + heap_usage 1;
 *   ② **观测契约**(memleak 插件消费; 消费方是插件 ⇒ 不能藏在 core 内部 ——
 *      与 3-02 §17.4 处置 `br_irq_stats_get` 同型): stats / walk / check / 池布局;
 *   ③ **记账契约**(5-01 §4 的"per-plugin arena 记账"的 v1.x 可落地形态):
 *      owner 标签注册 + 当前归属切换。
 */
#ifndef BR_CORE_BR_MEM_H
#define BR_CORE_BR_MEM_H

#include <br/core/br_types.h>

/* ---------------------------------------------------------------- 常量 */
/* 堆返回块的**最小对齐**。设计 3-01 §6 未给值; 取 16 B 的裁定理由(ADR-0003):
 * AAPCS64 要求 SP 16 对齐 ⇒ 结构体/栈上对象的自然对齐上界是 16; TLSF 原生按 4/8
 * 对齐会做出"malloc 出来的 buffer 不能放 __attribute__((aligned(16))) 对象"的坑。
 * 用例 TC-MEM-001 断言 `>= BR_MALLOC_ALIGN`。 */
#define BR_MALLOC_ALIGN   16u

/* 页粒度(设计 3-01 §6: **平台编译期常量**)。本原型固定 4 KiB(CA-8 的 4K/16K/64K
 * 里取 4K: QEMU virt + aarch64 最常见, 且页池/页表/L3 描述符口径一致)。
 * 平台在 br_mm.h 的 region 表里必须按这个粒度切池。 */
#define BR_PAGE_SIZE      4096u

/* br_page_alloc 的 attrs(3-01 §6 `BR_PAGE_F_*`; append-only) */
#define BR_PAGE_F_ZERO    0x0001u   /* 分配的页必须全零 */
#define BR_PAGE_F_CONTIG  0x0002u   /* 必须物理连续 —— 本池恒真(v1 恒等映射), 置位不报错 */

/* br_dma_alloc 的 attrs(3-01 §6 `BR_DMA_F_*`; append-only) */
#define BR_DMA_F_ZERO     0x0001u   /* 缓冲全零 */
#define BR_DMA_F_ALIGN4K  0x0002u   /* 4 KiB 对齐 */
#define BR_DMA_F_CACHED   0x0004u   /* 可缓存(默认非缓存 = 设备侧一致性的保守选择) */

/* ---------------------------------------------------------------- ① native 面 */
/* ---- 字节堆(TLSF) ---- */
void *br_malloc (br_size_t n);
void *br_calloc (br_size_t n, br_size_t size);
void *br_realloc(void *p, br_size_t n);     /* runtime/posix 的 `realloc` 直接转调它(ADR-0014) */
void  br_free   (void *p);

/* ---- 物理连续内存(CA-7): 独立 contig 池, 不走 TLSF 堆 ---- */
int  br_mem_alloc_contig(br_size_t size, br_size_t align, void **out);
void br_mem_free_contig (void *ptr, br_size_t size);
   /* 服务无 scatter-gather 的 DMA 引擎、v2 重定位的大块搬运;
      帧缓冲类大固定块走 region 表静态预留, 不走运行时分配 */

/* ---- 页分配器(CA-8): 页粒度, 静态位图 + 连续 run 首次适配 ---- */
void *br_page_alloc(br_size_t n_pages, br_u32 attrs);
void  br_page_free (void *vaddr, br_size_t n_pages);

/* ---- DMA(建于 contig 之上: 保证物理连续) ---- */
typedef struct {
    void         *vaddr;
    br_uintptr_t  dma_addr;     /* v1 恒等: dma_addr == vaddr(CA-6/INV-6) */
    br_size_t     size;
} br_dma_buf_t;

int br_dma_alloc(br_size_t n, br_u32 attrs, br_dma_buf_t *out);
int br_dma_free (const br_dma_buf_t *b);

/* arena 记账/bridge 上报(设计 3-01 §6; TC-MEM-007 执法"alloc/free 增减匹配") */
int br_heap_usage(br_size_t *used, br_size_t *total);

/* ---------------------------------------------------------------- 初始化 */
/*
 * 扫 region 表, 按种类认领三池 + DMA 池, 建 TLSF 控制块与页位图。
 * 归属(3-04 §3): **初始化点 = core.init**, 由 core 的入口 `br_core_main()` 在
 * `platform 插件初始化之后、br_mm_activate() 之前`调用(启动链阶段 ③, ADR-0008):
 * platform 只声明 region 表(哪块内存是什么), 认领与分配归 core。
 * 错误: `-EINVAL`(region 粒度/对齐不合)/ `-ENODEV`(缺任一必需池)/ `-ENOMEM`(池太小)
 * 幂等: 二次调用返回首次的结果, 不重建池。
 */
int  br_mem_init (void);
br_bool br_mem_ready(void);

/* 池几何(观测; dump/memleak 上报用) */
typedef struct { br_uintptr_t base; br_size_t size; } br_pool_t;
typedef struct {
    br_pool_t heap;
    br_pool_t contig;
    br_pool_t page;
    br_pool_t dma;
    br_u32    page_size;      /* = BR_PAGE_SIZE */
    br_u32    page_total;     /* 页池总页数 */
} br_mem_layout_t;

int br_mem_layout(br_mem_layout_t *out);          /* 未就绪 ⇒ -ENODEV */

/* ---------------------------------------------------------------- ② 观测契约 */
/* 堆统计(观测面; `br_heap_usage` 是它在 native 面的投影) */
typedef struct {
    br_size_t total;            /* 堆池总字节(可用区, 不含 TLSF 元数据) */
    br_size_t used;             /* 当前已分配给用户 + 每块的 header/footer/红区 */
    br_size_t free_bytes;
    br_size_t max_free_block;   /* 最大可分配块(碎片程度的一阶指标) */
    br_size_t high_water;       /* used 的历史峰值 */
    br_u32    n_alloc;          /* 成功分配次数 */
    br_u32    n_free;           /* 成功释放次数 */
    br_u32    n_live;           /* 当前存活块数 */
    br_u32    n_fail;           /* 分配失败次数 */
    br_u32    redzone_hits;     /* 红区(尾)被踩次数 —— 检出即破坏 */
    br_u32    canary_hits;      /* 头部魔数被破坏次数 */
    br_u32    double_free;      /* 重复释放被拦截次数 */
    br_u32    bad_free;         /* 释放非本堆指针被拦截次数 */
    br_u32    owner_count;      /* 已注册归属标签数 */
} br_heap_stats_t;

int br_heap_stats_get(br_heap_stats_t *out);      /* 未就绪 ⇒ -ENODEV */

/* 存活块(逐块遍历; memleak 的泄漏报告数据源) */
typedef struct {
    void        *addr;        /* 用户可见地址 */
    br_size_t    size;        /* 用户请求大小 */
    br_size_t    block;       /* 含 header/footer 的实占 */
    br_u16       owner;       /* 归属标签(BR_OWNER_NONE = 未归属) */
    br_uintptr_t caller;      /* 分配点返回地址(离线用 addr2line/llvm-symbolizer 解码) */
    br_u32       seq;         /* 分配序号(单调; 便于"最早的可疑泄漏"排序) */
} br_heap_block_t;

typedef int (*br_heap_walk_fn)(void *ctx, const br_heap_block_t *blk);
int   br_heap_walk(br_heap_walk_fn fn, void *ctx);   /* 返回遍历到的块数; -ENODEV 未就绪 */

/* 全堆自检: 验证每个块的头魔数/红区/空闲链毒化; 返回**违约数**(0 = 干净, TC-DBG-041) */
br_u32 br_heap_check(void);

/* 页池统计(观测) */
typedef struct {
    br_u32 total;
    br_u32 used;
    br_u32 free;
    br_u32 fail;
    br_u32 max_run;      /* 当前最大连续空闲 run(页) */
} br_page_stats_t;

int br_page_stats(br_page_stats_t *out);

/* ---------------------------------------------------------------- ③ 记账契约 */
/*
 * 归属标签 = 5-01 §4 "per-plugin arena 记账"的**v1.x 便宜形态**: 分配时把"当前归属"
 * 记进块头, 泄漏报告按归属出账。真正的 arena(预算上限 + 归属强制 + OOM 策略)归 v2。
 * 缺省归属 = BR_OWNER_NONE(0); 未注册时 current 恒为 0。
 */
typedef br_u16 br_owner_t;
#define BR_OWNER_NONE   0u

br_owner_t  br_heap_owner_register(const char *name);   /* 满/名字重复 ⇒ 已有标签或 None */
const char *br_heap_owner_name(br_owner_t o);           /* 未知 ⇒ "?"; None ⇒ "(none)" */
br_owner_t  br_heap_owner_set(br_owner_t o);            /* 返回旧值(便于成对恢复) */
br_owner_t  br_heap_owner_get(void);

#endif /* BR_CORE_BR_MEM_H */
