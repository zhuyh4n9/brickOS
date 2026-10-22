/*
 * MMU / region table / cache dispatch: static region table (BR_MM_MAX_REGIONS) in core, page
 * tables in platform via br_mm_ops_t. v1 = identity map + attribute isolation: br_mm_map/unmap
 * always return -ENOTSUP, and a post-activation add rolls back if ops.map_region fails, so table
 * and page table never disagree. No locking (single-core, boot/consistency cases only).
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>
#include <br/hal/br_mm.h>
#include <br/core/br_types.h>

static br_mm_region_t    s_regions[BR_MM_MAX_REGIONS];
static br_u32            s_count;
static const br_mm_ops_t *s_ops;
static br_bool           s_active;

/* ---------------------------------------------------------------- read-only observation surface */

br_u32 br_mm_region_count(void)
{
    return s_count;
}

const br_mm_region_t *br_mm_region_get(br_u32 index)
{
    if (index >= s_count) {
        return BR_NULL;
    }
    return &s_regions[index];
}

const br_mm_region_t *br_mm_region_find(br_uintptr_t addr)
{
    for (br_u32 i = 0u; i < s_count; i++) {
        const br_mm_region_t *r = &s_regions[i];
        if ((addr >= r->base) && (addr < (r->base + r->size))) {
            return r;
        }
    }
    return BR_NULL;
}

br_u32 br_mm_region_count_kind(br_u32 kind)
{
    br_u32 n = 0u;
    for (br_u32 i = 0u; i < s_count; i++) {
        if (BR_MM_KIND(s_regions[i].attrs) == kind) {
            n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------- native surface */

int br_mm_region_add(const br_mm_region_t *r)
{
    if ((r == BR_NULL) || (r->size == 0u)) {
        return BR_ERR(EINVAL);
    }
    if (((r->base & (br_uintptr_t)(BR_PAGE_SIZE - 1u)) != 0u) ||
        ((r->size & (br_size_t)(BR_PAGE_SIZE - 1u)) != 0u)) {
        return BR_ERR(EINVAL);
    }
    if ((r->base + r->size) < r->base) {
        return BR_ERR(EINVAL);   /* address wrap */
    }

    /* Overlap test on half-open intervals; an exact duplicate counts as overlap (TC-MM-001),
     * since it would give one physical page two attribute declarations. */
    const br_uintptr_t nlo = r->base;
    const br_uintptr_t nhi = r->base + r->size;
    for (br_u32 i = 0u; i < s_count; i++) {
        const br_uintptr_t lo = s_regions[i].base;
        const br_uintptr_t hi = s_regions[i].base + s_regions[i].size;
        if ((nlo < hi) && (lo < nhi)) {
            return BR_ERR(EEXIST);
        }
    }

    if (s_count >= (br_u32)BR_MM_MAX_REGIONS) {
        return BR_ERR(ENOSPC);
    }

    s_regions[s_count] = *r;
    s_count++;

    /* Active: table and page table must grow together; roll back the entry on failure. */
    if (s_active == BR_TRUE) {
        if ((s_ops == BR_NULL) || (s_ops->map_region == BR_NULL)) {
            s_count--;
            return BR_ERR(ENOTSUP);
        }
        const int rc = s_ops->map_region(r);
        if (rc != BR_OK) {
            s_count--;
            br_log_error("mem/mm: map_region failed rc=%d, region entry rolled back", rc);
            return rc;
        }
    }
    return BR_OK;
}

int br_mm_map(const br_mm_region_t *r)
{
    (void)r;
    return BR_ERR(ENOTSUP);   /* v1 signature first (TC-MM-003) */
}

int br_mm_unmap(br_uintptr_t addr, br_size_t size)
{
    (void)addr;
    (void)size;
    return BR_ERR(ENOTSUP);   /* v1 signature first (TC-MM-003) */
}

/* Cache maintenance around driver DMA. size 0 => 0; address not declared => -EINVAL (TC-MM-EXTRA);
 * region not CACHED or MMU inactive => 0; otherwise dispatch, missing op => -ENOTSUP. */
static int mm_cache_dispatch(void *addr, br_size_t size, br_bool flush)
{
    if (size == 0u) {
        return BR_OK;
    }
    const br_mm_region_t *r = br_mm_region_find((br_uintptr_t)addr);
    if (r == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if ((r->attrs & BR_MM_CACHED) == 0u) {
        return BR_OK;
    }
    if (s_active == BR_FALSE) {
        return BR_OK;
    }
    if (s_ops == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    if (flush == BR_TRUE) {
        if (s_ops->cache_flush == BR_NULL) {
            return BR_ERR(ENOTSUP);
        }
        s_ops->cache_flush(addr, size);
    } else {
        if (s_ops->cache_invalidate == BR_NULL) {
            return BR_ERR(ENOTSUP);
        }
        s_ops->cache_invalidate(addr, size);
    }
    return BR_OK;
}

int br_mm_cache_flush(void *addr, br_size_t size)
{
    return mm_cache_dispatch(addr, size, BR_TRUE);
}

int br_mm_cache_invalidate(void *addr, br_size_t size)
{
    return mm_cache_dispatch(addr, size, BR_FALSE);
}

/* ---------------------------------------------------------------- platform surface */

int br_mm_register(const br_mm_ops_t *ops)
{
    if (ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (s_ops != BR_NULL) {
        return BR_ERR(EBUSY);   /* exactly one platform page-table builder per image */
    }
    s_ops = ops;
    return BR_OK;
}

int br_mm_activate(void)
{
    if ((s_ops == BR_NULL) || (s_ops->activate == BR_NULL)) {
        return BR_ERR(ENOTSUP);   /* not registered */
    }
    if (s_active == BR_TRUE) {
        return BR_ERR(EBUSY);
    }

    const int rc = s_ops->activate(s_regions, s_count);
    if (rc != BR_OK) {
        return rc;   /* propagate as is, do not set the flag: activation can be retried after failure */
    }
    s_active = BR_TRUE;
    return BR_OK;
}

br_bool br_mm_active(void)
{
    return s_active;
}

int br_mm_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs)
{
    if ((s_active == BR_FALSE) || (s_ops == BR_NULL) || (s_ops->set_attrs == BR_NULL)) {
        return BR_ERR(ENOTSUP);
    }
    return s_ops->set_attrs(addr, size, attrs);
}

int br_mm_query(br_uintptr_t addr, br_size_t size, br_u32 *out_attrs)
{
    if (out_attrs == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if ((s_active == BR_FALSE) || (s_ops == BR_NULL) || (s_ops->query == BR_NULL)) {
        return BR_ERR(ENOTSUP);
    }
    return s_ops->query(addr, size, out_attrs);
}
