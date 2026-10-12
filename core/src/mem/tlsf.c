/*
 * brickOS prototype v0.1.0 — TLSF heap (pure algorithm; caller provides the arena, this file **logs nothing**)
 *
 * Authoritative design: `docs/3-os-core/3-01-core-api-list.md` §6 (byte heap of the br-mem group)
 * + §14 CA-7 (contiguous memory is a separate pool, not on this heap) + `docs/3-os-core/3-04-memory.md`
 * §3 (heap boot timing: init point = core.init). This file implements only the "two-level segregated
 * fit" algorithm itself, with no region table and no red zones/accounting — those live in mem.c
 * (§6 implementation design); this file must be stress-testable standalone on the host.
 *
 * 1. Two-level segregation (TLSF)
 *   Level 1 has 32 classes (2^5) + level 2 subdivides each class by 32 (2^5); `fl_bitmap`/`sl_bitmap`
 *   use bit scans to locate the smallest usable class ⇒ alloc/free are O(1). Mapping rules (parameter
 *   dictionary in tlsf_internal.h):
 *     - block size ∈ [32, 63] : fl = 0, sl = size - 32 (1 B granularity)
 *     - block size ∈ [2^k, 2^(k+1)) : fl = k - 5, sl = (size >> (k-5)) - 32
 *   `mapping_search` rounds the request **up to the upper bound of its sl slot** before searching,
 *   otherwise the same slot could yield a block smaller than the request (the most common TLSF
 *   mapping off-by-one; see br_tlsf_check's "size consistency" validation).
 *
 * 2. Block layout
 *   Block header is 16 B: {size | FREE bit, prev_phys} (prev_phys = total bytes of the previous
 *   physical block, 0 for the first block). Split/immediate coalesce; free blocks are doubly linked.
 *
 * ★ Free links are stored at the **block tail** (link_of), not the block head — the only shape
 *   difference from textbook TLSF: the 16 B payload at the block head is used by upper-layer mem.c
 *   as the first half of a **frozen 32 B block header**, and `docs/5-debug/5-01-debug.md` §4's
 *   "double-free interception" requires the header to stay readable after free (magic/state check).
 *   If links lived at the block head, TLSF would write next/prev into the header on free and a
 *   second free could only see garbage, unable to distinguish "double free" from "wild pointer".
 *   Cost: min block = 16 (header) + 16 (links), same as the textbook, no extra cost.
 *
 * 3. Single instance
 *   The prototype has one heap only (§6: "v1 has only the global heap"); the control block is
 *   file-static and the interface takes no handle. This file takes no lock: v0.1 is single-core
 *   and the caller guarantees serialization (thread-only; §11's ISR whitelist excludes memory).
 */
#include <br/core/br_error.h>
#include <br/core/br_types.h>

#include "tlsf_internal.h"

/* ---------------------------------------------------------------- block layout */

typedef struct tlsf_hdr {
    br_size_t size;       /* total bytes (including this header) | FREE bit */
    br_size_t prev_phys;  /* total bytes of previous physical block; first = 0 */
} tlsf_hdr_t;

#define BR_TLSF_F_FREE   ((br_size_t)1u)

/* Free-list node: valid only for **free blocks**, stored in the 16 B block tail. */
typedef struct tlsf_link {
    tlsf_hdr_t *prev;
    tlsf_hdr_t *next;
} tlsf_link_t;

/* ---------------------------------------------------------------- control block */

static struct {
    br_u8       *arena;
    br_size_t    bytes;
    br_u32       fl_bitmap;
    br_u32       sl_bitmap[BR_TLSF_FL_COUNT];
    tlsf_hdr_t  *blocks[BR_TLSF_FL_COUNT][BR_TLSF_SL_COUNT];
    br_size_t    used;        /* total footprint of allocated blocks (incl. headers) */
    br_size_t    free_bytes;  /* total footprint of free blocks (incl. headers); used + free == bytes */
    br_bool      init;
    br_u32       bad_free;    /* wild pointers/double frees caught at TLSF level (for self-test reference only) */
} s_tlsf;

/* ---------------------------------------------------------------- helpers */

static br_size_t blk_size(const tlsf_hdr_t *b)
{
    return b->size & ~BR_TLSF_F_FREE;
}

static br_bool blk_is_free(const tlsf_hdr_t *b)
{
    return ((b->size & BR_TLSF_F_FREE) != 0u) ? BR_TRUE : BR_FALSE;
}

static br_u8 *blk_user(const tlsf_hdr_t *b)
{
    return (br_u8 *)b + BR_TLSF_HDR;
}

static tlsf_hdr_t *blk_from_user(void *p)
{
    return (tlsf_hdr_t *)(void *)((br_u8 *)p - BR_TLSF_HDR);
}

static tlsf_link_t *link_of(tlsf_hdr_t *b)
{
    return (tlsf_link_t *)(void *)((br_u8 *)b + blk_size(b) - (br_size_t)sizeof(tlsf_link_t));
}

static br_size_t align_up16(br_size_t v)
{
    return (v + (br_size_t)(BR_TLSF_ALIGN - 1u)) & ~(br_size_t)(BR_TLSF_ALIGN - 1u);
}

static br_bool blk_in_arena(const tlsf_hdr_t *b)
{
    const br_u8 *p = (const br_u8 *)b;
    return ((p >= s_tlsf.arena) && (p < (s_tlsf.arena + s_tlsf.bytes))) ? BR_TRUE : BR_FALSE;
}

static br_bool pos_in_arena(const br_u8 *p)
{
    return ((p >= s_tlsf.arena) && (p < (s_tlsf.arena + s_tlsf.bytes))) ? BR_TRUE : BR_FALSE;
}

/* No libc: own bit scan (caller guarantees v != 0). */
static br_u32 ctz32(br_u32 v)
{
    br_u32 n = 0u;
    while ((v & 1u) == 0u) {
        v >>= 1;
        n++;
    }
    return n;
}

static br_u32 log2_floor(br_size_t v)
{
    br_u32 k = 0u;
    while (v > 1u) {
        v >>= 1;
        k++;
    }
    return k;
}

/* ---------------------------------------------------------------- mapping */

static void mapping_insert(br_size_t size, br_u32 *fl, br_u32 *sl)
{
    if (size < (br_size_t)(2u * BR_TLSF_MIN_BLOCK)) {
        /* Small class: [32, 63] at 1-byte granularity. Sizes below the min block map to 0 (should not occur). */
        *fl = 0u;
        *sl = (size >= (br_size_t)BR_TLSF_MIN_BLOCK)
                  ? (br_u32)(size - (br_size_t)BR_TLSF_MIN_BLOCK) : 0u;
    } else {
        const br_u32 k = log2_floor(size);
        *fl = k - BR_TLSF_SL_LOG2;
        *sl = (br_u32)((size >> (k - BR_TLSF_SL_LOG2)) - (br_size_t)BR_TLSF_SL_COUNT);
    }
}

/* Mapping used for search: round the request up to the upper bound of its sl slot, ensuring the selected block >= request. */
static void mapping_search(br_size_t size, br_u32 *fl, br_u32 *sl)
{
    if (size >= (br_size_t)(2u * BR_TLSF_MIN_BLOCK)) {
        const br_u32    k     = log2_floor(size);
        const br_u32    shift = k - BR_TLSF_SL_LOG2;
        const br_size_t mask  = ((br_size_t)1u << shift) - (br_size_t)1u;

        if ((size & mask) != 0u) {
            size = ((size >> shift) + (br_size_t)1u) << shift;
        }
    }
    mapping_insert(size, fl, sl);
}

/* ---------------------------------------------------------------- free list */

static void fl_insert(tlsf_hdr_t *b)
{
    br_u32 fl;
    br_u32 sl;

    mapping_insert(blk_size(b), &fl, &sl);

    tlsf_link_t *lk = link_of(b);
    lk->prev = BR_NULL;
    lk->next = s_tlsf.blocks[fl][sl];
    if (s_tlsf.blocks[fl][sl] != BR_NULL) {
        link_of(s_tlsf.blocks[fl][sl])->prev = b;
    }
    s_tlsf.blocks[fl][sl] = b;
    s_tlsf.fl_bitmap |= (br_u32)1u << fl;
    s_tlsf.sl_bitmap[fl] |= (br_u32)1u << sl;
}

static void fl_remove(tlsf_hdr_t *b)
{
    br_u32 fl;
    br_u32 sl;

    mapping_insert(blk_size(b), &fl, &sl);

    tlsf_link_t *lk = link_of(b);
    if (lk->prev != BR_NULL) {
        link_of(lk->prev)->next = lk->next;
    } else {
        s_tlsf.blocks[fl][sl] = lk->next;
    }
    if (lk->next != BR_NULL) {
        link_of(lk->next)->prev = lk->prev;
    }
    if (s_tlsf.blocks[fl][sl] == BR_NULL) {
        s_tlsf.sl_bitmap[fl] &= ~((br_u32)1u << sl);
        if (s_tlsf.sl_bitmap[fl] == 0u) {
            s_tlsf.fl_bitmap &= ~((br_u32)1u << fl);
        }
    }
    lk->prev = BR_NULL;
    lk->next = BR_NULL;
}

/* ---------------------------------------------------------------- public surface */

br_size_t br_tlsf_overhead(void)
{
    return (br_size_t)BR_TLSF_OVERHEAD;
}

int br_tlsf_init(void *arena, br_size_t bytes)
{
    if ((arena == BR_NULL) || (bytes < (br_size_t)(2u * BR_TLSF_MIN_BLOCK))) {
        return BR_ERR(EINVAL);
    }
    if (((br_uintptr_t)arena & (br_uintptr_t)(BR_TLSF_ALIGN - 1u)) != 0u) {
        return BR_ERR(EINVAL);
    }
    /* Level-1 index upper bound: fl = log2(size) - 5 must be < 32 ⇒ arena <= 2^36 (64 GiB). */
    if (bytes > ((br_size_t)1u << 36)) {
        return BR_ERR(EINVAL);
    }
    if (s_tlsf.init != BR_FALSE) {
        return BR_ERR(EBUSY);
    }

    s_tlsf.arena      = (br_u8 *)arena;
    s_tlsf.bytes      = bytes & ~(br_size_t)(BR_TLSF_ALIGN - 1u);
    s_tlsf.used       = 0u;
    s_tlsf.free_bytes = s_tlsf.bytes;
    s_tlsf.bad_free   = 0u;
    s_tlsf.fl_bitmap  = 0u;
    for (br_u32 fl = 0u; fl < BR_TLSF_FL_COUNT; fl++) {
        s_tlsf.sl_bitmap[fl] = 0u;
        for (br_u32 sl = 0u; sl < BR_TLSF_SL_COUNT; sl++) {
            s_tlsf.blocks[fl][sl] = BR_NULL;
        }
    }

    /* Whole arena as one free block. Links live at the block tail ⇒ must be at least MIN_BLOCK (checked above). */
    tlsf_hdr_t *b = (tlsf_hdr_t *)(void *)s_tlsf.arena;
    b->size      = s_tlsf.bytes | BR_TLSF_F_FREE;
    b->prev_phys = 0u;
    fl_insert(b);

    s_tlsf.init = BR_TRUE;
    return BR_OK;
}

void *br_tlsf_alloc(br_size_t n)
{
    if (s_tlsf.init == BR_FALSE) {
        return BR_NULL;
    }

    br_size_t req = align_up16(n) + (br_size_t)BR_TLSF_HDR;
    if (req < (br_size_t)BR_TLSF_MIN_BLOCK) {
        req = (br_size_t)BR_TLSF_MIN_BLOCK;
    }

    br_u32 fl;
    br_u32 sl;
    mapping_search(req, &fl, &sl);
    if (fl >= BR_TLSF_FL_COUNT) {
        return BR_NULL;
    }

    br_u32 map = s_tlsf.sl_bitmap[fl] & (~(br_u32)0u << sl);
    if (map == 0u) {
        if ((fl + 1u) >= BR_TLSF_FL_COUNT) {
            return BR_NULL;
        }
        const br_u32 fmap = s_tlsf.fl_bitmap & (~(br_u32)0u << (fl + 1u));
        if (fmap == 0u) {
            return BR_NULL;
        }
        fl  = ctz32(fmap);
        map = s_tlsf.sl_bitmap[fl];
    }
    sl = ctz32(map);

    tlsf_hdr_t *b = s_tlsf.blocks[fl][sl];
    if (b == BR_NULL) {
        return BR_NULL;
    }
    fl_remove(b);

    const br_size_t bsz = blk_size(b);
    if (bsz >= (req + (br_size_t)BR_TLSF_MIN_BLOCK)) {
        /* Split: [b, req) goes to the caller, [rest, bsz-req) returns to the free list. */
        tlsf_hdr_t *rest = (tlsf_hdr_t *)(void *)((br_u8 *)b + req);
        rest->size      = bsz - req;
        rest->prev_phys = req;
        b->size         = req;

        tlsf_hdr_t *after = (tlsf_hdr_t *)(void *)((br_u8 *)rest + rest->size);
        if (pos_in_arena((br_u8 *)after) == BR_TRUE) {
            after->prev_phys = rest->size;
        }
        rest->size |= BR_TLSF_F_FREE;
        fl_insert(rest);
    } else {
        b->size = bsz;   /* hand out the whole block (internal fragmentation) */
    }

    const br_size_t footprint = blk_size(b);
    s_tlsf.used       += footprint;
    s_tlsf.free_bytes -= footprint;
    return blk_user(b);
}

void br_tlsf_free(void *p)
{
    if ((p == BR_NULL) || (s_tlsf.init == BR_FALSE)) {
        return;
    }

    tlsf_hdr_t *b = blk_from_user(p);
    if ((blk_in_arena(b) == BR_FALSE) ||
        (((br_uintptr_t)b & (br_uintptr_t)(BR_TLSF_ALIGN - 1u)) != 0u) ||
        (blk_size(b) < (br_size_t)BR_TLSF_MIN_BLOCK)) {
        s_tlsf.bad_free++;
        return;
    }
    if (blk_is_free(b) == BR_TRUE) {
        s_tlsf.bad_free++;
        return;
    }

    const br_size_t sz = blk_size(b);
    s_tlsf.used       -= sz;
    s_tlsf.free_bytes += sz;

    /* Coalesce with the previous physical block (prev_phys maintained by every split/coalesce, always equal to the previous block's total bytes). */
    if (b->prev_phys != 0u) {
        tlsf_hdr_t *prev = (tlsf_hdr_t *)(void *)((br_u8 *)b - b->prev_phys);
        if ((blk_in_arena(prev) == BR_TRUE) && (blk_is_free(prev) == BR_TRUE) &&
            (blk_size(prev) == b->prev_phys)) {
            fl_remove(prev);
            b = prev;   /* sz is recomputed uniformly below after coalescing */
        }
    }

    /* Total size of the merged region (invariant: used + free == bytes is maintained by the sz adds/subs). */
    br_size_t merged = blk_size(b);
    if (b != blk_from_user(p)) {
        merged = blk_size(b) + sz;   /* b = prev, sz is the clean size of the just-freed block */
    }

    /* Coalesce with the next physical block. */
    tlsf_hdr_t *next = (tlsf_hdr_t *)(void *)((br_u8 *)b + merged);
    if ((pos_in_arena((br_u8 *)next) == BR_TRUE) && (blk_is_free(next) == BR_TRUE)) {
        fl_remove(next);
        merged += blk_size(next);
    }

    b->size = merged | BR_TLSF_F_FREE;

    tlsf_hdr_t *after = (tlsf_hdr_t *)(void *)((br_u8 *)b + merged);
    if (pos_in_arena((br_u8 *)after) == BR_TRUE) {
        after->prev_phys = merged;
    }
    fl_insert(b);
}

br_size_t br_tlsf_block_size(void *p)
{
    if ((p == BR_NULL) || (s_tlsf.init == BR_FALSE)) {
        return 0u;
    }
    const tlsf_hdr_t *b = blk_from_user(p);
    if ((blk_in_arena(b) == BR_FALSE) ||
        (((br_uintptr_t)b & (br_uintptr_t)(BR_TLSF_ALIGN - 1u)) != 0u)) {
        return 0u;
    }
    return blk_size(b);
}

br_size_t br_tlsf_used(void)
{
    return (s_tlsf.init != BR_FALSE) ? s_tlsf.used : 0u;
}

br_size_t br_tlsf_free_bytes(void)
{
    return (s_tlsf.init != BR_FALSE) ? s_tlsf.free_bytes : 0u;
}

br_size_t br_tlsf_max_free(void)
{
    if ((s_tlsf.init == BR_FALSE) || (s_tlsf.fl_bitmap == 0u)) {
        return 0u;
    }

    br_u32 fl = BR_TLSF_FL_COUNT - 1u;
    while ((s_tlsf.fl_bitmap & ((br_u32)1u << fl)) == 0u) {
        fl--;
    }
    const br_u32 map = s_tlsf.sl_bitmap[fl];
    br_u32       sl  = BR_TLSF_SL_COUNT - 1u;
    while ((map & ((br_u32)1u << sl)) == 0u) {
        sl--;
    }
    const tlsf_hdr_t *b = s_tlsf.blocks[fl][sl];
    if (b == BR_NULL) {
        return 0u;
    }
    return blk_size(b) - (br_size_t)BR_TLSF_HDR;
}

/*
 * Whole-heap self-check (the heap side of TC-DBG-041; returns violation count, 0 = clean). Checked:
 *   ① physical chain: size consistency (prev_phys alignment), min block, 16-byte alignment, arena bounds;
 *   ② no two adjacent free blocks;
 *   ③ free-list integrity: back links, nodes really inside the arena, size matches its class, no cycles;
 *   ④ bitmap vs lists: bit set with empty list / list non-empty with bit clear are both violations;
 *   ⑤ accounting consistency: used + free == bytes.
 */
br_u32 br_tlsf_check(void)
{
    if (s_tlsf.init == BR_FALSE) {
        return 0u;
    }

    br_u32 v = 0u;
    const br_u8 *end = s_tlsf.arena + s_tlsf.bytes;

    /* ①/② physical chain. */
    tlsf_hdr_t *b      = (tlsf_hdr_t *)(void *)s_tlsf.arena;
    br_size_t   prev   = 0u;
    br_bool     pfree  = BR_FALSE;
    br_u32      guard  = 0u;
    const br_u32 guard_max = (BR_TLSF_FL_COUNT * BR_TLSF_SL_COUNT) + 4096u;

    while (((const br_u8 *)b < end) && (guard < guard_max)) {
        guard++;
        const br_size_t sz = blk_size(b);
        if (sz < (br_size_t)BR_TLSF_MIN_BLOCK) {
            v++;
        }
        if ((sz & (br_size_t)(BR_TLSF_ALIGN - 1u)) != 0u) {
            v++;
        }
        if (b->prev_phys != prev) {
            v++;
        }
        if (((const br_u8 *)b + sz) > end) {
            v++;
            break;
        }
        if (blk_is_free(b) == BR_TRUE) {
            if (pfree == BR_TRUE) {
                v++;   /* adjacent double free: coalescing missed one */
            }
            /* ② must be in the class it belongs to */
            br_u32 fl;
            br_u32 sl;
            mapping_insert(sz, &fl, &sl);
            const tlsf_hdr_t *it  = s_tlsf.blocks[fl][sl];
            br_bool           hit = BR_FALSE;
            br_u32            g2  = 0u;
            while ((it != BR_NULL) && (g2 < guard_max)) {
                g2++;
                if (it == b) {
                    hit = BR_TRUE;
                    break;
                }
                it = link_of((tlsf_hdr_t *)(void *)it)->next;
            }
            if (hit == BR_FALSE) {
                v++;
            }
        }
        prev  = sz;
        pfree = blk_is_free(b);
        b     = (tlsf_hdr_t *)(void *)((br_u8 *)b + sz);
    }
    if ((const br_u8 *)b != end) {
        v++;
    }

    /* ③/④ free lists and bitmaps. */
    for (br_u32 fl = 0u; fl < BR_TLSF_FL_COUNT; fl++) {
        for (br_u32 sl = 0u; sl < BR_TLSF_SL_COUNT; sl++) {
            const br_bool has = (s_tlsf.blocks[fl][sl] != BR_NULL) ? BR_TRUE : BR_FALSE;
            const br_bool bit = ((s_tlsf.sl_bitmap[fl] >> sl) & 1u) != 0u ? BR_TRUE : BR_FALSE;
            if (has != bit) {
                v++;
            }
            if (has == BR_FALSE) {
                continue;
            }

            tlsf_hdr_t *it = s_tlsf.blocks[fl][sl];
            tlsf_hdr_t *pv = BR_NULL;
            br_u32      g  = 0u;
            while ((it != BR_NULL) && (g < guard_max)) {
                g++;
                if ((blk_in_arena(it) == BR_FALSE) || (blk_is_free(it) == BR_FALSE)) {
                    v++;
                    break;
                }
                br_u32 f2;
                br_u32 s2;
                mapping_insert(blk_size(it), &f2, &s2);
                if ((f2 != fl) || (s2 != sl)) {
                    v++;   /* block is in a class it does not belong to */
                }
                if (link_of(it)->prev != pv) {
                    v++;   /* back link broken */
                }
                pv = it;
                it = link_of(it)->next;
            }
            if ((it != BR_NULL) && (g >= guard_max)) {
                v++;   /* suspected cycle */
            }
        }
        const br_bool fbit = ((s_tlsf.fl_bitmap >> fl) & 1u) != 0u ? BR_TRUE : BR_FALSE;
        const br_bool fhas = (s_tlsf.sl_bitmap[fl] != 0u) ? BR_TRUE : BR_FALSE;
        if (fbit != fhas) {
            v++;
        }
    }

    /* ⑤ accounting consistency (metadata self-consistency, not structural corruption, but still "two truths disagree"). */
    if ((s_tlsf.used + s_tlsf.free_bytes) != s_tlsf.bytes) {
        v++;
    }

    return v;
}
