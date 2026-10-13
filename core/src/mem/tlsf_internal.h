/*
 * Memory-subsystem internal truth (core-private, not exported): the TLSF parameter
 * dictionary + single-instance interface (tlsf.c) and the page-pool setup entry (page.c,
 * called by br_mem_init() with the BR_MM_KIND_PAGE region). One heap and one static
 * control block, so no handle is passed.
 * Host cases include this file to stress the algorithm; br_mem.h/br_mm.h is the contract.
 */
#ifndef BR_MEM_TLSF_INTERNAL_H
#define BR_MEM_TLSF_INTERNAL_H

#include <br/core/br_types.h>

/* ---- TLSF parameter dictionary ---- */

/* Block alignment 16 B; the TLSF block stride must divide BR_MALLOC_ALIGN(16),
 * else mem.c's "user ptr = header + 32" is not 16-aligned. */
#define BR_TLSF_ALIGN_LOG2   4u
#define BR_TLSF_ALIGN        (1u << BR_TLSF_ALIGN_LOG2)

/* Two-level segregation: 32 first-level classes x 32 subdivisions each (2^5 x 2^5). */
#define BR_TLSF_FL_COUNT     32u
#define BR_TLSF_SL_LOG2      5u
#define BR_TLSF_SL_COUNT     (1u << BR_TLSF_SL_LOG2)

/* Block header = {size|free bit, prev_phys} (8 B each). */
#define BR_TLSF_HDR          16u
/* Min block = header 16 + free list 16; free links live at the block tail (see tlsf.c). */
#define BR_TLSF_MIN_BLOCK    32u

/* Per-block metadata overhead (excl. alignment slack); used for the min pool size. */
#define BR_TLSF_OVERHEAD     (BR_TLSF_HDR + BR_TLSF_MIN_BLOCK)

/* TLSF single-instance interface (one heap, so no handle). */
br_size_t br_tlsf_overhead(void);
int       br_tlsf_init(void *arena, br_size_t bytes);
void     *br_tlsf_alloc(br_size_t n);
void      br_tlsf_free(void *p);
br_size_t br_tlsf_block_size(void *p);   /* actual bytes (incl. TLSF header); 0 if not this heap */
br_size_t br_tlsf_used(void);
br_size_t br_tlsf_free_bytes(void);
br_size_t br_tlsf_max_free(void);        /* largest free block payload (allocatable bytes) */
br_u32    br_tlsf_check(void);           /* violation count (0 = clean); host case TC-MEM-002 */

/* ---- page pool internal setup (page.c) ---- */

/* Static bitmap bound: 4096 pages x 4 KiB = 16 MiB (bitmap 512 B). Compile-time constant;
 * past the bound br_page_pool_init() returns -ENOMEM instead of silently truncating. */
#define BR_PAGE_MAX_PAGES    4096u

/* Set up the page pool: base 4 KiB aligned, size non-zero and a multiple of 4 KiB.
 * Returns 0 / -EINVAL (granularity) / -ENOMEM (page count over BR_PAGE_MAX_PAGES). */
int br_page_pool_init(br_uintptr_t base, br_size_t size);

#endif /* BR_MEM_TLSF_INTERNAL_H */
