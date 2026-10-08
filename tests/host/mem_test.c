/*
 * tests/host/mem_test.c — 宿主侧内存语义用例(设计 6-01 的 TC-MEM / TC-MM 两组)
 *
 * 为什么在宿主跑: TLSF 的合并/分裂/碎片、页位图的 run 分配、region 表的重叠判定
 * 是**算法性质**, 宿主上几秒钟能跑几十万次操作(`make mem-test`, 见 Makefile 的注释);
 * 同一批性质在 QEMU 上只能靠几条用例撞运气。本文件**不使用 malloc**: 用 2 MiB
 * 静态缓冲区模拟 RAM, 自己经 `br_mm_region_add()` 声明 IMAGE/HEAP/CONTIG/PAGE/DMA/
 * RESERVED 各一条, 再交给 `br_mem_init()` 认领三池。
 *
 * 输出协议(与 target 侧 [MEMCONF] 同型, 便于 CI grep):
 *   [HOSTTEST] PASS|FAIL <tag> <desc>
 *   [HOSTTEST] SUMMARY pass=N fail=0 total=N
 * 有失败 ⇒ 退出码非 0。
 */
#include <br/core/br_error.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_types.h>

#include "tlsf_internal.h"

#include <stdio.h>

/* =====================================================================
 * 用例框架
 * ===================================================================== */

static int s_pass;
static int s_fail;

#define CHECK(cond, tag, desc)                                          \
    do {                                                                \
        if ((cond) != 0) {                                              \
            s_pass++;                                                   \
            printf("[HOSTTEST] PASS %s %s\n", (tag), (desc));           \
        } else {                                                        \
            s_fail++;                                                   \
            printf("[HOSTTEST] FAIL %s %s\n", (tag), (desc));           \
        }                                                               \
    } while (0)

static void section(const char *tag, const char *desc)
{
    printf("[HOSTTEST] ---- %s %s\n", tag, desc);
}

static br_bool str_is(const char *a, const char *b)
{
    if ((a == NULL) || (b == NULL)) {
        return BR_FALSE;
    }
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

/* =====================================================================
 * 模拟 RAM(静态; 不用 malloc)
 * ===================================================================== */

static br_u8 g_ram[2u * 1024u * 1024u] BR_ALIGN(4096);
static br_u8 g_regspace[128u * 1024u] BR_ALIGN(4096);

static const br_size_t SZ_IMAGE  = 64u * 1024u;
static const br_size_t SZ_HEAP   = 1024u * 1024u;
static const br_size_t SZ_CONTIG = 256u * 1024u;
static const br_size_t SZ_PAGE   = 256u * 1024u;
static const br_size_t SZ_DMA    = 128u * 1024u;
static const br_size_t SZ_RESV   = 64u * 1024u;

static br_uintptr_t g_heap_base;
static br_uintptr_t g_contig_base;
static br_uintptr_t g_page_base;
static br_uintptr_t g_dma_base;
static br_uintptr_t g_resv_base;

/* =====================================================================
 * br_mm_ops 替身(记录派发; 只为验证 core 侧状态机/一致性, 不建真页表)
 * ===================================================================== */

static struct {
    br_u32       activate_calls;
    br_u32       activate_count;
    br_u32       map_calls;
    br_u32       set_attrs_calls;
    br_u32       query_calls;
    br_u32       flush_calls;
    br_u32       inv_calls;
    br_uintptr_t fail_base;      /* map_region 对这个 base 返回 -EIO(回滚用例) */
} s_ops_stats;

static int stub_activate(const br_mm_region_t *regions, br_u32 count)
{
    (void)regions;
    s_ops_stats.activate_calls++;
    s_ops_stats.activate_count = count;
    return BR_OK;
}

static int stub_map_region(const br_mm_region_t *r)
{
    s_ops_stats.map_calls++;
    if ((s_ops_stats.fail_base != 0u) && (r->base == s_ops_stats.fail_base)) {
        return BR_ERR(BR_EIO);
    }
    return BR_OK;
}

static int stub_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs)
{
    (void)addr;
    (void)size;
    (void)attrs;
    s_ops_stats.set_attrs_calls++;
    return BR_OK;
}

static int stub_query(br_uintptr_t addr, br_size_t size, br_u32 *out_attrs)
{
    (void)addr;
    (void)size;
    if (out_attrs != NULL) {
        *out_attrs = BR_MM_CACHED;
    }
    s_ops_stats.query_calls++;
    return BR_OK;
}

static void stub_flush(void *addr, br_size_t size)
{
    (void)addr;
    (void)size;
    s_ops_stats.flush_calls++;
}

static void stub_inv(void *addr, br_size_t size)
{
    (void)addr;
    (void)size;
    s_ops_stats.inv_calls++;
}

static const br_mm_ops_t s_ops = {
    .activate         = stub_activate,
    .map_region       = stub_map_region,
    .set_attrs        = stub_set_attrs,
    .query            = stub_query,
    .cache_flush      = stub_flush,
    .cache_invalidate = stub_inv,
};

/* =====================================================================
 * region 声明
 * ===================================================================== */

static int add_region(br_uintptr_t base, br_size_t size, br_u32 attrs)
{
    br_mm_region_t r;
    r.base  = base;
    r.size  = size;
    r.attrs = attrs;
    return br_mm_region_add(&r);
}

static int setup_regions(void)
{
    const br_uintptr_t b = (br_uintptr_t)(void *)g_ram;

    int rc = 0;
    rc |= add_region(b, SZ_IMAGE,
                     BR_MM_WITH_KIND(BR_MM_RO | BR_MM_NX | BR_MM_CACHED,
                                     BR_MM_KIND_IMAGE));
    g_heap_base = b + SZ_IMAGE;
    rc |= add_region(g_heap_base, SZ_HEAP,
                     BR_MM_WITH_KIND(BR_MM_CACHED, BR_MM_KIND_HEAP));
    g_contig_base = g_heap_base + SZ_HEAP;
    rc |= add_region(g_contig_base, SZ_CONTIG,
                     BR_MM_WITH_KIND(BR_MM_CACHED, BR_MM_KIND_CONTIG));
    g_page_base = g_contig_base + SZ_CONTIG;
    rc |= add_region(g_page_base, SZ_PAGE,
                     BR_MM_WITH_KIND(BR_MM_CACHED, BR_MM_KIND_PAGE));
    g_dma_base = g_page_base + SZ_PAGE;
    rc |= add_region(g_dma_base, SZ_DMA,
                     BR_MM_WITH_KIND(0u, BR_MM_KIND_DMA));   /* 非 CACHED */
    g_resv_base = g_dma_base + SZ_DMA;
    rc |= add_region(g_resv_base, SZ_RESV, BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
    return rc;
}

/* =====================================================================
 * TC-MEM-001: malloc 对齐 >= BR_MALLOC_ALIGN
 * ===================================================================== */

static void test_mem_001_align(void)
{
    static const br_size_t sizes[] = {
        1u, 2u, 3u, 7u, 8u, 15u, 16u, 17u, 31u, 32u, 33u, 63u, 64u,
        100u, 255u, 256u, 1000u, 4096u,
    };

    section("TC-MEM-001", "malloc 对齐 >= BR_MALLOC_ALIGN");

    br_u32 bad_align = 0u;
    br_u32 bad_null  = 0u;
    br_u32 bad_data  = 0u;

    for (br_u32 i = 0u; i < (br_u32)BR_ARRAY_SIZE(sizes); i++) {
        const br_size_t n = sizes[i];
        br_u8 *p = (br_u8 *)br_malloc(n);
        if (p == NULL) {
            bad_null++;
            continue;
        }
        if (((br_uintptr_t)p & (br_uintptr_t)(BR_MALLOC_ALIGN - 1u)) != 0u) {
            bad_align++;
        }
        for (br_size_t k = 0u; k < n; k++) {
            p[k] = (br_u8)(k & 0xFFu);
        }
        for (br_size_t k = 0u; k < n; k++) {
            if (p[k] != (br_u8)(k & 0xFFu)) {
                bad_data++;
                break;
            }
        }
        br_free(p);
    }

    printf("[HOSTTEST]   cases=%u bad_null=%u bad_align=%u bad_data=%u (align=%u)\n",
           (unsigned)BR_ARRAY_SIZE(sizes), (unsigned)bad_null, (unsigned)bad_align,
           (unsigned)bad_data, (unsigned)BR_MALLOC_ALIGN);
    CHECK((bad_null == 0u) && (bad_align == 0u) && (bad_data == 0u),
          "TC-MEM-001", "全部 size 分配成功、16 对齐且内容可读写");
}

/* =====================================================================
 * TC-MEM-007: br_heap_usage 记账(分配增量 >= 请求量; 全释放回基线)
 * ===================================================================== */

static void test_mem_007_usage(void)
{
    section("TC-MEM-007", "heap_usage 记账");

    br_size_t u0 = 0u;
    br_size_t t0 = 0u;
    br_size_t u1 = 0u;
    br_size_t t1 = 0u;
    br_size_t u2 = 0u;
    br_size_t t2 = 0u;

    (void)br_heap_usage(&u0, &t0);
    void *p = br_malloc(3000u);
    (void)br_heap_usage(&u1, &t1);
    br_free(p);
    (void)br_heap_usage(&u2, &t2);

    printf("[HOSTTEST]   used: base=%lu after_alloc=%lu after_free=%lu total=%lu\n",
           (unsigned long)u0, (unsigned long)u1, (unsigned long)u2,
           (unsigned long)t0);

    CHECK(p != NULL, "TC-MEM-007", "3000 B 分配成功");
    CHECK(u1 >= (u0 + 3000u), "TC-MEM-007", "used 增量 >= 请求量");
    CHECK(u2 == u0, "TC-MEM-007", "释放后 used 回到基线");
    CHECK(t1 == t0, "TC-MEM-007", "total 不随分配变化");
}

/* =====================================================================
 * TC-MEM-003: calloc 零化 / 溢出 / realloc 保前缀 + 旧块回收
 * ===================================================================== */

static void test_mem_003_calloc_realloc(void)
{
    section("TC-MEM-003", "calloc / realloc 语义");

    br_heap_stats_t st0;
    (void)br_heap_stats_get(&st0);

    /* calloc 零化 */
    br_u8 *c = (br_u8 *)br_calloc(16u, 8u);
    br_u32 nz = 0u;
    if (c != NULL) {
        for (br_u32 i = 0u; i < 128u; i++) {
            if (c[i] != 0u) {
                nz++;
            }
        }
    }
    CHECK((c != NULL) && (nz == 0u), "TC-MEM-003", "calloc 返回全零");
    br_free(c);

    /* 乘法溢出 ⇒ NULL + n_fail++ */
    br_heap_stats_t st_a;
    (void)br_heap_stats_get(&st_a);
    void *ov = br_calloc(~(br_size_t)0u, 2u);
    br_heap_stats_t st_b;
    (void)br_heap_stats_get(&st_b);
    printf("[HOSTTEST]   calloc 溢出 ptr=%p n_fail %u -> %u\n",
           ov, (unsigned)st_a.n_fail, (unsigned)st_b.n_fail);
    CHECK((ov == NULL) && (st_b.n_fail > st_a.n_fail), "TC-MEM-003",
          "calloc 乘法溢出被拦截并计数");

    /* realloc 保前缀 */
    br_size_t ub = 0u;
    br_size_t tb = 0u;
    (void)br_heap_usage(&ub, &tb);

    br_u8 *p = (br_u8 *)br_malloc(100u);
    for (br_u32 i = 0u; i < 100u; i++) {
        p[i] = (br_u8)('A' + (i % 26u));
    }

    br_size_t u_before = 0u;
    (void)br_heap_usage(&u_before, &tb);

    br_u8 *q = (br_u8 *)br_realloc(p, 200u);

    br_size_t u_after = 0u;
    (void)br_heap_usage(&u_after, &tb);
    br_heap_stats_t st_c;
    (void)br_heap_stats_get(&st_c);

    br_u32 bad = 0u;
    if (q != NULL) {
        for (br_u32 i = 0u; i < 100u; i++) {
            if (q[i] != (br_u8)('A' + (i % 26u))) {
                bad++;
            }
        }
    }

    printf("[HOSTTEST]   realloc used %lu -> %lu n_live=%u prefix_bad=%u\n",
           (unsigned long)u_before, (unsigned long)u_after,
           (unsigned)st_c.n_live, (unsigned)bad);

    CHECK((q != NULL) && (bad == 0u), "TC-MEM-003", "realloc 保留 min(old,new) 前缀");
    CHECK(st_c.n_live == 1u, "TC-MEM-003", "realloc 后只剩一个新块(旧块已回收)");
    CHECK(u_after <= (u_before + 200u + 64u), "TC-MEM-003", "realloc 未额外堆增长");

    br_free(q);

    br_size_t u_end = 0u;
    (void)br_heap_usage(&u_end, &tb);
    CHECK(u_end == ub, "TC-MEM-003", "realloc 链全部释放后回到基线");
}

/* =====================================================================
 * TC-MEM-004: contig 对齐 / 互不重叠 / 释放可复用
 * ===================================================================== */

static void test_mem_004_contig(void)
{
    section("TC-MEM-004", "contig align=4096");

    void *a = NULL;
    void *b = NULL;
    void *c = NULL;
    br_size_t total = 0u;
    (void)br_heap_usage(&total, &total);

    const int r1 = br_mem_alloc_contig(4096u, 4096u, &a);
    const int r2 = br_mem_alloc_contig(4096u, 4096u, &b);

    const br_bool a_ok = (r1 == 0) && (a != NULL) &&
                         (((br_uintptr_t)a & 4095u) == 0u);
    const br_bool b_ok = (r2 == 0) && (b != NULL) &&
                         (((br_uintptr_t)b & 4095u) == 0u);
    const br_bool disjoint = (a != b) &&
                             ((((br_uintptr_t)a + 4096u) <= (br_uintptr_t)b) ||
                              (((br_uintptr_t)b + 4096u) <= (br_uintptr_t)a));

    printf("[HOSTTEST]   a=%p b=%p rc=(%d,%d) disjoint=%d\n",
           (void *)a, (void *)b, r1, r2, (int)disjoint);

    CHECK(a_ok && b_ok, "TC-MEM-004", "两次 4 KiB 对齐分配均 4096 对齐");
    CHECK(disjoint, "TC-MEM-004", "两次分配区间互不重叠");

    /* 非 2 的幂 align ⇒ -EINVAL */
    void *x = NULL;
    const int rbad = br_mem_alloc_contig(4096u, 3000u, &x);
    CHECK(rbad == BR_ERR(BR_EINVAL), "TC-MEM-004", "align 非 2 的幂 ⇒ -EINVAL");

    br_mem_free_contig(a, 4096u);
    const int r3 = br_mem_alloc_contig(4096u, 4096u, &c);
    printf("[HOSTTEST]   释放 a 后重分配 c=%p (a=%p) rc=%d\n", (void *)c, (void *)a, r3);
    CHECK((r3 == 0) && (c == a), "TC-MEM-004", "释放后可复分配同一 run");

    /* size 入参与记录不一致: 按记录释放(不切坏池) */
    br_mem_free_contig(b, 123u);
    br_mem_free_contig(c, 4096u);

    void *d = NULL;
    const int r4 = br_mem_alloc_contig(8192u, 4096u, &d);
    printf("[HOSTTEST]   合并后 8192 B 分配 d=%p rc=%d\n", (void *)d, r4);
    CHECK((r4 == 0) && (d == a), "TC-MEM-004", "相邻释放即合并(整块 8 KiB 可复用)");
    br_mem_free_contig(d, 8192u);
}

/* =====================================================================
 * TC-MEM-005: 页分配连续 + 释放复用
 * ===================================================================== */

static void test_mem_005_page(void)
{
    section("TC-MEM-005", "page alloc(3) 连续 + 复用");

    void *p3 = br_page_alloc(3u, 0u);
    void *p1 = br_page_alloc(1u, BR_PAGE_F_ZERO);

    const br_bool contig = (p3 != NULL) && (p1 != NULL) &&
                           (((br_uintptr_t)p1 - (br_uintptr_t)p3) == 3u * 4096u);
    printf("[HOSTTEST]   p3=%p p1=%p delta=%lu\n", p3, p1,
           (unsigned long)((p1 != NULL && p3 != NULL)
                               ? ((br_uintptr_t)p1 - (br_uintptr_t)p3) : 0u));

    br_u32 nz = 0u;
    if (p1 != NULL) {
        const br_u8 *z = (const br_u8 *)p1;
        for (br_u32 i = 0u; i < 4096u; i++) {
            if (z[i] != 0u) {
                nz++;
            }
        }
    }
    CHECK(contig, "TC-MEM-005", "3 页 run 连续(后一分配紧随其后)");
    CHECK(nz == 0u, "TC-MEM-005", "BR_PAGE_F_ZERO 生效");

    br_page_free(p3, 3u);

    void *p3b = br_page_alloc(3u, 0u);
    printf("[HOSTTEST]   释放 3 页后重分配=%p (原=%p)\n", p3b, p3);
    CHECK((p3b != NULL) && (p3b == p3), "TC-MEM-005", "释放后首次适配复现同一 run");

    br_page_free(p3b, 3u);
    br_page_free(p1, 1u);

    br_page_stats_t ps;
    (void)br_page_stats(&ps);
    CHECK(ps.used == 0u, "TC-MEM-005", "页池净空 used=0");
}

/* =====================================================================
 * TC-MEM-006: DMA dma_addr == vaddr(CA-6/INV-6)
 * ===================================================================== */

static void test_mem_006_dma(void)
{
    section("TC-MEM-006", "dma_addr == vaddr");

    br_dma_buf_t b;
    const int rc = br_dma_alloc(8192u, BR_DMA_F_ZERO | BR_DMA_F_ALIGN4K, &b);

    const br_bool ident = (rc == 0) && (b.vaddr != NULL) &&
                          (b.dma_addr == (br_uintptr_t)b.vaddr) && (b.size == 8192u);
    const br_bool aligned = (rc == 0) &&
                            (((br_uintptr_t)b.vaddr & 4095u) == 0u);
    br_u32 nz = 0u;
    if (rc == 0) {
        const br_u8 *z = (const br_u8 *)b.vaddr;
        for (br_u32 i = 0u; i < 8192u; i++) {
            if (z[i] != 0u) {
                nz++;
            }
        }
    }
    printf("[HOSTTEST]   vaddr=%p dma=0x%lx size=%lu nz=%u\n",
           b.vaddr, (unsigned long)b.dma_addr, (unsigned long)b.size, (unsigned)nz);

    CHECK(ident, "TC-MEM-006", "v1 恒等: dma_addr == vaddr");
    CHECK(aligned, "TC-MEM-006", "默认 4 KiB 对齐");
    CHECK(nz == 0u, "TC-MEM-006", "BR_DMA_F_ZERO 生效");
    CHECK(br_dma_free(&b) == 0, "TC-MEM-006", "br_dma_free 成功归还");

    /* dma_addr 与 vaddr 不一致 ⇒ 拒收(不许归还错地址) */
    br_dma_buf_t c;
    (void)br_dma_alloc(4096u, 0u, &c);
    br_dma_buf_t bad = c;
    bad.dma_addr = (br_uintptr_t)c.vaddr + 4096u;
    const int rbad = br_dma_free(&bad);
    printf("[HOSTTEST]   不一致 dma_free rc=%d\n", rbad);
    CHECK(rbad == BR_ERR(BR_EINVAL), "TC-MEM-006", "dma_addr 不一致被拒");
    CHECK(br_dma_free(&c) == 0, "TC-MEM-006", "拒收后真身仍可归还");
}

/* =====================================================================
 * 页池耗尽 ⇒ NULL 且 fail 增长; 全释放后 used==0
 * ===================================================================== */

static void *s_page_arr[BR_PAGE_MAX_PAGES];

static void test_page_exhaust(void)
{
    section("TC-MEM-PAGE-EXHAUST", "页池耗尽");

    br_page_stats_t ps0;
    (void)br_page_stats(&ps0);

    br_u32 n = 0u;
    while (n < ps0.total) {
        void *p = br_page_alloc(1u, 0u);
        if (p == NULL) {
            break;
        }
        s_page_arr[n] = p;
        n++;
    }

    br_page_stats_t ps1;
    (void)br_page_stats(&ps1);
    void *over = br_page_alloc(1u, 0u);
    br_page_stats_t ps2;
    (void)br_page_stats(&ps2);

    printf("[HOSTTEST]   total=%u 分配=%u free=%u over=%p fail %u -> %u\n",
           (unsigned)ps0.total, (unsigned)n, (unsigned)ps1.free, over,
           (unsigned)ps1.fail, (unsigned)ps2.fail);

    CHECK(n == ps0.total, "TC-MEM-PAGE-EXHAUST", "可分配页数 == 池总页数");
    CHECK(ps1.free == 0u, "TC-MEM-PAGE-EXHAUST", "池满时 free==0");
    CHECK((over == NULL) && (ps2.fail > ps1.fail), "TC-MEM-PAGE-EXHAUST",
          "耗尽后返回 NULL 且 fail 增长");

    for (br_u32 i = 0u; i < n; i++) {
        br_page_free(s_page_arr[i], 1u);
    }
    br_page_stats_t ps3;
    (void)br_page_stats(&ps3);
    printf("[HOSTTEST]   全释放后 used=%u free=%u max_run=%u\n",
           (unsigned)ps3.used, (unsigned)ps3.free, (unsigned)ps3.max_run);
    CHECK(ps3.used == 0u, "TC-MEM-PAGE-EXHAUST", "全释放后 used==0");

    /* 重复释放: 忽略 + fail++ */
    const br_u32 fail_before = ps3.fail;
    br_page_free(s_page_arr[0], 1u);
    br_page_stats_t ps4;
    (void)br_page_stats(&ps4);
    CHECK(ps4.fail > fail_before, "TC-MEM-PAGE-EXHAUST", "重复释放被忽略并计数");
}

/* =====================================================================
 * TC-LEAK-UNIT: 红区 / 双重释放 / 非本堆指针
 * ===================================================================== */

static br_u8 g_outside[64] BR_ALIGN(16);

static void test_leak_unit(void)
{
    section("TC-LEAK-UNIT", "红区 / 双重释放 / 非本堆指针");

    br_heap_stats_t s0;
    (void)br_heap_stats_get(&s0);

    /* ① 写用户区后一字节: 必然落在红区(红区最小 8 B) */
    br_u8 *p = (br_u8 *)br_malloc(37u);
    p[37] = 0x00u;
    const br_u32 v1 = br_heap_check();
    br_heap_stats_t s1;
    (void)br_heap_stats_get(&s1);
    printf("[HOSTTEST]   红区越界: check=%u redzone_hits %u -> %u\n",
           (unsigned)v1, (unsigned)s0.redzone_hits, (unsigned)s1.redzone_hits);

    CHECK((v1 >= 1u) && (s1.redzone_hits > s0.redzone_hits), "TC-LEAK-UNIT",
          "红区越界被 br_heap_check() 检出");

    p[37] = 0xA5u;   /* 复原, 使堆重回干净 */
    const br_u32 v2 = br_heap_check();
    CHECK(v2 == 0u, "TC-LEAK-UNIT", "复原后 br_heap_check()==0");
    br_free(p);

    /* ② 双重释放 */
    void *q = br_malloc(64u);
    br_free(q);
    br_free(q);
    br_heap_stats_t s2;
    (void)br_heap_stats_get(&s2);
    const br_u32 v3 = br_heap_check();
    printf("[HOSTTEST]   double_free=%u check=%u\n",
           (unsigned)s2.double_free, (unsigned)v3);
    CHECK(s2.double_free >= 1u, "TC-LEAK-UNIT", "双重释放被拦截并计数");
    CHECK(v3 == 0u, "TC-LEAK-UNIT", "双重释放后堆仍完好");

    /* ③ 释放非本堆指针 */
    br_heap_stats_t s3;
    (void)br_heap_stats_get(&s3);
    br_free(g_outside);
    br_heap_stats_t s4;
    (void)br_heap_stats_get(&s4);
    CHECK(s4.bad_free > s3.bad_free, "TC-LEAK-UNIT", "非本堆指针被拦截");

    /* ④ 未对齐指针 */
    void *r = br_malloc(32u);
    br_free((void *)((br_u8 *)r + 1u));
    br_heap_stats_t s5;
    (void)br_heap_stats_get(&s5);
    CHECK(s5.bad_free > s4.bad_free, "TC-LEAK-UNIT", "未对齐指针被拦截");
    br_free(r);
}

/* =====================================================================
 * TC-MEM-TLSF: 直接压 TLSF 单实例(br_tlsf_check / block_size)
 * ===================================================================== */

static void test_tlsf_direct(void)
{
    section("TC-MEM-TLSF", "直接调用 TLSF 接口");

    printf("[HOSTTEST]   overhead=%lu max_free=%lu used=%lu\n",
           (unsigned long)br_tlsf_overhead(),
           (unsigned long)br_tlsf_max_free(),
           (unsigned long)br_tlsf_used());

    const br_u32 v0 = br_tlsf_check();
    br_u8 *p = (br_u8 *)br_tlsf_alloc(64u);

    br_bool ok = (p != NULL) &&
                 (((br_uintptr_t)p & 15u) == 0u) &&
                 (br_tlsf_block_size(p) >= 80u);
    if (p != NULL) {
        for (br_u32 i = 0u; i < 64u; i++) {
            p[i] = (br_u8)(i ^ 0x5Au);
        }
        for (br_u32 i = 0u; i < 64u; i++) {
            if (p[i] != (br_u8)(i ^ 0x5Au)) {
                ok = BR_FALSE;
            }
        }
    }
    br_tlsf_free(p);

    const br_u32 v1 = br_tlsf_check();
    printf("[HOSTTEST]   check before=%u after=%u\n", (unsigned)v0, (unsigned)v1);

    CHECK(ok, "TC-MEM-TLSF", "alloc 16 对齐 + block_size >= 请求+头");
    CHECK((v0 == 0u) && (v1 == 0u), "TC-MEM-TLSF", "tlsf_check 前后均 0 违约");
}

/* =====================================================================
 * TC-MEM-WALK: 存活块遍历按 seq 升序
 * ===================================================================== */

static struct {
    br_u32 seen;
    br_u32 last_seq;
    br_u32 order_bad;
} s_walk;

static int walk_cb(void *ctx, const br_heap_block_t *blk)
{
    (void)ctx;
    if ((s_walk.seen > 0u) && (blk->seq <= s_walk.last_seq)) {
        s_walk.order_bad++;
    }
    s_walk.last_seq = blk->seq;
    s_walk.seen++;
    return 0;
}

static void test_walk(void)
{
    section("TC-MEM-WALK", "存活块按 seq 升序回调");

    void *a = br_malloc(10u);
    void *b = br_malloc(20u);
    void *c = br_malloc(30u);

    s_walk.seen = 0u;
    s_walk.last_seq = 0u;
    s_walk.order_bad = 0u;
    const int n = br_heap_walk(walk_cb, NULL);

    printf("[HOSTTEST]   walk n=%d seen=%u order_bad=%u\n",
           n, (unsigned)s_walk.seen, (unsigned)s_walk.order_bad);

    CHECK((n == 3) && (s_walk.seen == 3u), "TC-MEM-WALK", "遍历到 3 个存活块");
    CHECK(s_walk.order_bad == 0u, "TC-MEM-WALK", "回调序严格按 seq 升序");

    br_free(a);
    br_free(b);
    br_free(c);
}

/* =====================================================================
 * 归属标签(5-01 §4 的 v1.x 形态)
 * ===================================================================== */

static void test_owner(void)
{
    section("TC-MEM-OWNER", "归属标签注册/命名/记账");

    const br_owner_t o1 = br_heap_owner_register("hosttest");
    const br_owner_t dup = br_heap_owner_register("hosttest");
    const char *name_none = br_heap_owner_name(BR_OWNER_NONE);
    const char *name_unk  = br_heap_owner_name((br_owner_t)999u);

    const br_owner_t old = br_heap_owner_set(o1);
    void *p = br_malloc(16u);
    const br_owner_t cur = br_heap_owner_get();

    /* 让 walk 把 owner 带出来 */
    s_walk.seen = 0u;
    s_walk.last_seq = 0u;
    s_walk.order_bad = 0u;

    (void)br_heap_walk(walk_cb, NULL);

    br_heap_stats_t st;
    (void)br_heap_stats_get(&st);

    (void)br_heap_owner_set(old);
    br_free(p);

    printf("[HOSTTEST]   o1=%u dup=%u cur=%u name_none=\"%s\" name_unk=\"%s\" owner_count=%u\n",
           (unsigned)o1, (unsigned)dup, (unsigned)cur, name_none, name_unk,
           (unsigned)st.owner_count);

    CHECK(o1 != BR_OWNER_NONE, "TC-MEM-OWNER", "注册返回非 NONE 标签");
    CHECK(dup == BR_OWNER_NONE, "TC-MEM-OWNER", "重名注册返回 NONE");
    CHECK(str_is(br_heap_owner_name(o1), "hosttest"), "TC-MEM-OWNER", "标签→名字");
    CHECK(str_is(name_none, "(none)") && str_is(name_unk, "?"), "TC-MEM-OWNER",
          "NONE/未知名字约定");
    CHECK(st.owner_count >= 1u, "TC-MEM-OWNER", "owner_count 反映注册数");
}

/* =====================================================================
 * TC-MEM-002: 碎片压力(自写 LCG, 20 万次交错 alloc/free/write/verify)
 * ===================================================================== */

#define STRESS_OPS    200000u
#define STRESS_SLOTS  512u

typedef struct {
    br_u8    *p;
    br_size_t n;
    br_u32    pat;
} stress_slot_t;

static stress_slot_t s_slots[STRESS_SLOTS];
static br_u32        s_rng = 0x12345678u;

static br_u32 rng_next(void)
{
    s_rng = (s_rng * 1103515245u) + 12345u;
    return (s_rng >> 8) & 0xFFFFFFu;
}

static void test_mem_002_stress(void)
{
    section("TC-MEM-002", "碎片压力 20 万次(LCG)");

    br_u32 alloc_fail  = 0u;
    br_u32 mismatch    = 0u;
    br_u32 check_bad   = 0u;
    br_u32 allocs      = 0u;
    br_u32 frees       = 0u;

    for (br_u32 op = 0u; op < STRESS_OPS; op++) {
        const br_u32 r   = rng_next();
        const br_u32 idx = r % (br_u32)STRESS_SLOTS;

        if (s_slots[idx].p == NULL) {
            const br_size_t n = 1u + (rng_next() % 1024u);
            br_u8 *p = (br_u8 *)br_malloc(n);
            if (p == NULL) {
                alloc_fail++;
            } else {
                const br_u32 pat = rng_next() & 0xFFu;
                for (br_size_t k = 0u; k < n; k++) {
                    p[k] = (br_u8)(pat + (br_u32)k);
                }
                s_slots[idx].p   = p;
                s_slots[idx].n   = n;
                s_slots[idx].pat = pat;
                allocs++;
                if (((br_uintptr_t)p & (br_uintptr_t)(BR_MALLOC_ALIGN - 1u)) != 0u) {
                    mismatch++;
                }
            }
        } else {
            br_u8    *p   = s_slots[idx].p;
            const br_size_t n   = s_slots[idx].n;
            const br_u32    pat = s_slots[idx].pat;
            for (br_size_t k = 0u; k < n; k++) {
                if (p[k] != (br_u8)(pat + (br_u32)k)) {
                    mismatch++;
                    break;
                }
            }
            br_free(p);
            s_slots[idx].p = NULL;
            frees++;
        }

        if ((op % 5000u) == 0u) {
            const br_u32 v1 = br_tlsf_check();
            const br_u32 v2 = br_heap_check();
            if ((v1 != 0u) || (v2 != 0u)) {
                check_bad++;
            }
        }
    }

    /* 清场: 剩余块全部释放 */
    br_u32 left = 0u;
    for (br_u32 i = 0u; i < (br_u32)STRESS_SLOTS; i++) {
        if (s_slots[i].p != NULL) {
            br_free(s_slots[i].p);
            s_slots[i].p = NULL;
            left++;
        }
    }

    const br_u32 vf = br_tlsf_check();
    const br_u32 vh = br_heap_check();

    br_heap_stats_t st;
    (void)br_heap_stats_get(&st);

    printf("[HOSTTEST]   ops=%u alloc=%u free=%u left=%u alloc_fail=%u mismatch=%u check_bad=%u\n",
           (unsigned)STRESS_OPS, (unsigned)allocs, (unsigned)frees, (unsigned)left,
           (unsigned)alloc_fail, (unsigned)mismatch, (unsigned)check_bad);
    printf("[HOSTTEST]   收尾: tlsf_check=%u heap_check=%u used=%lu total=%lu max_free=%lu n_live=%u\n",
           (unsigned)vf, (unsigned)vh, (unsigned long)st.used,
           (unsigned long)st.total, (unsigned long)st.max_free_block,
           (unsigned)st.n_live);

    CHECK((mismatch == 0u) && (check_bad == 0u), "TC-MEM-002",
          "20 万次操作内容不变 + 不变量恒 0");
    CHECK((vf == 0u) && (vh == 0u), "TC-MEM-002", "收尾自检 0 违约");
    CHECK((st.n_live == 0u) && (st.used == 0u), "TC-MEM-002", "全部释放后无存活块");
    CHECK((st.max_free_block * 10u) >= (st.total * 8u), "TC-MEM-002",
          "收尾最大空闲块 >= 池的 80%(完全合并)");
}

/* =====================================================================
 * TC-MM-001: region 重叠/重复 ⇒ -EEXIST; 非对齐/ size 0 ⇒ -EINVAL
 * ===================================================================== */

static void test_mm_001_region(void)
{
    section("TC-MM-001", "region 重叠/重复/非法粒度");

    const int r_null = br_mm_region_add(NULL);
    const int r_dup  = add_region(g_heap_base, SZ_HEAP, BR_MM_WITH_KIND(0u, BR_MM_KIND_HEAP));
    const int r_ovl  = add_region(g_heap_base + 4096u, 4096u, BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
    const int r_unal = add_region(g_heap_base + 1u, 4096u, BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
    const int r_zero = add_region(g_heap_base, 0u, BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
    const int r_gran = add_region(g_heap_base, 4096u + 1u, BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));

    printf("[HOSTTEST]   null=%d dup=%d overlap=%d unaligned=%d zero=%d gran=%d\n",
           r_null, r_dup, r_ovl, r_unal, r_zero, r_gran);

    CHECK(r_null == BR_ERR(BR_EINVAL), "TC-MM-001", "NULL ⇒ -EINVAL");
    CHECK(r_dup == BR_ERR(BR_EEXIST), "TC-MM-001", "完全相同 ⇒ -EEXIST");
    CHECK(r_ovl == BR_ERR(BR_EEXIST), "TC-MM-001", "部分重叠 ⇒ -EEXIST");
    CHECK(r_unal == BR_ERR(BR_EINVAL), "TC-MM-001", "base 非 4 KiB 对齐 ⇒ -EINVAL");
    CHECK(r_zero == BR_ERR(BR_EINVAL), "TC-MM-001", "size==0 ⇒ -EINVAL");
    CHECK(r_gran == BR_ERR(BR_EINVAL), "TC-MM-001", "size 非 4 KiB 粒度 ⇒ -EINVAL");

    const br_u32 nkind = br_mm_region_count_kind(BR_MM_KIND_HEAP);
    const br_mm_region_t *hit = br_mm_region_find(g_heap_base + 16u);
    const br_mm_region_t *miss = br_mm_region_find((br_uintptr_t)0x1000u);
    printf("[HOSTTEST]   heap_kind=%u find(hit)=%p find(miss)=%p count=%u\n",
           (unsigned)nkind, (const void *)hit, (const void *)miss,
           (unsigned)br_mm_region_count());
    CHECK(nkind == 1u, "TC-MM-001", "region_count_kind 统计正确");
    CHECK((hit != NULL) && (miss == NULL), "TC-MM-001", "region_find 命中/未命中");
}

/* =====================================================================
 * TC-MM-002: 非 CACHED region 的 cache 维护 = 0(含未激活)
 * ===================================================================== */

static void test_mm_002_cache(void)
{
    section("TC-MM-002", "非 CACHED region cache 维护 = 0");

    const br_u32 before = s_ops_stats.flush_calls;

    void *np = (void *)g_dma_base;   /* DMA region: 非 CACHED */
    const int r1 = br_mm_cache_flush(np, 4096u);
    const int r2 = br_mm_cache_invalidate(np, 4096u);
    const int r3 = br_mm_cache_flush(np, 0u);

    printf("[HOSTTEST]   未激活: flush=%d inv=%d size0=%d flush_calls=%u\n",
           r1, r2, r3, (unsigned)(s_ops_stats.flush_calls - before));

    CHECK((r1 == 0) && (r2 == 0) && (r3 == 0), "TC-MM-002",
          "未激活 + 非 CACHED ⇒ 0(无操作)");
    CHECK(s_ops_stats.flush_calls == before, "TC-MM-002", "非 CACHED 不派发 ops");
}

/* =====================================================================
 * TC-MM-003: map/unmap v1 = -ENOTSUP
 * ===================================================================== */

static void test_mm_003_map(void)
{
    section("TC-MM-003", "map/unmap 签名先行");

    br_mm_region_t r;
    r.base  = g_resv_base;
    r.size  = SZ_RESV;
    r.attrs = BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED);

    const int r1 = br_mm_map(&r);
    const int r2 = br_mm_unmap(g_resv_base, SZ_RESV);
    printf("[HOSTTEST]   map=%d unmap=%d\n", r1, r2);
    CHECK((r1 == BR_ERR(BR_ENOTSUP)) && (r2 == BR_ERR(BR_ENOTSUP)),
          "TC-MM-003", "v1 br_mm_map/unmap 一律 -ENOTSUP");
}

/* =====================================================================
 * 激活状态机 + TC-MM-EXTRA(未声明地址 / 表满) + 回滚
 * ===================================================================== */

static void test_mm_extra(void)
{
    section("TC-MM-EXTRA", "未声明地址 / 表满 / 落表回滚");

    /* 未激活时 set_attrs/query ⇒ -ENOTSUP */
    br_u32 attrs = 0u;
    const int pre1 = br_mm_set_attrs(g_heap_base, 4096u, BR_MM_RO);
    const int pre2 = br_mm_query(g_heap_base, 4096u, &attrs);
    CHECK((pre1 == BR_ERR(BR_ENOTSUP)) && (pre2 == BR_ERR(BR_ENOTSUP)),
          "TC-MM-EXTRA", "未激活 set_attrs/query ⇒ -ENOTSUP");

    /* 注册状态机 */
    const int reg1 = br_mm_register(&s_ops);
    const int reg2 = br_mm_register(&s_ops);
    CHECK(reg1 == 0, "TC-MM-EXTRA", "首次 register 成功");
    CHECK(reg2 == BR_ERR(BR_EBUSY), "TC-MM-EXTRA", "二次 register ⇒ -EBUSY");

    const int act1 = br_mm_activate();
    const int act2 = br_mm_activate();
    printf("[HOSTTEST]   activate=%d again=%d active=%d count_at_activate=%u\n",
           act1, act2, (int)br_mm_active(), (unsigned)s_ops_stats.activate_count);
    CHECK(act1 == 0, "TC-MM-EXTRA", "activate 成功");
    CHECK(act2 == BR_ERR(BR_EBUSY), "TC-MM-EXTRA", "二次 activate ⇒ -EBUSY");
    CHECK(br_mm_active() == BR_TRUE, "TC-MM-EXTRA", "br_mm_active() 置位");
    CHECK(s_ops_stats.activate_count == br_mm_region_count(), "TC-MM-EXTRA",
          "activate 收到当前 region 表快照");

    /* 未声明地址 ⇒ -EINVAL(即使已激活) */
    const int r_un = br_mm_cache_flush((void *)(br_uintptr_t)0x1000u, 4096u);
    printf("[HOSTTEST]   未声明地址 flush=%d\n", r_un);
    CHECK(r_un == BR_ERR(BR_EINVAL), "TC-MM-EXTRA", "未声明地址 ⇒ -EINVAL");

    /* 已激活 + CACHED ⇒ 派发 ops; 非 CACHED ⇒ 不派发 */
    const br_u32 f0 = s_ops_stats.flush_calls;
    const int rc_cached = br_mm_cache_flush((void *)g_heap_base, 4096u);
    const br_u32 f1 = s_ops_stats.flush_calls;
    const int rc_nc = br_mm_cache_flush((void *)g_dma_base, 4096u);
    const br_u32 f2 = s_ops_stats.flush_calls;
    printf("[HOSTTEST]   cached flush=%d calls=%u->%u ; noncached=%d calls=%u\n",
           rc_cached, (unsigned)f0, (unsigned)f1, rc_nc, (unsigned)f2);
    CHECK((rc_cached == 0) && (f1 == (f0 + 1u)), "TC-MM-EXTRA",
          "CACHED region 经 ops 派发");
    CHECK((rc_nc == 0) && (f2 == f1), "TC-MM-EXTRA", "非 CACHED 不派发");

    /* set_attrs / query 派发 */
    attrs = 0u;
    const int rc_sa = br_mm_set_attrs(g_heap_base, 4096u, BR_MM_RO);
    const int rc_q  = br_mm_query(g_heap_base, 4096u, &attrs);
    CHECK((rc_sa == 0) && (rc_q == 0) && (attrs == BR_MM_CACHED), "TC-MM-EXTRA",
          "set_attrs/query 经 ops 派发");

    /* 落表失败 ⇒ 回滚表项(不留第二真值) */
    const br_uintptr_t fail_at = (br_uintptr_t)(void *)g_regspace;
    s_ops_stats.fail_base = fail_at;
    const br_u32 count_before = br_mm_region_count();
    const int rc_fail = add_region(fail_at, 4096u, BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
    const br_u32 count_after = br_mm_region_count();
    s_ops_stats.fail_base = 0u;
    printf("[HOSTTEST]   map_region 失败 rc=%d count %u -> %u\n",
           rc_fail, (unsigned)count_before, (unsigned)count_after);
    CHECK((rc_fail == BR_ERR(BR_EIO)) && (count_after == count_before),
          "TC-MM-EXTRA", "map_region 失败即回滚 region 表项");

    /* 表满 ⇒ -ENOSPC(用一个不与已有 region 重叠的新区) */
    br_u32 i = 0u;
    while ((br_mm_region_count() < (br_u32)BR_MM_MAX_REGIONS) && (i < 32u)) {
        const int rc = add_region((br_uintptr_t)(void *)g_regspace + (br_uintptr_t)(i * 4096u),
                                  4096u, BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
        if (rc != 0) {
            printf("[HOSTTEST]   填充 region 第 %u 条失败 rc=%d\n", (unsigned)i, rc);
            break;
        }
        i++;
    }
    const br_uintptr_t over_at =
        (br_uintptr_t)(void *)g_regspace + (br_uintptr_t)(i * 4096u);
    const int rc_full = add_region(over_at, 4096u, BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
    printf("[HOSTTEST]   count=%u (max=%u) over=%d map_calls=%u\n",
           (unsigned)br_mm_region_count(), (unsigned)BR_MM_MAX_REGIONS,
           rc_full, (unsigned)s_ops_stats.map_calls);
    CHECK(br_mm_region_count() == (br_u32)BR_MM_MAX_REGIONS, "TC-MM-EXTRA",
          "region 表填满");
    CHECK(rc_full == BR_ERR(BR_ENOSPC), "TC-MM-EXTRA", "表满 ⇒ -ENOSPC");
}

/* =====================================================================
 * 池几何观测
 * ===================================================================== */

static void test_layout(void)
{
    section("TC-MEM-LAYOUT", "br_mem_layout / br_mem_ready");

    br_mem_layout_t l;
    const int rc = br_mem_layout(&l);
    printf("[HOSTTEST]   heap=%lu contig=%lu page=%lu(%u pages) dma=%lu ready=%d\n",
           (unsigned long)l.heap.size, (unsigned long)l.contig.size,
           (unsigned long)l.page.size, (unsigned)l.page_total,
           (unsigned long)l.dma.size, (int)br_mem_ready());

    CHECK(rc == 0, "TC-MEM-LAYOUT", "br_mem_layout 成功");
    CHECK(l.heap.size == SZ_HEAP, "TC-MEM-LAYOUT", "heap 池尺寸 = region 声明");
    CHECK(l.page_total == (br_u32)(SZ_PAGE / BR_PAGE_SIZE), "TC-MEM-LAYOUT",
          "page_total 按 4 KiB 粒度换算");
    CHECK(br_mem_ready() == BR_TRUE, "TC-MEM-LAYOUT", "br_mem_ready() == TRUE");
}

/* =====================================================================
 * main
 * ===================================================================== */

int main(void)
{
    printf("[HOSTTEST] brickOS prototype — 宿主侧内存语义门禁\n");

    const int rsetup = setup_regions();
    printf("[HOSTTEST] region 声明 rc=%d count=%u\n",
           rsetup, (unsigned)br_mm_region_count());
    if (rsetup != 0) {
        printf("[HOSTTEST] FAIL SETUP region 声明失败\n");
        printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d\n",
               s_pass, s_fail + 1, s_pass + s_fail + 1);
        return 1;
    }

    /* 幂等: 二次调用返回首次结果 */
    const int init1 = br_mem_init();
    const int init2 = br_mem_init();
    printf("[HOSTTEST] br_mem_init rc=%d again=%d\n", init1, init2);
    CHECK(init1 == 0, "TC-MEM-INIT", "br_mem_init 成功");
    CHECK(init2 == init1, "TC-MEM-INIT", "br_mem_init 幂等");

    test_layout();
    test_mem_001_align();
    test_mem_007_usage();
    test_mem_003_calloc_realloc();
    test_mem_004_contig();
    test_mem_005_page();
    test_mem_006_dma();
    test_page_exhaust();
    test_leak_unit();
    test_tlsf_direct();
    test_walk();
    test_owner();

    test_mm_001_region();
    test_mm_002_cache();
    test_mm_003_map();
    test_mm_extra();

    /* 压力放最后: 它把堆清空, 便于验证合并回单一空闲块 */
    test_mem_002_stress();

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d\n",
           s_pass, s_fail, s_pass + s_fail);
    return (s_fail == 0) ? 0 : 1;
}
