/*
 * brickOS prototype v0.1.0 — memory subsystem internal truth (core-private; **not exported**, CA-10)
 *
 * Two layers live here, both on the implementation side of
 * `docs/3-os-core/3-01-core-api-list.md` §6:
 *   ① the TLSF **parameter dictionary and single-instance interface** (tlsf.c). TLSF is
 *      deliberately shaped as "caller provides the arena, core holds one static control
 *      block" — the prototype has one heap only (§6: "v1 has only the global heap"), so the
 *      interface takes no handle: host cases and mem.c call the **same instance**.
 *      Why a "dictionary" and not scattered magic numbers: 32 first-level classes (2^5) × 32
 *      subdivisions each (2^5) is TLSF's O(1) premise; changing either makes the bitmap and
 *      mapping inconsistent — centralized here so `mapping_insert`/`mapping_search` and
 *      `fl_bitmap`/`sl_bitmap` share one source of truth.
 *   ② the page pool's internal setup interface (page.c). `br_mem_init()` must hand the
 *      base/size of the `BR_MM_KIND_PAGE` region-table entry to the page allocator (§6's
 *      "the platform region table splits RAM into heap / contig pool / page pool"); the public
 *      surface (§6) has alloc/free/stats only, no pool setup, so this entry is an internal
 *      contract.
 *
 * Ownership: core-private. Host cases (TC-MEM-002's `br_tlsf_check()==0`) include this file
 * directly to **stress the algorithm itself**, not to treat it as an external contract — the
 * external contract is br_mem.h/br_mm.h only.
 */
#ifndef BR_MEM_TLSF_INTERNAL_H
#define BR_MEM_TLSF_INTERNAL_H

#include <br/core/br_types.h>

/* =====================================================================
 * ① TLSF parameter dictionary (the TLSF heap of 3-01 §6)
 * ===================================================================== */

/* Block alignment = 16 B. Same origin as br_mem.h's BR_MALLOC_ALIGN(16): the TLSF block
 * stride must divide the user alignment, else mem.c's "user ptr = header + 32" is not 16-aligned. */
#define BR_TLSF_ALIGN_LOG2   4u
#define BR_TLSF_ALIGN        (1u << BR_TLSF_ALIGN_LOG2)

/* Two-level segregation: 32 first-level classes × 32 subdivisions each (2^5 × 2^5). */
#define BR_TLSF_FL_COUNT     32u
#define BR_TLSF_SL_LOG2      5u
#define BR_TLSF_SL_COUNT     (1u << BR_TLSF_SL_LOG2)

/* Block header = {size|free bit, prev_phys} (8 B each). */
#define BR_TLSF_HDR          16u
/* Min block = header 16 + free list 16. The free list stores the **block tail** (see tlsf.c notes). */
#define BR_TLSF_MIN_BLOCK    32u

/* Fixed per-block metadata overhead (excl. alignment slack): header + min payload. Used for min pool size. */
#define BR_TLSF_OVERHEAD     (BR_TLSF_HDR + BR_TLSF_MIN_BLOCK)

/* TLSF single-instance interface (3-01 §6; one heap in the prototype ⇒ no handle). */
br_size_t br_tlsf_overhead(void);
int       br_tlsf_init(void *arena, br_size_t bytes);
void     *br_tlsf_alloc(br_size_t n);
void      br_tlsf_free(void *p);
br_size_t br_tlsf_block_size(void *p);   /* actual bytes (incl. TLSF header); 0 if not this heap */
br_size_t br_tlsf_used(void);
br_size_t br_tlsf_free_bytes(void);
br_size_t br_tlsf_max_free(void);        /* largest free block **payload** (allocatable bytes) */
br_u32    br_tlsf_check(void);           /* violation count (0 = clean) */

/* =====================================================================
 * ② page pool internal setup (page.c)
 * ===================================================================== */

/* Static upper bound of the page-pool bitmap (CA-8's "static bitmap").
 * Where the bound comes from: the bitmap word count is a compile-time constant and cannot
 * grow with the region table — take 4096 pages, i.e. 4096 × 4 KiB = 16 MiB pool bound
 * (bitmap itself 512 B). The prototype's three pools total far less; past the bound
 * `br_page_pool_init()` returns -ENOMEM instead of silently truncating. */
#define BR_PAGE_MAX_PAGES    4096u

/* Set up the page pool: base must be 4 KiB aligned, size non-zero and a multiple of 4 KiB.
 * Returns 0 / -EINVAL (granularity) / -ENOMEM (page count over BR_PAGE_MAX_PAGES). */
int br_page_pool_init(br_uintptr_t base, br_size_t size);

#endif /* BR_MEM_TLSF_INTERNAL_H */
