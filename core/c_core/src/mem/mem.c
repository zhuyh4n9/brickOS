/*
 * Memory subsystem: br_mem_init() claims the heap/contig/page pools from the platform region
 * table (core does not own RAM, it only claims by BR_MM_KIND_*) and builds TLSF + page bitmap
 * + contig/DMA pools; the TLSF wrapper adds accounting, redzone, poison and owner tags.
 *
 * User block layout (frozen 32 B header; the br_heap_block_t fields live inside it):
 *   TLSF block: [ TLSF header 16B | ------------------- payload ------------------- ]
 *   payload:    [ mem header 32B | user area n | redzone 0xA5 | footer 8B magic2|size ]
 *   user pointer = mem header + 32
 * Redzone length = (8 - n % 8) % 8 + BR_MEM_REDZONE, so a one-byte overflow past the user area
 * always lands in the redzone. TLSF request = 32 + ALIGN8(n) + 8 + 8. The header does not store
 * the block footprint: it follows from n and is queryable via br_tlsf_block_size().
 * br_malloc/br_free do not log (hot path); only errors and the init summary do, prefix "mem: ".
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_types.h>

#include "tlsf_internal.h"

/* ---- constants and block layout ---- */

#define BR_MEM_HDR        32u    /* mem-layer block header */
#define BR_MEM_FOOTER     8u     /* trailing magic2 + size copy */
#define BR_MEM_REDZONE    8u     /* minimum redzone length (beyond the 8-byte padding) */

#define BR_MEM_MAGIC      0x4D454D42u   /* "BMEM" */
#define BR_MEM_FOOT_MAGIC 0x544F4F46u   /* "FOOT" */
#define BR_MEM_RZ_BYTE    0xA5u         /* redzone fill byte */
#define BR_MEM_POISON     0x5Au         /* user-area poison value after free */

#define BR_MEM_ST_LIVE    0x11A7u
#define BR_MEM_ST_DEAD    0xDEADu

#define BR_OWNER_MAX      16u

/* static node bound for the contig / DMA pools (small pools, no growth). */
#define BR_CONTIG_MAX_FREE   64u
#define BR_CONTIG_MAX_ALLOC  64u

typedef struct br_mem_hdr {
    br_u32              magic;   /* [0x00] header magic (corruption => canary) */
    br_u32              size;    /* [0x04] user-requested bytes (< 4 GiB; pools are far smaller) */
    br_u32              seq;     /* [0x08] allocation sequence (monotonic; walk sort key) */
    br_u16              owner;   /* [0x0C] ownership tag */
    br_u16              state;   /* [0x0E] LIVE / DEAD (double-free check) */
    struct br_mem_hdr  *next;    /* [0x10] live-block singly linked list */
    br_uintptr_t        caller;  /* [0x18] return address of the allocation site */
} br_mem_hdr_t;

_Static_assert(sizeof(br_mem_hdr_t) == BR_MEM_HDR, "mem block header must be 32 B (layout frozen)");

typedef struct br_mem_footer {
    br_u32 magic;   /* footer magic */
    br_u32 size;    /* size copy (mismatch with header => header/footer stomped) */
} br_mem_footer_t;

_Static_assert(sizeof(br_mem_footer_t) == BR_MEM_FOOTER, "footer sentinel must be 8 B");

/* ---- global state (single instance; no threads in v1, so no locking) ---- */

static struct {
    br_bool          attempted;   /* idempotent: first call's result is cached, never rebuilt */
    int              init_rc;
    br_bool          ready;
    br_bool          have_dma;
    br_mem_layout_t  layout;
    br_mem_hdr_t    *live;        /* live-block singly linked list (push-front => seq decreasing) */
    br_u32           seq;
    br_size_t        high_water;
    br_u32           n_alloc;
    br_u32           n_free;
    br_u32           n_live;
    br_u32           n_fail;
    br_u32           redzone_hits;
    br_u32           canary_hits;
    br_u32           double_free;
    br_u32           bad_free;
    br_u32           contig_bad_free;
    br_u32           contig_size_mismatch;
} s_mem;

/* Owner tag table (tags only, no budget/enforcement). */
static struct {
    const char *name;
} s_owners[BR_OWNER_MAX];
static br_u32     s_owner_n;
static br_owner_t s_owner_cur = BR_OWNER_NONE;

/* ---- small helpers (no libc) ---- */

static br_bool mem_str_eq(const char *a, const char *b)
{
    if ((a == BR_NULL) || (b == BR_NULL)) {
        return BR_FALSE;
    }
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

static void mem_fill(void *p, br_size_t n, br_u8 v)
{
    br_u8 *b = (br_u8 *)p;
    for (br_size_t i = 0u; i < n; i++) {
        b[i] = v;
    }
}

static br_size_t mem_align8(br_size_t v)
{
    return (v + 7u) & ~(br_size_t)7u;
}

static br_size_t mem_req(br_size_t n)
{
    return (br_size_t)BR_MEM_HDR + mem_align8(n) +
           (br_size_t)BR_MEM_REDZONE + (br_size_t)BR_MEM_FOOTER;
}

static br_u8 *mem_user(const br_mem_hdr_t *h)
{
    return (br_u8 *)(void *)h + BR_MEM_HDR;
}

static br_u8 *mem_rz_begin(const br_mem_hdr_t *h)
{
    return mem_user(h) + h->size;
}

static br_u8 *mem_foot_at(const br_mem_hdr_t *h)
{
    return mem_user(h) + mem_align8(h->size) + (br_size_t)BR_MEM_REDZONE;
}

/* user pointer -> header (inside heap pool, BR_MALLOC_ALIGN-aligned); invalid => NULL. */
static br_mem_hdr_t *mem_hdr_of_user(void *p)
{
    if ((p == BR_NULL) || (s_mem.ready == BR_FALSE)) {
        return BR_NULL;
    }
    const br_uintptr_t ua = (br_uintptr_t)p;
    const br_uintptr_t lo = s_mem.layout.heap.base + (br_uintptr_t)BR_MEM_HDR;
    const br_uintptr_t hi = s_mem.layout.heap.base + s_mem.layout.heap.size;
    if ((ua < lo) || (ua >= hi)) {
        return BR_NULL;
    }
    if ((ua & (br_uintptr_t)(BR_MALLOC_ALIGN - 1u)) != 0u) {
        return BR_NULL;
    }
    return (br_mem_hdr_t *)(void *)((br_u8 *)p - BR_MEM_HDR);
}

static br_mem_hdr_t *mem_find_live(const br_mem_hdr_t *h)
{
    for (br_mem_hdr_t *it = s_mem.live; it != BR_NULL; it = it->next) {
        if (it == h) {
            return it;
        }
    }
    return BR_NULL;
}

static void mem_unlink_live(br_mem_hdr_t *h)
{
    br_mem_hdr_t **pp = &s_mem.live;
    while (*pp != BR_NULL) {
        if (*pp == h) {
            *pp = h->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ---- contig pool: sorted free list + first fit + aligned hole split + merge on free ----
 * The allocation record is the only source of truth at free time: the size argument of
 * br_mem_free_contig(ptr,size) is the caller's memory, so the record's block size is always
 * used and a mismatch is only counted as an error.
 */

typedef struct {
    br_uintptr_t addr;
    br_size_t    size;
} br_contig_free_t;

typedef struct {
    br_uintptr_t addr;    /* user block start */
    br_size_t    block;   /* actual footprint (after 16-byte alignment) */
    br_size_t    size;    /* user request */
} br_contig_alloc_t;

typedef struct {
    br_uintptr_t      base;
    br_size_t         size;
    br_bool           ready;
    br_contig_free_t  free_list[BR_CONTIG_MAX_FREE];
    br_u32            n_free;
    br_contig_alloc_t allocs[BR_CONTIG_MAX_ALLOC];
    br_u32            n_alloc;
} br_contig_pool_t;

static br_contig_pool_t s_contig;
static br_contig_pool_t s_dma;

static br_uintptr_t mem_align_up_ptr(br_uintptr_t p, br_size_t align)
{
    return (p + ((br_uintptr_t)align - 1u)) & ~((br_uintptr_t)align - 1u);
}

static void contig_pool_init(br_contig_pool_t *cp, br_uintptr_t base, br_size_t size)
{
    cp->base    = base;
    cp->size    = size;
    cp->ready   = BR_TRUE;
    cp->n_free  = 1u;
    cp->n_alloc = 0u;
    cp->free_list[0].addr = base;
    cp->free_list[0].size = size;
    for (br_u32 i = 1u; i < BR_CONTIG_MAX_FREE; i++) {
        cp->free_list[i].addr = 0u;
        cp->free_list[i].size = 0u;
    }
    for (br_u32 i = 0u; i < BR_CONTIG_MAX_ALLOC; i++) {
        cp->allocs[i].addr  = 0u;
        cp->allocs[i].block = 0u;
        cp->allocs[i].size  = 0u;
    }
}

/* insert a free node in ascending address order (contig_merge merges in one pass). */
static br_bool contig_free_insert(br_contig_pool_t *cp, br_uintptr_t addr, br_size_t size)
{
    if (cp->n_free >= (br_u32)BR_CONTIG_MAX_FREE) {
        return BR_FALSE;
    }
    br_u32 pos = cp->n_free;
    while (pos > 0u) {
        if (cp->free_list[pos - 1u].addr <= addr) {
            break;
        }
        cp->free_list[pos] = cp->free_list[pos - 1u];
        pos--;
    }
    cp->free_list[pos].addr = addr;
    cp->free_list[pos].size = size;
    cp->n_free++;
    return BR_TRUE;
}

/* one linear pass merging adjacent free nodes (the list is address-sorted and non-overlapping). */
static void contig_merge(br_contig_pool_t *cp)
{
    br_u32 i = 0u;
    while ((i + 1u) < cp->n_free) {
        br_contig_free_t *a = &cp->free_list[i];
        const br_contig_free_t *b = &cp->free_list[i + 1u];
        if ((a->addr + a->size) == b->addr) {
            a->size += b->size;
            for (br_u32 k = i + 1u; (k + 1u) < cp->n_free; k++) {
                cp->free_list[k] = cp->free_list[k + 1u];
            }
            cp->n_free--;
        } else {
            i++;
        }
    }
}

static int contig_alloc(br_contig_pool_t *cp, br_size_t size, br_size_t align, void **out)
{
    if (cp->ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if (size == 0u) {
        return BR_ERR(EINVAL);
    }
    /* align must be a power of two (0 and values with several 1 bits are rejected). */
    if ((align == 0u) || ((align & (align - 1u)) != 0u)) {
        return BR_ERR(EINVAL);
    }
    if (align < (br_size_t)BR_TLSF_ALIGN) {
        align = (br_size_t)BR_TLSF_ALIGN;
    }

    const br_size_t need = mem_align8(size);

    for (br_u32 i = 0u; i < cp->n_free; i++) {
        const br_uintptr_t start  = cp->free_list[i].addr;
        const br_size_t    fsize  = cp->free_list[i].size;
        const br_uintptr_t astart = mem_align_up_ptr(start, align);
        const br_size_t    front  = (br_size_t)(astart - start);
        if (front > fsize) {
            continue;
        }
        if ((fsize - front) < need) {
            continue;
        }

        const br_size_t tail = fsize - front - need;

        if (cp->n_alloc >= (br_u32)BR_CONTIG_MAX_ALLOC) {
            return BR_ERR(ENOMEM);
        }
        const br_u32 extra = ((front > 0u) ? 1u : 0u) + ((tail > 0u) ? 1u : 0u);
        if ((cp->n_free + extra - 1u) > (br_u32)BR_CONTIG_MAX_FREE) {
            return BR_ERR(ENOMEM);
        }

        cp->allocs[cp->n_alloc].addr  = astart;
        cp->allocs[cp->n_alloc].block = need;
        cp->allocs[cp->n_alloc].size  = size;
        cp->n_alloc++;

        for (br_u32 k = i; (k + 1u) < cp->n_free; k++) {
            cp->free_list[k] = cp->free_list[k + 1u];
        }
        cp->n_free--;
        if (front > 0u) {
            (void)contig_free_insert(cp, start, front);
        }
        if (tail > 0u) {
            (void)contig_free_insert(cp, astart + need, tail);
        }

        *out = (void *)astart;
        return BR_OK;
    }

    return BR_ERR(ENOMEM);
}

static void contig_free(br_contig_pool_t *cp, void *ptr, br_size_t size)
{
    if ((cp->ready == BR_FALSE) || (ptr == BR_NULL)) {
        return;
    }

    const br_uintptr_t a = (br_uintptr_t)ptr;
    br_u32 idx = (br_u32)BR_CONTIG_MAX_ALLOC;
    for (br_u32 i = 0u; i < cp->n_alloc; i++) {
        if (cp->allocs[i].addr == a) {
            idx = i;
            break;
        }
    }
    if (idx >= (br_u32)BR_CONTIG_MAX_ALLOC) {
        s_mem.contig_bad_free++;   /* not in this pool / never allocated: ignore + count */
        br_log_warn("mem: contig free of non-pool pointer %p", ptr);
        return;
    }

    const br_contig_alloc_t rec = cp->allocs[idx];
    if (size != rec.size) {
        s_mem.contig_size_mismatch++;   /* free by record, do not trust the argument */
    }

    cp->n_alloc--;
    if (idx != cp->n_alloc) {
        cp->allocs[idx] = cp->allocs[cp->n_alloc];
    }
    cp->allocs[cp->n_alloc].addr  = 0u;
    cp->allocs[cp->n_alloc].block = 0u;
    cp->allocs[cp->n_alloc].size  = 0u;

    if (contig_free_insert(cp, rec.addr, rec.block) == BR_FALSE) {
        s_mem.contig_bad_free++;
        br_log_error("mem: contig free list full, %p not returned (leak)", ptr);
        return;
    }
    contig_merge(cp);
}

/* ---- owner tags ---- */

br_owner_t br_heap_owner_register(const char *name)
{
    if ((name == BR_NULL) || (s_owner_n >= (br_u32)BR_OWNER_MAX)) {
        return BR_OWNER_NONE;
    }
    for (br_u32 i = 0u; i < s_owner_n; i++) {
        if (mem_str_eq(s_owners[i].name, name) == BR_TRUE) {
            return BR_OWNER_NONE;   /* duplicate name: do not create a new one */
        }
    }
    s_owners[s_owner_n].name = name;
    s_owner_n++;
    return (br_owner_t)s_owner_n;
}

const char *br_heap_owner_name(br_owner_t o)
{
    if (o == BR_OWNER_NONE) {
        return "(none)";
    }
    if (((br_u32)o >= 1u) && ((br_u32)o <= s_owner_n)) {
        return s_owners[(br_u32)o - 1u].name;
    }
    return "?";
}

br_owner_t br_heap_owner_set(br_owner_t o)
{
    const br_owner_t old = s_owner_cur;
    s_owner_cur = o;
    return old;
}

br_owner_t br_heap_owner_get(void)
{
    return s_owner_cur;
}

/* ---- byte heap (TLSF wrapper) ---- */

void *br_malloc(br_size_t n)
{
    if (s_mem.ready == BR_FALSE) {
        s_mem.n_fail++;
        return BR_NULL;
    }
    if (n > (br_size_t)0xFFFFFFF0u) {   /* the size field is u32 (frozen 32 B block header layout) */
        s_mem.n_fail++;
        return BR_NULL;
    }

    br_u8 *h = (br_u8 *)br_tlsf_alloc(mem_req(n));
    if (h == BR_NULL) {
        s_mem.n_fail++;
        return BR_NULL;   /* hot path: no logging; counters suffice to locate it */
    }

    br_mem_hdr_t *hdr = (br_mem_hdr_t *)(void *)h;
    hdr->magic  = BR_MEM_MAGIC;
    hdr->size   = (br_u32)n;
    hdr->seq    = ++s_mem.seq;
    hdr->owner  = br_heap_owner_get();
    hdr->state  = BR_MEM_ST_LIVE;
    hdr->caller = (br_uintptr_t)__builtin_return_address(0);

    br_u8 *up  = h + BR_MEM_HDR;
    br_u8 *rzb = up + n;
    br_u8 *fo  = up + mem_align8(n) + (br_size_t)BR_MEM_REDZONE;

    mem_fill(rzb, (br_size_t)(fo - rzb), (br_u8)BR_MEM_RZ_BYTE);

    br_mem_footer_t *f = (br_mem_footer_t *)(void *)fo;
    f->magic = BR_MEM_FOOT_MAGIC;
    f->size  = (br_u32)n;

    hdr->next  = s_mem.live;
    s_mem.live = hdr;
    s_mem.n_alloc++;
    s_mem.n_live++;

    const br_size_t u = br_tlsf_used();
    if (u > s_mem.high_water) {
        s_mem.high_water = u;
    }
    return (void *)up;
}

void br_free(void *p)
{
    if (p == BR_NULL) {
        return;   /* free(NULL) is a no-op */
    }
    if (s_mem.ready == BR_FALSE) {
        s_mem.bad_free++;
        return;
    }

    br_mem_hdr_t *h = mem_hdr_of_user(p);
    if (h == BR_NULL) {
        s_mem.bad_free++;   /* not this heap / misaligned */
        br_log_error("mem: free of non-heap pointer %p", p);
        return;
    }

    /* The free list stores the block tail (tlsf.c), so a freed header stays readable: this
     * tells "already freed" (state=DEAD) from a wild pointer. */
    if ((h->magic == BR_MEM_MAGIC) && (h->state == BR_MEM_ST_DEAD)) {
        s_mem.double_free++;
        return;
    }
    if (mem_find_live(h) == BR_NULL) {
        s_mem.double_free++;   /* heap address but not on the live list: double free or forged pointer */
        return;
    }
    if (h->magic != BR_MEM_MAGIC) {
        s_mem.canary_hits++;   /* header magic stomped, keep freeing anyway to avoid a leak */
    }

    const br_size_t n   = h->size;
    br_u8          *rzb = mem_rz_begin(h);
    br_u8          *fo  = mem_foot_at(h);
    br_u32          bad = 0u;
    for (br_u8 *q = rzb; q < fo; q++) {
        if (*q != (br_u8)BR_MEM_RZ_BYTE) {
            bad++;
        }
    }
    if (bad > 0u) {
        s_mem.redzone_hits += bad;
        br_log_error("mem: redzone stomped, %u bytes (blk=%p size=%lu)", bad, p, (unsigned long)n);
    }

    const br_mem_footer_t *f = (const br_mem_footer_t *)(const void *)fo;
    if ((f->magic != BR_MEM_FOOT_MAGIC) || (f->size != (br_u32)n)) {
        s_mem.canary_hits++;
    }

    mem_fill((void *)p, n, (br_u8)BR_MEM_POISON);   /* poison the user area (makes dangling reads visible) */

    mem_unlink_live(h);
    h->state = BR_MEM_ST_DEAD;
    h->next  = BR_NULL;
    br_tlsf_free(h);

    s_mem.n_free++;
    if (s_mem.n_live > 0u) {
        s_mem.n_live--;
    }
}

void *br_calloc(br_size_t n, br_size_t size)
{
    if ((n != 0u) && (size > (~(br_size_t)0u) / n)) {
        s_mem.n_fail++;   /* multiplication overflow: avoid "computed small, then wrote out of bounds" */
        return BR_NULL;
    }
    const br_size_t total = n * size;
    void *p = br_malloc(total);
    if (p == BR_NULL) {
        return BR_NULL;
    }
    mem_fill(p, total, 0u);
    return p;
}

void *br_realloc(void *p, br_size_t n)
{
    if (p == BR_NULL) {
        return br_malloc(n);
    }
    if (n == 0u) {
        br_free(p);
        return BR_NULL;
    }

    br_mem_hdr_t *h = mem_hdr_of_user(p);
    if ((h == BR_NULL) || (mem_find_live(h) == BR_NULL)) {
        s_mem.bad_free++;
        return BR_NULL;
    }

    const br_size_t old_n = h->size;
    void *q = br_malloc(n);
    if (q == BR_NULL) {
        return BR_NULL;   /* the original block stays valid (POSIX semantics; n_fail already counted by br_malloc) */
    }

    const br_size_t keep = (old_n < n) ? old_n : n;
    const br_u8 *src = (const br_u8 *)p;
    br_u8 *dst = (br_u8 *)q;
    for (br_size_t i = 0u; i < keep; i++) {
        dst[i] = src[i];
    }
    br_free(p);
    return q;
}

int br_heap_usage(br_size_t *used, br_size_t *total)
{
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if ((used == BR_NULL) || (total == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* Ask TLSF directly; no second cache, so accounting cannot drift from reality. */
    *used  = br_tlsf_used();
    *total = s_mem.layout.heap.size;
    return BR_OK;
}

int br_heap_stats_get(br_heap_stats_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }

    out->total          = s_mem.layout.heap.size;
    out->used           = br_tlsf_used();
    out->free_bytes     = br_tlsf_free_bytes();
    out->max_free_block = br_tlsf_max_free();
    out->high_water     = s_mem.high_water;
    out->n_alloc        = s_mem.n_alloc;
    out->n_free         = s_mem.n_free;
    out->n_live         = s_mem.n_live;
    out->n_fail         = s_mem.n_fail;
    out->redzone_hits   = s_mem.redzone_hits;
    out->canary_hits    = s_mem.canary_hits;
    out->double_free    = s_mem.double_free;
    out->bad_free       = s_mem.bad_free;
    out->owner_count    = s_owner_n;
    return BR_OK;
}

int br_heap_walk(br_heap_walk_fn fn, void *ctx)
{
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if (fn == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /* The live list is push-front, so seq decreases head to tail; reverse in place for ascending
     * order, flip back after. The callback must not allocate/free (it would mutate the list). */
    br_mem_hdr_t *prev = BR_NULL;
    br_mem_hdr_t *cur  = s_mem.live;
    while (cur != BR_NULL) {
        br_mem_hdr_t *nx = cur->next;
        cur->next = prev;
        prev = cur;
        cur = nx;
    }
    s_mem.live = prev;

    int count = 0;
    for (cur = s_mem.live; cur != BR_NULL; cur = cur->next) {
        br_heap_block_t blk;
        blk.addr   = (void *)mem_user(cur);
        blk.size   = cur->size;
        blk.block  = br_tlsf_block_size(blk.addr);
        blk.owner  = cur->owner;
        blk.caller = cur->caller;
        blk.seq    = cur->seq;
        count++;
        if (fn(ctx, &blk) != 0) {
            break;
        }
    }

    prev = BR_NULL;
    cur  = s_mem.live;
    while (cur != BR_NULL) {
        br_mem_hdr_t *nx = cur->next;
        cur->next = prev;
        prev = cur;
        cur = nx;
    }
    s_mem.live = prev;
    return count;
}

br_u32 br_heap_check(void)
{
    if (s_mem.ready == BR_FALSE) {
        return 0u;
    }

    br_u32 v = 0u;
    for (br_mem_hdr_t *h = s_mem.live; h != BR_NULL; h = h->next) {
        if (h->magic != BR_MEM_MAGIC) {
            s_mem.canary_hits++;
            v++;
        }
        if (h->state != BR_MEM_ST_LIVE) {
            s_mem.canary_hits++;
            v++;
        }

        const br_size_t n   = h->size;
        const br_u8    *rzb = mem_rz_begin(h);
        const br_u8    *fo  = mem_foot_at(h);
        for (const br_u8 *q = rzb; q < fo; q++) {
            if (*q != (br_u8)BR_MEM_RZ_BYTE) {
                s_mem.redzone_hits++;
                v++;
            }
        }
        const br_mem_footer_t *f = (const br_mem_footer_t *)(const void *)fo;
        if ((f->magic != BR_MEM_FOOT_MAGIC) || (f->size != (br_u32)n)) {
            s_mem.canary_hits++;
            v++;
        }
    }

    v += br_tlsf_check();
    return v;
}

/* ---- contig pool / page pool / DMA ---- */

int br_mem_alloc_contig(br_size_t size, br_size_t align, void **out)
{
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *out = BR_NULL;
    return contig_alloc(&s_contig, size, align, out);
}

void br_mem_free_contig(void *ptr, br_size_t size)
{
    if (s_mem.ready == BR_FALSE) {
        return;
    }
    contig_free(&s_contig, ptr, size);
}

int br_dma_alloc(br_size_t n, br_u32 attrs, br_dma_buf_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    out->vaddr    = BR_NULL;
    out->dma_addr = 0u;
    out->size     = 0u;

    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if (s_mem.have_dma == BR_FALSE) {
        return BR_ERR(ENODEV);   /* the region table declares no DMA pool */
    }
    if (n == 0u) {
        return BR_ERR(EINVAL);
    }

    /* 4 KiB aligned by default (identity mapping + device consistency); BR_DMA_F_ALIGN4K does
     * not change the path, BR_DMA_F_CACHED is ignored (uncached is the conservative default). */
    void *v = BR_NULL;
    const int rc = contig_alloc(&s_dma, n, (br_size_t)BR_PAGE_SIZE, &v);
    if (rc != BR_OK) {
        return rc;
    }

    if ((attrs & BR_DMA_F_ZERO) != 0u) {
        mem_fill(v, n, 0u);
    }
    out->vaddr    = v;
    out->dma_addr = (br_uintptr_t)v;   /* CA-6 / INV-6: identity in v1 */
    out->size     = n;
    return BR_OK;
}

int br_dma_free(const br_dma_buf_t *b)
{
    if (b == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if (s_mem.have_dma == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if (b->vaddr == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (b->dma_addr != (br_uintptr_t)b->vaddr) {
        /* The two addresses are two views of one source of truth: on mismatch do not return
         * the block (the wrong address would damage the pool). */
        br_log_error("mem: dma_free address mismatch vaddr=%p dma=0x%lx",
                     b->vaddr, (unsigned long)b->dma_addr);
        return BR_ERR(EINVAL);
    }
    contig_free(&s_dma, b->vaddr, b->size);
    return BR_OK;
}

/* ---- init: claim regions, build TLSF + page bitmap + contig/DMA pools ---- */

static int mem_claim_pools(void)
{
    br_pool_t heap   = { 0u, 0u };
    br_pool_t contig = { 0u, 0u };
    br_pool_t page   = { 0u, 0u };
    br_pool_t dma    = { 0u, 0u };
    br_bool   have_dma = BR_FALSE;

    const br_u32 n = br_mm_region_count();
    for (br_u32 i = 0u; i < n; i++) {
        const br_mm_region_t *r = br_mm_region_get(i);
        if (r == BR_NULL) {
            continue;
        }
        /* Pool must be 4 KiB aligned and a 4 KiB multiple (br_mm_region_add blocks once). */
        if (((r->base & (br_uintptr_t)(BR_PAGE_SIZE - 1u)) != 0u) ||
            (r->size == 0u) ||
            ((r->size & (br_size_t)(BR_PAGE_SIZE - 1u)) != 0u)) {
            br_log_error("mem: region[%u] illegal granularity base=%p size=%lu",
                         (unsigned)i, (void *)r->base, (unsigned long)r->size);
            return BR_ERR(EINVAL);
        }

        switch (BR_MM_KIND(r->attrs)) {
        case BR_MM_KIND_HEAP:
            if (heap.size == 0u) {
                heap.base = r->base;
                heap.size = r->size;
            }
            break;
        case BR_MM_KIND_CONTIG:
            if (contig.size == 0u) {
                contig.base = r->base;
                contig.size = r->size;
            }
            break;
        case BR_MM_KIND_PAGE:
            if (page.size == 0u) {
                page.base = r->base;
                page.size = r->size;
            }
            break;
        case BR_MM_KIND_DMA:
            if (have_dma == BR_FALSE) {
                dma.base  = r->base;
                dma.size  = r->size;
                have_dma  = BR_TRUE;
            }
            break;
        default:
            break;   /* IMAGE/STACK/MMIO/RESERVED: not pools, do not claim */
        }
    }

    if ((heap.size == 0u) || (contig.size == 0u) || (page.size == 0u)) {
        br_log_error("mem: missing required pool (heap=%lu contig=%lu page=%lu)",
                     (unsigned long)heap.size, (unsigned long)contig.size,
                     (unsigned long)page.size);
        return BR_ERR(ENODEV);
    }

    /* pools too small: each gets its own lower bound, so no unusable pool is built. */
    if (heap.size < ((br_size_t)BR_TLSF_OVERHEAD * 2u)) {
        return BR_ERR(ENOMEM);
    }
    if (contig.size < ((br_size_t)BR_TLSF_ALIGN * 2u)) {
        return BR_ERR(ENOMEM);
    }
    if (page.size < (br_size_t)BR_PAGE_SIZE) {
        return BR_ERR(ENOMEM);
    }
    if ((have_dma == BR_TRUE) && (dma.size < (br_size_t)BR_PAGE_SIZE)) {
        return BR_ERR(ENOMEM);
    }

    int rc = br_tlsf_init((void *)heap.base, heap.size);
    if (rc != BR_OK) {
        br_log_error("mem: TLSF init failed rc=%d", rc);
        return rc;
    }
    rc = br_page_pool_init(page.base, page.size);
    if (rc != BR_OK) {
        br_log_error("mem: page pool init failed rc=%d", rc);
        return rc;
    }

    contig_pool_init(&s_contig, contig.base, contig.size);
    if (have_dma == BR_TRUE) {
        contig_pool_init(&s_dma, dma.base, dma.size);
    }

    s_mem.layout.heap.base  = heap.base;
    s_mem.layout.heap.size  = heap.size;
    s_mem.layout.contig     = contig;
    s_mem.layout.page       = page;
    s_mem.layout.dma        = dma;
    s_mem.layout.page_size  = (br_u32)BR_PAGE_SIZE;
    s_mem.layout.page_total = (br_u32)(page.size / (br_size_t)BR_PAGE_SIZE);
    s_mem.have_dma          = have_dma;
    s_mem.ready             = BR_TRUE;

    br_log_info("mem: heap=%lu contig=%lu page=%lu(%lu pages) dma=%lu",
                (unsigned long)heap.size, (unsigned long)contig.size,
                (unsigned long)page.size, (unsigned long)s_mem.layout.page_total,
                have_dma == BR_TRUE ? (unsigned long)dma.size : 0ul);
    return BR_OK;
}

int br_mem_init(void)
{
    if (s_mem.attempted == BR_TRUE) {
        return s_mem.init_rc;   /* idempotent: a second call returns the first result, never rebuilds */
    }
    s_mem.attempted = BR_TRUE;
    s_mem.init_rc   = mem_claim_pools();
    return s_mem.init_rc;
}

br_bool br_mem_ready(void)
{
    return s_mem.ready;
}

int br_mem_layout(br_mem_layout_t *out)
{
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *out = s_mem.layout;
    return BR_OK;
}
