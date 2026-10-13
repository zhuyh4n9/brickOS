/*
 * Memory (mem) contract: TLSF heap, contig pool, page pool and DMA. Core does not own RAM, it
 * claims regions: br_mem_init() takes the region table from br_mm.h by kind
 * (BR_MM_KIND_HEAP/CONTIG/PAGE/DMA) and builds one pool each. Contents: the 11 design-mandated
 * functions, the observation contract (consumed by plugins), and the owner-label accounting.
 */
#ifndef BR_CORE_BR_MEM_H
#define BR_CORE_BR_MEM_H

#include <br/core/br_types.h>

/* ---- constants ---- */
/* Minimum alignment of heap blocks: 16 B so a malloc'd buffer can hold any AAPCS64-natural
 * object (TC-MEM-001 asserts >= BR_MALLOC_ALIGN). */
#define BR_MALLOC_ALIGN   16u

/* Page granularity, fixed to 4 KiB on this prototype (of CA-8's 4K/16K/64K it is the most common
 * on QEMU virt aarch64, and pool/page-table/L3 descriptor granularities agree). */
#define BR_PAGE_SIZE      4096u

/* br_page_alloc attrs (append-only) */
#define BR_PAGE_F_ZERO    0x0001u   /* allocated pages must be all zero */
#define BR_PAGE_F_CONTIG  0x0002u   /* must be physically contiguous — always true in this pool (v1 identity mapping), setting it is not an error */

/* br_dma_alloc attrs (append-only) */
#define BR_DMA_F_ZERO     0x0001u   /* buffer all zero */
#define BR_DMA_F_ALIGN4K  0x0002u   /* 4 KiB aligned */
#define BR_DMA_F_CACHED   0x0004u   /* cacheable (default non-cached = conservative choice for device-side coherency) */

/* ---- (1) native surface ---- */
/* ---- byte heap (TLSF) ---- */
void *br_malloc (br_size_t n);
void *br_calloc (br_size_t n, br_size_t size);
void *br_realloc(void *p, br_size_t n);     /* runtime/posix `realloc` forwards here directly (ADR-0014) */
void  br_free   (void *p);

/* ---- physically contiguous memory (CA-7): separate contig pool, not the TLSF heap ---- */
int  br_mem_alloc_contig(br_size_t size, br_size_t align, void **out);
void br_mem_free_contig (void *ptr, br_size_t size);
   /* For DMA engines without scatter-gather and large v2 relocation copies; large fixed
      blocks (framebuffers) are reserved in the region table, not runtime-allocated. */

/* ---- page allocator (CA-8): static bitmap + first fit of contiguous runs ---- */
void *br_page_alloc(br_size_t n_pages, br_u32 attrs);
void  br_page_free (void *vaddr, br_size_t n_pages);

/* ---- DMA (built on contig: physical contiguity guaranteed) ---- */
typedef struct {
    void         *vaddr;
    br_uintptr_t  dma_addr;     /* v1 identity: dma_addr == vaddr (CA-6/INV-6) */
    br_size_t     size;
} br_dma_buf_t;

int br_dma_alloc(br_size_t n, br_u32 attrs, br_dma_buf_t *out);
int br_dma_free (const br_dma_buf_t *b);

/* Arena accounting/bridge reporting (TC-MEM-007 enforces "alloc/free deltas match"). */
int br_heap_usage(br_size_t *used, br_size_t *total);

/* ---- initialization ---- */
/*
 * Scan the region table and claim the pools by kind, then build the TLSF control block and page
 * bitmap. Called by br_core_main() after platform plugin init and before br_mm_activate(); the
 * platform only declares the table. Errors: -EINVAL (bad granularity/alignment), -ENODEV (pool
 * missing), -ENOMEM (pool too small). Idempotent: a second call returns the first result.
 */
int  br_mem_init (void);
br_bool br_mem_ready(void);

/* Pool geometry (observation; used by dump/memleak reporting) */
typedef struct { br_uintptr_t base; br_size_t size; } br_pool_t;
typedef struct {
    br_pool_t heap;
    br_pool_t contig;
    br_pool_t page;
    br_pool_t dma;
    br_u32    page_size;      /* = BR_PAGE_SIZE */
    br_u32    page_total;     /* total pages in the page pool */
} br_mem_layout_t;

int br_mem_layout(br_mem_layout_t *out);          /* not ready ⇒ -ENODEV */

/* ---- (2) observation contract ---- */
/* Heap stats (observation; br_heap_usage is its projection onto the native surface) */
typedef struct {
    br_size_t total;            /* total bytes of the heap pool (usable area, excluding TLSF metadata) */
    br_size_t used;             /* currently allocated to users + per-block header/footer/redzone */
    br_size_t free_bytes;
    br_size_t max_free_block;   /* largest allocatable block (first-order fragmentation metric) */
    br_size_t high_water;       /* historical peak of used */
    br_u32    n_alloc;          /* successful allocations */
    br_u32    n_free;           /* successful frees */
    br_u32    n_live;           /* currently live blocks */
    br_u32    n_fail;           /* failed allocations */
    br_u32    redzone_hits;     /* tail redzone trampled count — detection means corruption */
    br_u32    canary_hits;      /* header magic corrupted count */
    br_u32    double_free;      /* double free rejections */
    br_u32    bad_free;         /* frees of pointers not from this heap, rejected */
    br_u32    owner_count;      /* number of registered owner labels */
} br_heap_stats_t;

int br_heap_stats_get(br_heap_stats_t *out);      /* not ready ⇒ -ENODEV */

/* Live blocks (per-block walk; data source for memleak's leak report) */
typedef struct {
    void        *addr;        /* user-visible address */
    br_size_t    size;        /* user-requested size */
    br_size_t    block;       /* actual footprint including header/footer */
    br_u16       owner;       /* owner label (BR_OWNER_NONE = unattributed) */
    br_uintptr_t caller;      /* allocation-site return address (decoded offline with addr2line/llvm-symbolizer) */
    br_u32       seq;         /* allocation sequence number (monotonic; for "earliest suspicious leak" sorting) */
} br_heap_block_t;

typedef int (*br_heap_walk_fn)(void *ctx, const br_heap_block_t *blk);
int   br_heap_walk(br_heap_walk_fn fn, void *ctx);   /* returns blocks visited; -ENODEV if not ready */

/* Whole-heap self-check: verify each block's header magic/redzone/free-list poisoning; returns
 * the violation count (0 = clean, TC-DBG-041) */
br_u32 br_heap_check(void);

/* Page pool stats (observation) */
typedef struct {
    br_u32 total;
    br_u32 used;
    br_u32 free;
    br_u32 fail;
    br_u32 max_run;      /* current largest contiguous free run (pages) */
} br_page_stats_t;

int br_page_stats(br_page_stats_t *out);

/* ---- (3) accounting contract ---- */
/* Owner label = the cheap v1.x form of "per-plugin arena accounting": the current owner is
 * recorded in the block header and the leak report accounts by owner. Default BR_OWNER_NONE (0). */
typedef br_u16 br_owner_t;
#define BR_OWNER_NONE   0u

br_owner_t  br_heap_owner_register(const char *name);   /* full/duplicate name ⇒ existing label or None */
const char *br_heap_owner_name(br_owner_t o);           /* unknown ⇒ "?"; None ⇒ "(none)" */
br_owner_t  br_heap_owner_set(br_owner_t o);            /* returns the old value (for paired restore) */
br_owner_t  br_heap_owner_get(void);

#endif /* BR_CORE_BR_MEM_H */
