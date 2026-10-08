/*
 * brickOS prototype v0.1.0 — 内存子系统内部真值(core 私有; **不导出**, CA-10)
 *
 * 这里放两层东西, 都在 `docs/3-os-core/3-01-core-api-list.md` §6 的实现侧:
 *   ① TLSF 的**参数字典与单实例接口**(tlsf.c)。TLSF 刻意做成"caller 提供 arena、
 *      core 持一个 static 控制块"的形态 —— 原型只有一个堆(§6: "v1 只有全局堆"),
 *      所以接口不接收句柄: 宿主用例与 mem.c 调的是**同一个实例**。
 *      为什么参数是"字典"而不是散落的魔数: 一级 32 类(2^5)× 二级每类 32 细分(2^5)
 *      是 TLSF 的 O(1) 前提, 任何一个改了都会让位图/映射不一致 —— 集中在这里,
 *      `mapping_insert`/`mapping_search` 与 `fl_bitmap`/`sl_bitmap` 共用同一份真值。
 *   ② 页池的内部建立接口(page.c)。`br_mem_init()` 必须把 region 表里
 *      `BR_MM_KIND_PAGE` 那条的 base/size 交给页分配器(§6 的"platform region 表
 *      把 RAM 划为 heap / contig 池 / 页池"); 公开面(§6)只有 alloc/free/stats,
 *      没有"建池", 所以这条入口是内部契约。
 *
 * 归属: core 私有。宿主用例(TC-MEM-002 的 `br_tlsf_check()==0`)直接 include 本文件
 * 是为了**压测算法本身**, 不是把它当对外契约 —— 对外契约只有 br_mem.h/br_mm.h。
 */
#ifndef BR_MEM_TLSF_INTERNAL_H
#define BR_MEM_TLSF_INTERNAL_H

#include <br/core/br_types.h>

/* =====================================================================
 * ① TLSF 参数字典(3-01 §6 的 TLSF 堆)
 * ===================================================================== */

/* 块对齐 = 16 B。与 br_mem.h 的 BR_MALLOC_ALIGN(16)同源: TLSF 的块步长必须能被
 * 用户对齐整除, 否则 mem.c 的"用户指针 = 块头 + 32"拿不到 16 对齐的返回。 */
#define BR_TLSF_ALIGN_LOG2   4u
#define BR_TLSF_ALIGN        (1u << BR_TLSF_ALIGN_LOG2)

/* 二级隔离: 一级 32 类 × 二级每类 32 细分(2^5 × 2^5)。 */
#define BR_TLSF_FL_COUNT     32u
#define BR_TLSF_SL_LOG2      5u
#define BR_TLSF_SL_COUNT     (1u << BR_TLSF_SL_LOG2)

/* 块头 = {size|free 位, prev_phys}(各 8 B)。 */
#define BR_TLSF_HDR          16u
/* 最小块 = 块头 16 + 空闲链 16。空闲链存**块尾**(见 tlsf.c 的设计注释)。 */
#define BR_TLSF_MIN_BLOCK    32u

/* 每块固定元数据开销(不含对齐松弛): 块头 + 最小载荷。建池最小尺寸判据用它。 */
#define BR_TLSF_OVERHEAD     (BR_TLSF_HDR + BR_TLSF_MIN_BLOCK)

/* TLSF 单实例接口(3-01 §6; 原型单堆 ⇒ 无句柄)。 */
br_size_t br_tlsf_overhead(void);
int       br_tlsf_init(void *arena, br_size_t bytes);
void     *br_tlsf_alloc(br_size_t n);
void      br_tlsf_free(void *p);
br_size_t br_tlsf_block_size(void *p);   /* 实占总字节(含 TLSF 块头); 非本堆 ⇒ 0 */
br_size_t br_tlsf_used(void);
br_size_t br_tlsf_free_bytes(void);
br_size_t br_tlsf_max_free(void);        /* 最大空闲块**载荷**(可分配字节) */
br_u32    br_tlsf_check(void);           /* 违约数(0 = 干净) */

/* =====================================================================
 * ② 页池内部建立(page.c)
 * ===================================================================== */

/* 页池位图的静态上界(CA-8 的"静态位图")。
 * 上界从哪来: 位图的字数是编译期常量, 不能随 region 表增长而变 —— 取 4096 页,
 * 即 4096 × 4 KiB = 16 MiB 池上界(位图本体 512 B)。原型三池合计远小于此;
 * 超过上界时 `br_page_pool_init()` 返回 -ENOMEM, 而不是静默截断。 */
#define BR_PAGE_MAX_PAGES    4096u

/* 建页池: base 必须 4 KiB 对齐, size 必须非零且是 4 KiB 粒度。
 * 返回 0 / -EINVAL(粒度) / -ENOMEM(页数超 BR_PAGE_MAX_PAGES)。 */
int br_page_pool_init(br_uintptr_t base, br_size_t size);

#endif /* BR_MEM_TLSF_INTERNAL_H */
