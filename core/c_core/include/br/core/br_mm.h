/*
 * Memory mapping (MMU / region table / cache) contract. Core owns the interface and state, the
 * platform supplies page-table construction via br_mm_ops_t and the region data. v1 policy is
 * identity mapping + attribute isolation: the table is declared in platform early_init and built
 * once by ops.activate(), so br_mm_map/br_mm_unmap return -ENOTSUP (TC-MM-003).
 */
#ifndef BR_CORE_BR_MM_H
#define BR_CORE_BR_MM_H

#include <br/core/br_types.h>

/* ---- region attributes (append-only) ----
 * BR_MM_RO | NX | DEVICE | CACHED; new bits may only be appended, never redefined. */
#define BR_MM_RO      0x0001u   /* read-only (write ⇒ permission fault) */
#define BR_MM_NX      0x0002u   /* non-executable (instruction fetch ⇒ instruction abort) */
#define BR_MM_DEVICE  0x0004u   /* device memory (Device-nGnRnE; unaligned access faults) */
#define BR_MM_CACHED  0x0008u   /* cacheable (Normal WB; cache maintenance is meaningful) */

/*
 * Region **kind** (bits 8..11): the executable form of the regions axis (heap | dma | contig |
 * page | mmio | reserved). br_mem_init() claims its pools by kind; the platform declares which
 * RAM is which pool.
 */
#define BR_MM_KIND_SHIFT   8u
#define BR_MM_KIND_MASK    0x00000F00u
#define BR_MM_KIND_NONE    0u
#define BR_MM_KIND_IMAGE   1u   /* image (text/rodata/data/bss) */
#define BR_MM_KIND_STACK   2u   /* boot stack */
#define BR_MM_KIND_HEAP    3u   /* TLSF heap pool (byte pool outside CA-7) */
#define BR_MM_KIND_CONTIG  4u   /* physically contiguous pool (CA-7) */
#define BR_MM_KIND_PAGE    5u   /* page pool (CA-8) */
#define BR_MM_KIND_DMA     6u   /* DMA buffer pool (CA-6: built on contig semantics) */
#define BR_MM_KIND_MMIO    7u   /* register window (Device) */
#define BR_MM_KIND_RESERVED 8u  /* reserved (given to no pool) */

#define BR_MM_KIND(a)          (((a) & BR_MM_KIND_MASK) >> BR_MM_KIND_SHIFT)
#define BR_MM_WITH_KIND(a, k)  (((a) & ~BR_MM_KIND_MASK) | (((k) << BR_MM_KIND_SHIFT) & BR_MM_KIND_MASK))

/* Region table capacity (static bound; manifest trimming is v1.x). */
#define BR_MM_MAX_REGIONS   24u

/* One region descriptor (base/size/attrs) */
typedef struct {
    br_uintptr_t base;
    br_size_t    size;
    br_u32       attrs;     /* BR_MM_RO|NX|DEVICE|CACHED + kind */
} br_mm_region_t;

/* ---- native face ---- */

/*
 * Platform early_init declares one identity region (owned by platform). Pure registration, no
 * page-table touch: activate() builds from the snapshot as of that moment. Errors: -EINVAL
 * (r NULL / size 0 / base unaligned / size not page granular), -EEXIST (overlap or duplicate,
 * TC-MM-001), -ENOSPC (table full). Regions added after activation are also mapped via
 * ops.map_region, so the table keeps no second truth.
 */
int br_mm_region_add(const br_mm_region_t *r);

/* Runtime map/unmap; v1 has signatures only => always -ENOTSUP (TC-MM-003). */
int br_mm_map  (const br_mm_region_t *r);
int br_mm_unmap(br_uintptr_t addr, br_size_t size);

/*
 * Cache maintenance, used around driver DMA. No-op returning 0 when the region is non-cacheable
 * (DEVICE or not CACHED, TC-MM-002) or the MMU is not activated; -EINVAL if the address is
 * undeclared (attributes are never guessed).
 */
int br_mm_cache_flush     (void *addr, br_size_t size);
int br_mm_cache_invalidate(void *addr, br_size_t size);

/* ---- read-only observation face (ramdump/dump plugins enumerate the region table) ---- */
br_u32                br_mm_region_count(void);
const br_mm_region_t *br_mm_region_get(br_u32 index);          /* out of range ⇒ NULL */
const br_mm_region_t *br_mm_region_find(br_uintptr_t addr);    /* undeclared ⇒ NULL */
br_u32                br_mm_region_count_kind(br_u32 kind);

/* ---- platform face: page-table construction ops (core defines shape, platform fills) ---- */
typedef struct br_mm_ops {
    /* Build the page table + install TTBR/MAIR/TCR + enable MMU (SCTLR.M); regions/count =
     * snapshot of core's region table. */
    int  (*activate)(const br_mm_region_t *regions, br_u32 count);

    /* Map one region (called by `br_mm_region_add` after activation; also for platform use) */
    int  (*map_region)(const br_mm_region_t *r);

    /* Change attributes of an already-mapped range (relocation and protection validation) */
    int  (*set_attrs)(br_uintptr_t addr, br_size_t size, br_u32 attrs);

    /* Read back effective attributes of a range (page-table walk, not a region-table lookup) */
    int  (*query)(br_uintptr_t addr, br_size_t size, br_u32 *out_attrs);

    void (*cache_flush)(void *addr, br_size_t size);
    void (*cache_invalidate)(void *addr, br_size_t size);
} br_mm_ops_t;

/* Platform-side one-time registration (duplicate => the later one is rejected with -EBUSY).
 * Called by the platform plugin's `early_init`; registers the mechanism only, builds no table. */
int br_mm_register(const br_mm_ops_t *ops);

/*
 * Build the table and enable the MMU; called by br_core_main() after br_mem_init(). Hands the
 * current region-table snapshot to ops.activate() and, on success, marks the MMU active. Every
 * fetch goes through translation after this, so it must be the last step before plugin init.
 * Errors: -ENOTSUP (ops not registered), -EBUSY (already active), -ENOMEM (page-table memory),
 * else the ops return value.
 */
int br_mm_activate(void);

/* Whether the MMU has been enabled by `br_mm_activate()`. */
br_bool br_mm_active(void);

/* Core-side dispatch for the ops above (not registered / not active ⇒ -ENOTSUP). */
int br_mm_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs);
int br_mm_query    (br_uintptr_t addr, br_size_t size, br_u32 *out_attrs);

#endif /* BR_CORE_BR_MM_H */
