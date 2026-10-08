/*
 * brickOS prototype v0.1.0 — TLSF 堆(纯算法; caller 提供 arena, 本文件**不打日志**)
 *
 * 权威设计: `docs/3-os-core/3-01-core-api-list.md` §6(br-mem 组的字节堆)+ §14 CA-7
 * (连续内存独立成池, 不走本堆)+ `docs/3-os-core/3-04-memory.md` §3(堆的启动时序:
 * 初始化点 = core.init)。本文件只实现"两级隔离适配"这一算法本体, 不含 region 表、
 * 不含红区/记账 —— 那些在 mem.c(§6 的实现设计), 这里必须能在宿主上单独压测。
 *
 * 一、二级隔离(TLSF)
 *   一级 32 类(2^5)+ 二级每类 32 细分(2^5); `fl_bitmap`/`sl_bitmap` 用位扫描
 *   定位最小可用类 ⇒ 分配/释放 O(1)。映射规则(参数字典见 tlsf_internal.h):
 *     - 块大小 ∈ [32, 63] : fl = 0, sl = size - 32(1 B 粒度)
 *     - 块大小 ∈ [2^k, 2^(k+1)) : fl = k - 5, sl = (size >> (k-5)) - 32
 *   `mapping_search` 在查找前把请求**上取整到所属 sl 槽的上界**, 否则同槽里
 *   可能挑到比请求小的块(这是 TLSF 映射最常见的 off-by-one, 见 br_tlsf_check
 *   对"size 一致性"的校验)。
 *
 * 二、块结构
 *   块头 16 B: {size | FREE 位, prev_phys}(prev_phys = 前一个物理块总字节,
 *   首个块为 0)。分裂/立即合并; 空闲块双向链。
 *
 * ★ 空闲链存**块尾**(link_of), 不存块首 —— 这是本实现与教科书 TLSF 的唯一形态差异:
 *   块首 16 B 载荷被上层 mem.c 用作**冻结的 32 B 块头**的前半部分, 而
 *   `docs/5-debug/5-01-debug.md` §4 的"重复释放拦截"要求块头在 free 之后仍可读
 *   (magic/state 判定)。若链存块首, TLSF 会在 free 里把 next/prev 写进块头,
 *   第二次 free 就只能看到垃圾而无法区分"重复释放"与"野指针"。
 *   代价: 最小块 = 16(头) + 16(链), 与教科书一致, 无额外开销。
 *
 * 三、单实例
 *   原型只有一个堆(§6: "v1 只有全局堆"), 控制块是文件内 static; 接口不接收句柄。
 *   本文件不取锁: v0.1 单核 + 调用方保证串行(thread-only, §11 的 ISR 白名单不含内存)。
 */
#include <br/core/br_error.h>
#include <br/core/br_types.h>

#include "tlsf_internal.h"

/* ---------------------------------------------------------------- 块结构 */

typedef struct tlsf_hdr {
    br_size_t size;       /* 总字节(含本头)| FREE 位 */
    br_size_t prev_phys;  /* 前一物理块总字节; 首个 = 0 */
} tlsf_hdr_t;

#define BR_TLSF_F_FREE   ((br_size_t)1u)

/* 空闲链节点: 只对**空闲块**有效, 落在块尾 16 B。 */
typedef struct tlsf_link {
    tlsf_hdr_t *prev;
    tlsf_hdr_t *next;
} tlsf_link_t;

/* ---------------------------------------------------------------- 控制块 */

static struct {
    br_u8       *arena;
    br_size_t    bytes;
    br_u32       fl_bitmap;
    br_u32       sl_bitmap[BR_TLSF_FL_COUNT];
    tlsf_hdr_t  *blocks[BR_TLSF_FL_COUNT][BR_TLSF_SL_COUNT];
    br_size_t    used;        /* 已分配块的总足迹(含块头) */
    br_size_t    free_bytes;  /* 空闲块的总足迹(含块头); used + free == bytes */
    br_bool      init;
    br_u32       bad_free;    /* TLSF 层拦下的野指针/重复释放(仅供自检参考) */
} s_tlsf;

/* ---------------------------------------------------------------- 小工具 */

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

/* 无 libc: 自持的位扫描(调用者保证 v != 0)。 */
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

/* ---------------------------------------------------------------- 映射 */

static void mapping_insert(br_size_t size, br_u32 *fl, br_u32 *sl)
{
    if (size < (br_size_t)(2u * BR_TLSF_MIN_BLOCK)) {
        /* 小类: [32, 63] 一字节粒度。低于最小块的尺寸归 0(不该出现)。 */
        *fl = 0u;
        *sl = (size >= (br_size_t)BR_TLSF_MIN_BLOCK)
                  ? (br_u32)(size - (br_size_t)BR_TLSF_MIN_BLOCK) : 0u;
    } else {
        const br_u32 k = log2_floor(size);
        *fl = k - BR_TLSF_SL_LOG2;
        *sl = (br_u32)((size >> (k - BR_TLSF_SL_LOG2)) - (br_size_t)BR_TLSF_SL_COUNT);
    }
}

/* 查找用映射: 把请求上取整到所属 sl 槽的上界, 保证被选中的块 >= 请求。 */
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

/* ---------------------------------------------------------------- 空闲链 */

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

/* ---------------------------------------------------------------- 公开面 */

br_size_t br_tlsf_overhead(void)
{
    return (br_size_t)BR_TLSF_OVERHEAD;
}

int br_tlsf_init(void *arena, br_size_t bytes)
{
    if ((arena == BR_NULL) || (bytes < (br_size_t)(2u * BR_TLSF_MIN_BLOCK))) {
        return BR_ERR(BR_EINVAL);
    }
    if (((br_uintptr_t)arena & (br_uintptr_t)(BR_TLSF_ALIGN - 1u)) != 0u) {
        return BR_ERR(BR_EINVAL);
    }
    /* 一级索引上界: fl = log2(size) - 5 必须 < 32 ⇒ arena <= 2^36(64 GiB)。 */
    if (bytes > ((br_size_t)1u << 36)) {
        return BR_ERR(BR_EINVAL);
    }
    if (s_tlsf.init != BR_FALSE) {
        return BR_ERR(BR_EBUSY);
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

    /* 整个 arena 一块空闲。链存块尾 ⇒ 必须至少 MIN_BLOCK(上面已校验)。 */
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
        /* 分裂: [b, req) 分配给调用者, [rest, bsz-req) 回空闲链。 */
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
        b->size = bsz;   /* 整块给出(内部碎片) */
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

    /* 与前一个物理块合并(prev_phys 由每次分裂/合并维护, 恒等于前块总字节)。 */
    if (b->prev_phys != 0u) {
        tlsf_hdr_t *prev = (tlsf_hdr_t *)(void *)((br_u8 *)b - b->prev_phys);
        if ((blk_in_arena(prev) == BR_TRUE) && (blk_is_free(prev) == BR_TRUE) &&
            (blk_size(prev) == b->prev_phys)) {
            fl_remove(prev);
            b = prev;   /* 合并后 sz 在下面统一重算 */
        }
    }

    /* 合并区域总大小(不变量: used + free == bytes 由 sz 的加减维持)。 */
    br_size_t merged = blk_size(b);
    if (b != blk_from_user(p)) {
        merged = blk_size(b) + sz;   /* b = prev, sz 是刚释放块的干净大小 */
    }

    /* 与后一个物理块合并。 */
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
 * 全堆自检(TC-DBG-041 的堆侧; 返回违约数, 0 = 干净)。校验面:
 *   ① 物理链: size 一致性(prev_phys 对齐)、最小块、16 对齐、arena 边界;
 *   ② 相邻块不双空闲;
 *   ③ 空闲链完整性: 反向链接、节点确在 arena 内、size 与所在类一致、无环;
 *   ④ 位图 vs 链: 位有链空/链有位空都算违约;
 *   ⑤ 记账一致性: used + free == bytes。
 */
br_u32 br_tlsf_check(void)
{
    if (s_tlsf.init == BR_FALSE) {
        return 0u;
    }

    br_u32 v = 0u;
    const br_u8 *end = s_tlsf.arena + s_tlsf.bytes;

    /* ①/② 物理链。 */
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
                v++;   /* 相邻双空闲: 合并漏了 */
            }
            /* ② 必须在它应属的类里 */
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

    /* ③/④ 空闲链与位图。 */
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
                    v++;   /* 块在不属于它的类里 */
                }
                if (link_of(it)->prev != pv) {
                    v++;   /* 反向链接断裂 */
                }
                pv = it;
                it = link_of(it)->next;
            }
            if ((it != BR_NULL) && (g >= guard_max)) {
                v++;   /* 疑似成环 */
            }
        }
        const br_bool fbit = ((s_tlsf.fl_bitmap >> fl) & 1u) != 0u ? BR_TRUE : BR_FALSE;
        const br_bool fhas = (s_tlsf.sl_bitmap[fl] != 0u) ? BR_TRUE : BR_FALSE;
        if (fbit != fhas) {
            v++;
        }
    }

    /* ⑤ 记账一致性(元数据自洽, 不属结构破坏, 但同样是"两处真值对不上")。 */
    if ((s_tlsf.used + s_tlsf.free_bytes) != s_tlsf.bytes) {
        v++;
    }

    return v;
}
