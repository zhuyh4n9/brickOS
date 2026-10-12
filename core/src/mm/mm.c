/*
 * brickOS prototype v0.1.0 — MMU / region table / cache dispatch (mm.c)
 *
 * Authoritative design: `docs/3-os-core/3-01-core-api-list.md` §7 (the five br-mm functions) +
 *   §13.6 (three privilege-tier memory axes: ops × granularity × regions) +
 *   `docs/3-os-core/3-04-memory.md` (identity map + attribute isolation; static region table).
 *
 * Three-layer model (design 1-01 §8): **interface in core / construction in platform / data in
 * the platform region table**. This file is the two core-side pieces:
 *   ① the region table (static array, BR_MM_MAX_REGIONS; single source of truth for duplicates);
 *   ② dispatch to `br_mm_ops_t` (state machine in core, page-table construction in platform).
 *
 * v1 virtual-memory policy = identity map + attribute isolation (3-04 §1):
 *   - `br_mm_map`/`br_mm_unmap` are **signature first** (main doc risk R4), always -ENOTSUP in
 *     v1 (TC-MM-003);
 *   - the region table is declared in platform early_init; page tables are built in one pass by
 *     `ops.activate()`;
 *   - a region added after activation is **also** placed via `ops.map_region`; if that fails the
 *     table entry is rolled back, never leaving a second truth "table and page table disagree"
 *     (design §7's consistency obligation).
 *
 * No locking: v0.1 is single-core, and the region table and active flag are touched only during
 * boot / consistency cases (thread-only).
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>   /* BR_PAGE_SIZE: region granularity and pool granularity share a source (§6/§7) */
#include <br/core/br_mm.h>
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

/* ---------------------------------------------------------------- native surface (§7) */

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

    /* Overlap test: half-open intervals intersect. **An exact duplicate counts as overlap**
     * (TC-MM-001 requires both "overlap" and "duplicate" to fail), because it would give one
     * physical page two attribute declarations — the page table honors one, the other is a
     * second truth. */
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

    /* Already active: table and page table must grow together; on failure roll back the entry (never "in the table but not in the page table"). */
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

/*
 * Cache maintenance (§7; around driver DMA, risk R4). Semantics per §13.6's orthogonal table:
 *   ① address not declared ⇒ -EINVAL (the prototype deliberately does not "guess" attributes —
 *      see ADR-0003, TC-MM-EXTRA);
 *   ② size == 0 ⇒ 0 (no bytes to maintain);
 *   ③ region not marked CACHED (DEVICE/plain) ⇒ 0 (no-op);
 *   ④ MMU not active (caches are off anyway) ⇒ 0;
 *   ⑤ otherwise dispatch via ops; ops missing ⇒ -ENOTSUP.
 */
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
        return rc;   /* propagate as is, do **not** set the flag: activation can be retried after failure */
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
