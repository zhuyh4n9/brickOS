/*
 * brickOS prototype v0.1.0 — memory mapping (MMU / region table / cache) contract
 *
 * Authoritative design: `docs/3-os-core/3-01-core-api-list.md` §7 (br-mm group, 5 functions)
 * + §6 (page pool) + §13.6 (privilege grading's memory axes: ops × granularity × regions)
 * + `docs/3-os-core/3-04-memory.md` (three pools + region table + virtual memory policy).
 *
 * Three-layer pattern (design 1-01 §8): **interface in core / construction in platform
 * (the ISA shared library in the design) / data in the platform region table**. So this
 * header has two halves:
 *   ① native face (§7's 5 functions: region_add / map / unmap / cache_flush /
 *      cache_invalidate) — signatures unchanged from the design;
 *   ② platform face (`br_mm_ops_t` + `br_mm_register`, same pattern as `br_pic_ops`)
 *      — platform fills in the "page-table construction" table; core only dispatches and
 *      keeps consistency.
 *
 * v1 virtual memory policy = **identity mapping + attribute isolation** (3-04 §1): the
 * static region table is declared in platform early_init, and the page table is built once
 * by `br_mm_ops.activate()`. Hence
 *   - `br_mm_map`/`br_mm_unmap` **return -ENOTSUP** in v1 (signatures come first, the
 *     implementation belongs to v2 relocation; case `TC-MM-003` is exactly this:
 *     "map/unmap(v1) = -ENOTSUP, signature-first validation of main-doc risk R4");
 *   - runtime attribute changes go through `br_mm_set_attrs()` (→ `ops.set_attrs`) — the
 *     same mechanical path v2 relocation needs, used in v1 by platform consistency cases
 *     for RO page protection validation.
 */
#ifndef BR_CORE_BR_MM_H
#define BR_CORE_BR_MM_H

#include <br/core/br_types.h>

/* ---------------------------------------------------------------- region attributes
 * Design 3-01 §7: BR_MM_RO | BR_MM_NX | BR_MM_DEVICE | BR_MM_CACHED (**append-only, D14**).
 * The 4 bits below are fixed by the design; new bits may only be appended, never redefined.
 */
#define BR_MM_RO      0x0001u   /* read-only (write ⇒ permission fault) */
#define BR_MM_NX      0x0002u   /* non-executable (instruction fetch ⇒ instruction abort) */
#define BR_MM_DEVICE  0x0004u   /* device memory (Device-nGnRnE; unaligned access faults) */
#define BR_MM_CACHED  0x0008u   /* cacheable (Normal WB; cache maintenance is meaningful) */

/*
 * region **kind** (bits 8..11): turns the `regions` axis of 3-01 §13.6
 * (heap | dma | contig | page | mmio | reserved) from a declaration into an executable
 * form in the region table. The three-pool split (3-01 §6 "the platform region table
 * splits RAM into heap / contig pool / page pool") is expressed exactly this way:
 * `br_mem_init()` claims its pools by kind, and the platform only declares "which RAM
 * is which pool".
 * ★ This is a prototype **addition** (design §13.6 defines only the value domain, not the
 *   carrier), registered in
 *   `docs/decisions/0003-memory-map-heap-and-debug-plugins.md` as an increment.
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

/* region table capacity. The prototype uses a static bound (design 3-04 §2: static
 * region table; manifest trimming is v1.x). */
#define BR_MM_MAX_REGIONS   24u

/* One region descriptor (design 3-01 §7 verbatim: base/size/attrs) */
typedef struct {
    br_uintptr_t base;
    br_size_t    size;
    br_u32       attrs;     /* BR_MM_RO|NX|DEVICE|CACHED + kind */
} br_mm_region_t;

/* ---------------------------------------------------------------- native face */

/*
 * platform early_init declares one identity region (design 3-01 §7 / §10: owned by Platform).
 * Semantics: pure registration (no page-table touch) — the page table is built in
 * `br_mm_ops.activate()` from the table **as of that moment**.
 * Errors: `-EINVAL` (r==NULL / size==0 / base not page aligned / size not page granular)
 *       `-EEXIST` (overlaps or duplicates an existing region — case TC-MM-001)
 *       `-ENOSPC` (table full)
 * Regions appended after activation are **also** mapped via `ops.map_region` (the table and
 * the page table keep no second truth).
 */
int br_mm_region_add(const br_mm_region_t *r);

/* Runtime map/attribute change (design 3-01 §7). v1 has signatures only, no
 * implementation ⇒ always `-ENOTSUP` (TC-MM-003). */
int br_mm_map  (const br_mm_region_t *r);
int br_mm_unmap(br_uintptr_t addr, br_size_t size);

/*
 * cache maintenance (design 3-01 §7; around driver DMA, risk R4).
 * Semantics (per the orthogonal table in 3-01 §13.6, `flush`/`invalidate` are P2 and
 * `granularity` may be byte):
 *   - hitting a **non-cacheable** region (DEVICE or not marked CACHED) ⇒ no-op, return 0
 *     (case TC-MM-002);
 *   - MMU not activated (cache was never on) ⇒ no-op, return 0;
 *   - address not declared ⇒ `-EINVAL` (the prototype deliberately does not guess
 *     attributes; see ADR-0003).
 */
int br_mm_cache_flush     (void *addr, br_size_t size);
int br_mm_cache_invalidate(void *addr, br_size_t size);

/* ---------------------------------------------------------------- read-only observation face
 * The three query functions are an **observation contract** (the ramdump/dump plugins
 * enumerate the region table); they are not part of the 48-function native face of 3-01
 * — registered in ADR-0003 as an increment (same treatment as 3-02 §17.4 gives
 * `br_irq_stats_get`: the consumer is a debug plugin ⇒ it cannot hide inside core).
 */
br_u32                br_mm_region_count(void);
const br_mm_region_t *br_mm_region_get(br_u32 index);          /* out of range ⇒ NULL */
const br_mm_region_t *br_mm_region_find(br_uintptr_t addr);    /* undeclared ⇒ NULL */
br_u32                br_mm_region_count_kind(br_u32 kind);

/* ---------------------------------------------------------------- platform face
 * Page-table construction ops (same pattern as 3-01 §10's "`br_pic` ops table (interrupt
 * controller implementation) | Platform plugin (fills the table)"): core defines the
 * shape, platform fills the implementation.
 */
typedef struct br_mm_ops {
    /* Build the page table + install TTBR/MAIR/TCR + enable MMU (SCTLR.M).
     * regions/count = snapshot of core's region table. */
    int  (*activate)(const br_mm_region_t *regions, br_u32 count);

    /* Map one region (called by `br_mm_region_add` after activation; also for platform use) */
    int  (*map_region)(const br_mm_region_t *r);

    /* Change attributes of an already-mapped range (shared path for relocation and
     * protection validation) */
    int  (*set_attrs)(br_uintptr_t addr, br_size_t size, br_u32 attrs);

    /* Read back the currently effective attributes of a range (**page-table walk**,
     * not a region-table lookup) */
    int  (*query)(br_uintptr_t addr, br_size_t size, br_u32 *out_attrs);

    void (*cache_flush)(void *addr, br_size_t size);
    void (*cache_invalidate)(void *addr, br_size_t size);
} br_mm_ops_t;

/* Platform-side one-time registration (duplicate registration ⇒ the later one is
 * rejected with -EBUSY).
 * Called by the platform plugin's `early_init` in boot-chain phase ② (registers the
 * mechanism only, builds no table). */
int br_mm_register(const br_mm_ops_t *ops);

/*
 * Build the table and enable the MMU — the address-mapping **construction action** —
 * called by the core entry `br_core_main()` after `br_mem_init()` (boot-chain phase ③,
 * ADR-0008).
 * Semantics: hand the **current** region-table snapshot to `ops.activate()` (page-table
 * construction mechanism lives in platform); on success (returns 0) core records
 * "activated", and `br_mm_active()` is true thereafter.
 * ★ Once the MMU is on, every fetch and access goes through translation ⇒ this must be the
 *   last step before plugin init.
 * Errors: `-ENOTSUP` (ops not registered) / `-EBUSY` (already active) / `-ENOMEM`
 *        (not enough page-table memory) / others = ops return value passed through.
 * Why "activate" is not inside ops: that is a **state machine** (unregistered→registered→
 * activated) ownership question — state and orchestration belong to core, construction
 * mechanism to platform (the three-layer pattern boundary).
 */
int br_mm_activate(void);

/* Whether the MMU has been enabled by `br_mm_activate()`. */
br_bool br_mm_active(void);

/* core-side dispatch for the ops above (not registered / not active ⇒ `-ENOTSUP`). */
int br_mm_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs);
int br_mm_query    (br_uintptr_t addr, br_size_t size, br_u32 *out_attrs);

#endif /* BR_CORE_BR_MM_H */
