/*
 * brickOS prototype v0.1.0 — 页分配器(静态位图 + 连续 run 首次适配)
 *
 * 权威设计: `docs/3-os-core/3-01-core-api-list.md` §6(页池)+ §14 **CA-8**
 * ("页分配器 = 静态位图 + 连续 run 首次适配; `BR_PAGE_SIZE` 平台编译期常量")。
 * 池从哪来: platform 在 region 表里声明 `BR_MM_KIND_PAGE` 那条, `br_mem_init()`
 * 经 `br_page_pool_init()` 把 base/size 交给本文件(`docs/3-os-core/3-04-memory.md`
 * 的"三池由 region 表划分")。
 *
 * 位图上界(静态, 与 region 表无关): BR_PAGE_MAX_PAGES = 4096 页
 *   = 4096 × BR_PAGE_SIZE(4 KiB) = **16 MiB 池上界**; 位图本体 512 B。
 * 为什么取这个数: 位图字数必须是编译期常量(CA-8 的"静态"), 而原型三池合计
 * 远小于 16 MiB; 超过上界由 `br_page_pool_init()` 返回 -ENOMEM, 不静默截断。
 *
 * v1 语义(§6 原文 + 3-04 §1):
 *   - `BR_PAGE_F_CONTIG` 恒真(恒等映射 + 单池), 置位不报错、不改变路径;
 *   - `BR_PAGE_F_ZERO` 逐字节清零(无 libc, 自持循环);
 *   - run 不跨池尾(首次适配从低地址起找, 找不到即失败)。
 * 不变量: bitmap 的 1 = 已分配; `s_page.used` 恒等于被置位数; 空闲 run 不含洞
 * 之外的解释 —— max_run 按需求值(遍历 O(total pages), 观测面非热路径)。
 */
#include <br/core/br_error.h>
#include <br/core/br_mem.h>          /* BR_PAGE_SIZE / BR_PAGE_F_* / br_page_stats_t */
#include <br/core/br_types.h>

#include "tlsf_internal.h"           /* BR_PAGE_MAX_PAGES / br_page_pool_init */

#define BR_PAGE_BITMAP_WORDS   ((BR_PAGE_MAX_PAGES + 31u) / 32u)

static struct {
    br_uintptr_t base;
    br_size_t    size;
    br_u32       pages;
    br_bool      ready;
    br_u32       bitmap[BR_PAGE_BITMAP_WORDS];
    br_u32       used;
    br_u32       fail;
} s_page;

/* ---------------------------------------------------------------- 位操作 */

static br_bool bit_test(br_u32 idx)
{
    return ((s_page.bitmap[idx >> 5] >> (idx & 31u)) & 1u) != 0u ? BR_TRUE : BR_FALSE;
}

static void bit_set(br_u32 idx)
{
    s_page.bitmap[idx >> 5] |= (br_u32)1u << (idx & 31u);
}

static void bit_clr(br_u32 idx)
{
    s_page.bitmap[idx >> 5] &= ~((br_u32)1u << (idx & 31u));
}

/* 无 libc 的逐字节清零。 */
static void page_zero(br_u8 *p, br_size_t n)
{
    for (br_size_t i = 0u; i < n; i++) {
        p[i] = 0u;
    }
}

/* ---------------------------------------------------------------- 内部建立 */

int br_page_pool_init(br_uintptr_t base, br_size_t size)
{
    if ((base == 0u) ||
        ((base & (br_uintptr_t)(BR_PAGE_SIZE - 1u)) != 0u)) {
        return BR_ERR(BR_EINVAL);
    }
    if ((size == 0u) || ((size & (br_size_t)(BR_PAGE_SIZE - 1u)) != 0u)) {
        return BR_ERR(BR_EINVAL);
    }

    const br_u32 pages = (br_u32)(size / (br_size_t)BR_PAGE_SIZE);
    if (pages > (br_u32)BR_PAGE_MAX_PAGES) {
        return BR_ERR(BR_ENOMEM);
    }

    s_page.base  = base;
    s_page.size  = size;
    s_page.pages = pages;
    s_page.used  = 0u;
    s_page.fail  = 0u;
    for (br_u32 i = 0u; i < BR_PAGE_BITMAP_WORDS; i++) {
        s_page.bitmap[i] = 0u;
    }
    s_page.ready = BR_TRUE;
    return BR_OK;
}

/* ---------------------------------------------------------------- 公开面(§6) */

void *br_page_alloc(br_size_t n_pages, br_u32 attrs)
{
    if ((s_page.ready == BR_FALSE) || (n_pages == 0u) ||
        (n_pages > (br_size_t)s_page.pages)) {
        s_page.fail++;
        return BR_NULL;
    }

    const br_u32 need = (br_u32)n_pages;
    br_u32 start = 0u;
    br_bool found = BR_FALSE;

    /* 首次适配: 从低地址起扫第一个能容纳 need 页的连续 run, 不跨池尾。 */
    while ((start + need) <= s_page.pages) {
        br_bool run_free = BR_TRUE;
        for (br_u32 k = 0u; k < need; k++) {
            if (bit_test(start + k) == BR_TRUE) {
                run_free = BR_FALSE;
                break;
            }
        }
        if (run_free == BR_TRUE) {
            found = BR_TRUE;
            break;
        }
        start++;
    }

    if (found == BR_FALSE) {
        s_page.fail++;
        return BR_NULL;
    }

    for (br_u32 k = 0u; k < need; k++) {
        bit_set(start + k);
    }
    s_page.used += need;

    br_u8 *vaddr = (br_u8 *)(void *)(s_page.base +
                                     (br_uintptr_t)start * (br_uintptr_t)BR_PAGE_SIZE);

    if ((attrs & BR_PAGE_F_ZERO) != 0u) {
        page_zero(vaddr, (br_size_t)need * (br_size_t)BR_PAGE_SIZE);
    }
    /* BR_PAGE_F_CONTIG: 本池恒真(恒等映射 + 单池), 刻意不做分支。 */
    return (void *)vaddr;
}

void br_page_free(void *vaddr, br_size_t n_pages)
{
    if ((s_page.ready == BR_FALSE) || (vaddr == BR_NULL) || (n_pages == 0u)) {
        s_page.fail++;
        return;
    }

    const br_uintptr_t a = (br_uintptr_t)vaddr;
    if ((a & (br_uintptr_t)(BR_PAGE_SIZE - 1u)) != 0u) {
        s_page.fail++;                       /* 未按页对齐 */
        return;
    }
    if ((a < s_page.base) || (a >= (s_page.base + s_page.size))) {
        s_page.fail++;                       /* 不在本池 */
        return;
    }

    const br_u32 start = (br_u32)((a - s_page.base) / (br_uintptr_t)BR_PAGE_SIZE);
    if (n_pages > (br_size_t)(s_page.pages - start)) {
        s_page.fail++;                       /* 越过池尾 */
        return;
    }

    const br_u32 n = (br_u32)n_pages;
    for (br_u32 k = 0u; k < n; k++) {
        if (bit_test(start + k) == BR_FALSE) {
            s_page.fail++;                   /* 重复释放 / 区间含未分配页: 忽略 */
            return;
        }
    }

    for (br_u32 k = 0u; k < n; k++) {
        bit_clr(start + k);
    }
    s_page.used -= n;
}

int br_page_stats(br_page_stats_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (s_page.ready == BR_FALSE) {
        return BR_ERR(BR_ENODEV);
    }

    /* 当前最大连续空闲 run: 观测面, 线性扫描足够。 */
    br_u32 best = 0u;
    br_u32 run  = 0u;
    for (br_u32 i = 0u; i < s_page.pages; i++) {
        if (bit_test(i) == BR_FALSE) {
            run++;
            if (run > best) {
                best = run;
            }
        } else {
            run = 0u;
        }
    }

    out->total   = s_page.pages;
    out->used    = s_page.used;
    out->free    = s_page.pages - s_page.used;
    out->fail    = s_page.fail;
    out->max_run = best;
    return BR_OK;
}
