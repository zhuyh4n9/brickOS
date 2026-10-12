/*
 * brickOS prototype v0.1.0 — page allocator (static bitmap + first-fit contiguous run)
 *
 * Authoritative design: `docs/3-os-core/3-01-core-api-list.md` §6 (page pool) + §14 **CA-8**
 * ("page allocator = static bitmap + first-fit contiguous run; `BR_PAGE_SIZE` is a platform
 * compile-time constant"). Where the pool comes from: the platform declares it in the region
 * table as the `BR_MM_KIND_PAGE` entry, and `br_mem_init()` hands base/size to this file via
 * `br_page_pool_init()` (the "three pools divided by the region table" of
 * `docs/3-os-core/3-04-memory.md`).
 *
 * Bitmap upper bound (static, independent of the region table): BR_PAGE_MAX_PAGES = 4096 pages
 *   = 4096 × BR_PAGE_SIZE(4 KiB) = **16 MiB pool upper bound**; the bitmap itself is 512 B.
 * Why this number: the bitmap word count must be a compile-time constant (CA-8 "static"), and
 * the prototype's three pools total far less than 16 MiB; exceeding the bound makes
 * `br_page_pool_init()` return -ENOMEM rather than truncate silently.
 *
 * v1 semantics (§6 text + 3-04 §1):
 *   - `BR_PAGE_F_CONTIG` is always true (identity mapping + single pool); setting it neither
 *     errors nor changes the path;
 *   - `BR_PAGE_F_ZERO` zeroes byte by byte (no libc, self-contained loop);
 *   - runs never cross the pool end (first fit scans from low addresses; not found = failure).
 * Invariants: bitmap 1 = allocated; `s_page.used` always equals the number of set bits; the
 * only reading of free runs is max_run computed on demand (O(total pages) scan, observation
 * path, not hot).
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

/* ---------------------------------------------------------------- bit ops */

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

/* Byte-by-byte zeroing without libc. */
static void page_zero(br_u8 *p, br_size_t n)
{
    for (br_size_t i = 0u; i < n; i++) {
        p[i] = 0u;
    }
}

/* ---------------------------------------------------------------- internal setup */

int br_page_pool_init(br_uintptr_t base, br_size_t size)
{
    if ((base == 0u) ||
        ((base & (br_uintptr_t)(BR_PAGE_SIZE - 1u)) != 0u)) {
        return BR_ERR(EINVAL);
    }
    if ((size == 0u) || ((size & (br_size_t)(BR_PAGE_SIZE - 1u)) != 0u)) {
        return BR_ERR(EINVAL);
    }

    const br_u32 pages = (br_u32)(size / (br_size_t)BR_PAGE_SIZE);
    if (pages > (br_u32)BR_PAGE_MAX_PAGES) {
        return BR_ERR(ENOMEM);
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

/* ---------------------------------------------------------------- public surface (§6) */

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

    /* First fit: scan from low addresses for the first run holding need pages, no pool-end crossing. */
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
    /* BR_PAGE_F_CONTIG: always true in this pool (identity mapping + single pool), deliberately not branched. */
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
        s_page.fail++;                       /* not page-aligned */
        return;
    }
    if ((a < s_page.base) || (a >= (s_page.base + s_page.size))) {
        s_page.fail++;                       /* not in this pool */
        return;
    }

    const br_u32 start = (br_u32)((a - s_page.base) / (br_uintptr_t)BR_PAGE_SIZE);
    if (n_pages > (br_size_t)(s_page.pages - start)) {
        s_page.fail++;                       /* past the pool end */
        return;
    }

    const br_u32 n = (br_u32)n_pages;
    for (br_u32 k = 0u; k < n; k++) {
        if (bit_test(start + k) == BR_FALSE) {
            s_page.fail++;                   /* double free / free span contains unallocated page: ignore */
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
        return BR_ERR(EINVAL);
    }
    if (s_page.ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }

    /* Current largest contiguous free run: observation path, linear scan suffices. */
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
