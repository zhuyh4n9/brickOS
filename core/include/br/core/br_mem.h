/*
 * brickOS prototype v0.1.0 — memory (mem) contract: TLSF heap / contig pool / page pool / DMA
 *
 * Authoritative design: `docs/3-os-core/3-01-core-api-list.md` §6 (br-mem group,
 * **11 functions**)
 *   + CA-7 (contiguous memory = separate contig pool, not through TLSF) / CA-8
 *     (page allocator = static bitmap + first fit)
 *   + `docs/3-os-core/3-04-memory.md` (three pools carved from the platform region
 *     table; "heap boot timing: init point = core.init" [3-04 §3])
 *   + `docs/5-debug/5-01-debug.md` §4 (redzone/poison/canary = cheap v1.x substitute;
 *     per-plugin arena accounting = v2, this prototype approximates it with an
 *     "owner label").
 *
 * Where the three pools come from: **core does not own RAM, it only claims
 * regions** (3-01 §6, verbatim: "the platform region table carves RAM into heap /
 * contig pool / page pool, ratio = manifest budget"). So the input to
 * `br_mem_init()` is the region table in `br_mm.h`: claim by
 * `BR_MM_KIND_HEAP/CONTIG/PAGE/DMA`, build one pool each.
 *
 * The three kinds of content in this header (CA-5 accounting must be explicit, see
 * ADR-0003):
 *   (1) **the 11 design-mandated functions** (native surface, names/signatures
 *       verbatim with 3-01 §6): malloc family 4 + contig 2 + page 2 + dma 2 +
 *       heap_usage 1;
 *   (2) **observation contract** (consumed by the memleak plugin; the consumer is a
 *       plugin ⇒ it cannot hide inside core — same shape as 3-02 §17.4's treatment
 *       of `br_irq_stats_get`): stats / walk / check / pool layout;
 *   (3) **accounting contract** (the v1.x deliverable form of 5-01 §4 "per-plugin
 *       arena accounting"): owner label registration + current owner switching.
 */
#ifndef BR_CORE_BR_MEM_H
#define BR_CORE_BR_MEM_H

#include <br/core/br_types.h>

/* ---------------------------------------------------------------- constants */
/* **Minimum alignment** of blocks returned by the heap. Design 3-01 §6 gives no
 * value; rationale for choosing 16 B (ADR-0003): AAPCS64 requires SP 16-aligned ⇒
 * the natural alignment upper bound for structs/stack objects is 16; TLSF natively
 * aligning to 4/8 would create the trap that "a malloc'd buffer cannot hold an
 * __attribute__((aligned(16))) object". Case TC-MEM-001 asserts `>= BR_MALLOC_ALIGN`. */
#define BR_MALLOC_ALIGN   16u

/* Page granularity (design 3-01 §6: **platform compile-time constant**). This
 * prototype fixes 4 KiB (of CA-8's 4K/16K/64K, 4K is the most common on QEMU virt +
 * aarch64, and page pool/page table/L3 descriptor granularity agree). The platform
 * must carve pools at this granularity in the br_mm.h region table. */
#define BR_PAGE_SIZE      4096u

/* br_page_alloc attrs (3-01 §6 `BR_PAGE_F_*`; append-only) */
#define BR_PAGE_F_ZERO    0x0001u   /* allocated pages must be all zero */
#define BR_PAGE_F_CONTIG  0x0002u   /* must be physically contiguous — always true in this pool (v1 identity mapping), setting it is not an error */

/* br_dma_alloc attrs (3-01 §6 `BR_DMA_F_*`; append-only) */
#define BR_DMA_F_ZERO     0x0001u   /* buffer all zero */
#define BR_DMA_F_ALIGN4K  0x0002u   /* 4 KiB aligned */
#define BR_DMA_F_CACHED   0x0004u   /* cacheable (default non-cached = conservative choice for device-side coherency) */

/* ---------------------------------------------------------------- (1) native surface */
/* ---- byte heap (TLSF) ---- */
void *br_malloc (br_size_t n);
void *br_calloc (br_size_t n, br_size_t size);
void *br_realloc(void *p, br_size_t n);     /* runtime/posix `realloc` forwards here directly (ADR-0014) */
void  br_free   (void *p);

/* ---- physically contiguous memory (CA-7): separate contig pool, not through the TLSF heap ---- */
int  br_mem_alloc_contig(br_size_t size, br_size_t align, void **out);
void br_mem_free_contig (void *ptr, br_size_t size);
   /* for DMA engines without scatter-gather, large v2 relocation copies;
      large fixed blocks like framebuffers are statically reserved in the region
      table, not allocated at runtime */

/* ---- page allocator (CA-8): page granularity, static bitmap + first fit over contiguous runs ---- */
void *br_page_alloc(br_size_t n_pages, br_u32 attrs);
void  br_page_free (void *vaddr, br_size_t n_pages);

/* ---- DMA (built on contig: guarantees physical contiguity) ---- */
typedef struct {
    void         *vaddr;
    br_uintptr_t  dma_addr;     /* v1 identity: dma_addr == vaddr (CA-6/INV-6) */
    br_size_t     size;
} br_dma_buf_t;

int br_dma_alloc(br_size_t n, br_u32 attrs, br_dma_buf_t *out);
int br_dma_free (const br_dma_buf_t *b);

/* arena accounting/bridge reporting (design 3-01 §6; TC-MEM-007 enforces "alloc/free deltas match") */
int br_heap_usage(br_size_t *used, br_size_t *total);

/* ---------------------------------------------------------------- initialization */
/*
 * Scan the region table, claim the three pools + DMA pool by kind, build the TLSF
 * control block and page bitmap.
 * Ownership (3-04 §3): **init point = core.init**, called by core's entry
 * `br_core_main()` "after platform plugin init, before br_mm_activate()" (boot chain
 * stage (3), ADR-0008): platform only declares the region table (which memory is
 * what), claiming and allocating belong to core.
 * Errors: `-EINVAL` (bad region granularity/alignment) / `-ENODEV` (a required pool
 * is missing) / `-ENOMEM` (pool too small).
 * Idempotent: a second call returns the first result, pools are not rebuilt.
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

/* ---------------------------------------------------------------- (2) observation contract */
/* Heap stats (observation surface; `br_heap_usage` is its projection onto the native surface) */
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

/* Whole-heap self-check: verify each block's header magic/redzone/free-list poisoning; returns **violation count** (0 = clean, TC-DBG-041) */
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

/* ---------------------------------------------------------------- (3) accounting contract */
/*
 * Owner label = the **cheap v1.x form** of 5-01 §4 "per-plugin arena accounting":
 * on allocation the "current owner" is recorded in the block header, and the leak
 * report accounts by owner. Real arenas (budget caps + mandatory ownership + OOM
 * policy) belong to v2.
 * Default owner = BR_OWNER_NONE (0); while unregistered, current is always 0.
 */
typedef br_u16 br_owner_t;
#define BR_OWNER_NONE   0u

br_owner_t  br_heap_owner_register(const char *name);   /* full/duplicate name ⇒ existing label or None */
const char *br_heap_owner_name(br_owner_t o);           /* unknown ⇒ "?"; None ⇒ "(none)" */
br_owner_t  br_heap_owner_set(br_owner_t o);            /* returns the old value (for paired restore) */
br_owner_t  br_heap_owner_get(void);

#endif /* BR_CORE_BR_MEM_H */
