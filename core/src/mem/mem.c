/*
 * brickOS prototype v0.1.0 — 内存子系统实现(mem.c):
 *   br_mem_init 认领三池 + TLSF 包装(记账/红区/毒化) + contig 池 + DMA + 观测
 *
 * 权威设计: `docs/3-os-core/3-01-core-api-list.md` §6(br-mem 组 **11 函数**),
 *   §14 CA-6(dma_addr 恒等)/ CA-7(contig 独立成池)/ CA-8(页池);
 *   `docs/3-os-core/3-04-memory.md`(三池由 platform region 表划分; §3 的启动时序 =
 *   core.init, 早于 EARLY 相);
 *   `docs/5-debug/5-01-debug.md` §4(红区 / freelist 毒化 / 重复释放拦截,
 *   per-plugin 归属记账的 v1.x 便宜形态 = owner 标签)。
 *
 * 池的输入是 `br_mm.h` 的 region 表: core 不拥有 RAM, 只按 BR_MM_KIND_* 认领
 * (§6 原文: "platform region 表把 RAM 划为 heap / contig 池 / 页池")。
 *
 * 用户块布局(**冻结的 32 B 块头**, 观测契约 br_heap_block_t 的字段都在里面):
 *
 *   TLSF 块: [ TLSF 头 16B | ------------------- payload ------------------- ]
 *   payload: [ mem 头 32B | 用户区 n | 红区(0xA5) | 尾 8B magic2|size 副本 ]
 *   用户指针 = mem 头 + 32 = TLSF payload + 32
 *
 *   红区长度 = (8 - n % 8) % 8 + BR_MEM_REDZONE(最小 8 B) —— 补到 8 字节边界后
 *   **再留 8 B**, 目的是让"写用户区后一字节"这种最典型的单字节越界一定落在红区里
 *   (TC-LEAK-UNIT 的判据)。TLSF 请求 = 32 + ALIGN8(n) + 8 + 8。
 *
 * 为什么块头里不存 block(实占): 32 B 放不下
 *   {magic,size,block,owner,caller,seq,state,next} 的全宽字段; block 由 n 唯一
 *   推导(见 mem_req)且可由 `br_tlsf_block_size()` 随时问回, 存两份反而多一处真值。
 *
 * 热路径纪律(§6 的"v1 只有全局堆"): br_malloc/br_free **不打日志**;
 * 日志只用于错误与初始化摘要(前缀 "mem: ")。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_types.h>

#include "tlsf_internal.h"

/* =====================================================================
 * 常量与块布局
 * ===================================================================== */

#define BR_MEM_HDR        32u    /* mem 层块头 */
#define BR_MEM_FOOTER     8u     /* 尾部 magic2 + size 副本 */
#define BR_MEM_REDZONE    8u     /* 红区最小长度(在 8 字节补齐之外) */

#define BR_MEM_MAGIC      0x4D454D42u   /* "BMEM" */
#define BR_MEM_FOOT_MAGIC 0x544F4F46u   /* "FOOT" */
#define BR_MEM_RZ_BYTE    0xA5u         /* 红区填充值 */
#define BR_MEM_POISON     0x5Au         /* 释放后用户区毒化值 */

#define BR_MEM_ST_LIVE    0x11A7u
#define BR_MEM_ST_DEAD    0xDEADu

#define BR_OWNER_MAX      16u

/* contig / DMA 池的静态节点上界(原型: 运行期分配的小池, 不做动态扩容)。 */
#define BR_CONTIG_MAX_FREE   64u
#define BR_CONTIG_MAX_ALLOC  64u

typedef struct br_mem_hdr {
    br_u32              magic;   /* [0x00] 头魔数(破坏 ⇒ canary) */
    br_u32              size;    /* [0x04] 用户请求字节(< 4 GiB; 池远小于此) */
    br_u32              seq;     /* [0x08] 分配序号(单调; walk 排序键) */
    br_u16              owner;   /* [0x0C] 归属标签 */
    br_u16              state;   /* [0x0E] LIVE / DEAD(重复释放判据) */
    struct br_mem_hdr  *next;    /* [0x10] 存活块单链 */
    br_uintptr_t        caller;  /* [0x18] 分配点返回地址 */
} br_mem_hdr_t;

_Static_assert(sizeof(br_mem_hdr_t) == BR_MEM_HDR, "mem 块头必须 32 B(布局冻结)");

typedef struct br_mem_footer {
    br_u32 magic;   /* 尾魔数 */
    br_u32 size;    /* size 副本(与头对不上 ⇒ 头部/尾部被踩) */
} br_mem_footer_t;

_Static_assert(sizeof(br_mem_footer_t) == BR_MEM_FOOTER, "尾哨兵必须 8 B");

/* =====================================================================
 * 全局状态(原型单实例; v1 无线程 ⇒ 无锁)
 * ===================================================================== */

static struct {
    br_bool          attempted;   /* 幂等: 首次调用的结果被缓存, 不重建 */
    int              init_rc;
    br_bool          ready;
    br_bool          have_dma;
    br_mem_layout_t  layout;
    br_mem_hdr_t    *live;        /* 存活块单链(push-front ⇒ seq 递减) */
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

/* 归属标签表(5-01 §4 的 v1.x 形态: 只有标签, 没有预算/强制)。 */
static struct {
    const char *name;
} s_owners[BR_OWNER_MAX];
static br_u32     s_owner_n;
static br_owner_t s_owner_cur = BR_OWNER_NONE;

/* =====================================================================
 * 小工具(无 libc)
 * ===================================================================== */

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

/* TLSF 请求 = 块头 32 + 用户区补齐 8 B + 红区 8 + 尾 8。 */
static br_size_t mem_req(br_size_t n)
{
    return (br_size_t)BR_MEM_HDR + mem_align8(n) +
           (br_size_t)BR_MEM_REDZONE + (br_size_t)BR_MEM_FOOTER;
}

static br_u8 *mem_user(const br_mem_hdr_t *h)
{
    return (br_u8 *)(void *)h + BR_MEM_HDR;
}

/* 红区 = [user + n, user + ALIGN8(n) + 8); 尾哨兵紧随其后。 */
static br_u8 *mem_rz_begin(const br_mem_hdr_t *h)
{
    return mem_user(h) + h->size;
}

static br_u8 *mem_foot_at(const br_mem_hdr_t *h)
{
    return mem_user(h) + mem_align8(h->size) + (br_size_t)BR_MEM_REDZONE;
}

/* 用户指针 → 块头(合法性: 在堆池内、按 BR_MALLOC_ALIGN 对齐); 非法 ⇒ NULL。 */
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

/* =====================================================================
 * contig 池(CA-7): 有序空闲链 + 首次适配 + 对齐空洞切块还链 + 释放合并
 *
 * 记录表 vs 空闲链: 分配记录(addr/block/user_size)是**释放时唯一的真值来源**。
 * `br_mem_free_contig(ptr,size)` 的 size 入参不可信 —— 它由调用方记忆, 记错了
 * 会把池切出洞; 所以实现一律按记录里的 block 大小归还, 只把不一致当错误计数
 * (3-04 §2 的"静态划分"精神: 释放边界不能由调用方即兴决定)。
 * ===================================================================== */

typedef struct {
    br_uintptr_t addr;
    br_size_t    size;
} br_contig_free_t;

typedef struct {
    br_uintptr_t addr;    /* 用户块起始 */
    br_size_t    block;   /* 实际占用(16 对齐后) */
    br_size_t    size;    /* 用户请求 */
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

/* 按地址升序插入空闲节点(不合并; 合并由 contig_merge 一次完成)。 */
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

/* 一次线性合并相邻空闲节点(链已按地址升序且互不重叠)。 */
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
        return BR_ERR(BR_ENODEV);
    }
    if (size == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    /* align 必须是 2 的幂(0 与含多个 1 位的值都拒)。 */
    if ((align == 0u) || ((align & (align - 1u)) != 0u)) {
        return BR_ERR(BR_EINVAL);
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
            return BR_ERR(BR_ENOMEM);
        }
        const br_u32 extra = ((front > 0u) ? 1u : 0u) + ((tail > 0u) ? 1u : 0u);
        if ((cp->n_free + extra - 1u) > (br_u32)BR_CONTIG_MAX_FREE) {
            return BR_ERR(BR_ENOMEM);
        }

        cp->allocs[cp->n_alloc].addr  = astart;
        cp->allocs[cp->n_alloc].block = need;
        cp->allocs[cp->n_alloc].size  = size;
        cp->n_alloc++;

        /* 把该空闲节点从链上摘掉, 再把前/后空洞各作为独立节点还链。 */
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

    return BR_ERR(BR_ENOMEM);
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
        s_mem.contig_bad_free++;   /* 不在本池/从未分配: 忽略 + 记错 */
        br_log_warn("mem: contig free 非池内指针 %p", ptr);
        return;
    }

    const br_contig_alloc_t rec = cp->allocs[idx];
    if (size != rec.size) {
        s_mem.contig_size_mismatch++;   /* 按记录释放, 不信任入参 */
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
        br_log_error("mem: contig 空闲链满, %p 未归还(欠账)", ptr);
        return;
    }
    contig_merge(cp);
}

/* =====================================================================
 * 归属标签(③ 记账契约; 5-01 §4 的 v1.x 形态)
 * ===================================================================== */

br_owner_t br_heap_owner_register(const char *name)
{
    if ((name == BR_NULL) || (s_owner_n >= (br_u32)BR_OWNER_MAX)) {
        return BR_OWNER_NONE;
    }
    for (br_u32 i = 0u; i < s_owner_n; i++) {
        if (mem_str_eq(s_owners[i].name, name) == BR_TRUE) {
            return BR_OWNER_NONE;   /* 重名: 不新建 */
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

/* =====================================================================
 * 字节堆(TLSF 包装; §6)
 * ===================================================================== */

void *br_malloc(br_size_t n)
{
    if (s_mem.ready == BR_FALSE) {
        s_mem.n_fail++;
        return BR_NULL;
    }
    if (n > (br_size_t)0xFFFFFFF0u) {   /* size 字段是 u32(块头 32 B 的冻结布局) */
        s_mem.n_fail++;
        return BR_NULL;
    }

    br_u8 *h = (br_u8 *)br_tlsf_alloc(mem_req(n));
    if (h == BR_NULL) {
        s_mem.n_fail++;
        return BR_NULL;   /* 热路径: 不打日志(§6; 计数已足够定位) */
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
        return;   /* §6: free(NULL) = 无操作 */
    }
    if (s_mem.ready == BR_FALSE) {
        s_mem.bad_free++;
        return;
    }

    br_mem_hdr_t *h = mem_hdr_of_user(p);
    if (h == BR_NULL) {
        s_mem.bad_free++;   /* 非本堆/未对齐 */
        br_log_error("mem: free 非本堆指针 %p", p);
        return;
    }

    /* ★ 空闲链存**块尾**(tlsf.c), 所以块头在 free 后仍可读 ⇒ 这里能区分
     *   "已释放"(state=DEAD, 头未复用)与"野指针", 给出 double_free 计数。 */
    if ((h->magic == BR_MEM_MAGIC) && (h->state == BR_MEM_ST_DEAD)) {
        s_mem.double_free++;
        return;
    }
    if (mem_find_live(h) == BR_NULL) {
        s_mem.double_free++;   /* 本堆地址但不在存活链: 重复释放或伪造指针 */
        return;
    }
    if (h->magic != BR_MEM_MAGIC) {
        s_mem.canary_hits++;   /* 头魔数被踩, 仍继续释放以避免泄漏 */
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
        br_log_error("mem: 红区被踩 %u 字节 (blk=%p size=%lu)", bad, p, (unsigned long)n);
    }

    const br_mem_footer_t *f = (const br_mem_footer_t *)(const void *)fo;
    if ((f->magic != BR_MEM_FOOT_MAGIC) || (f->size != (br_u32)n)) {
        s_mem.canary_hits++;
    }

    mem_fill((void *)p, n, (br_u8)BR_MEM_POISON);   /* 毒化用户区(悬垂读可见) */

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
        s_mem.n_fail++;   /* 乘法溢出: §6 的错误面(避免"算小了再越界写") */
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
        return BR_NULL;   /* 原块保持有效(POSIX 语义; n_fail 已由 br_malloc 计) */
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
        return BR_ERR(BR_ENODEV);
    }
    if ((used == BR_NULL) || (total == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    /* 与 br_heap_stats_get 的 used/total 是同一投影: 两者都直接问 TLSF 计数,
     * 不做第二份缓存 —— 否则"记账"与"实况"会各自漂移。 */
    *used  = br_tlsf_used();
    *total = s_mem.layout.heap.size;
    return BR_OK;
}

int br_heap_stats_get(br_heap_stats_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(BR_ENODEV);
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
        return BR_ERR(BR_ENODEV);
    }
    if (fn == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    /* 存活链是 push-front ⇒ 头→尾 seq 严格递减。原地反转即得升序, 回调后翻回。
     * ★ 约束: 回调内**不得**分配/释放(会改链); 只读观测(与 memleak 的用法一致)。 */
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

/* =====================================================================
 * 连续池 / 页池 / DMA(§6; CA-6/CA-7/CA-8)
 * ===================================================================== */

int br_mem_alloc_contig(br_size_t size, br_size_t align, void **out)
{
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(BR_ENODEV);
    }
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
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
        return BR_ERR(BR_EINVAL);
    }
    out->vaddr    = BR_NULL;
    out->dma_addr = 0u;
    out->size     = 0u;

    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(BR_ENODEV);
    }
    if (s_mem.have_dma == BR_FALSE) {
        return BR_ERR(BR_ENODEV);   /* region 表没声明 DMA 池 */
    }
    if (n == 0u) {
        return BR_ERR(BR_EINVAL);
    }

    /* v1 默认即 4 KiB 对齐(CA-6 恒等映射 + 设备侧一致性); BR_DMA_F_ALIGN4K 置位
     * 不改变路径。BR_DMA_F_CACHED 在 v1 是**非目标**(§6 的头部注释: 默认非缓存
     * 是设备一致性的保守选择), 刻意忽略而不报错 —— append-only 属性位。 */
    void *v = BR_NULL;
    const int rc = contig_alloc(&s_dma, n, (br_size_t)BR_PAGE_SIZE, &v);
    if (rc != BR_OK) {
        return rc;
    }

    if ((attrs & BR_DMA_F_ZERO) != 0u) {
        mem_fill(v, n, 0u);
    }
    out->vaddr    = v;
    out->dma_addr = (br_uintptr_t)v;   /* CA-6 / INV-6: v1 恒等 */
    out->size     = n;
    return BR_OK;
}

int br_dma_free(const br_dma_buf_t *b)
{
    if (b == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (s_mem.ready == BR_FALSE) {
        return BR_ERR(BR_ENODEV);
    }
    if (s_mem.have_dma == BR_FALSE) {
        return BR_ERR(BR_ENODEV);
    }
    if (b->vaddr == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (b->dma_addr != (br_uintptr_t)b->vaddr) {
        /* CA-6 的一致性判据: 两个地址是同一处真值的两个视图, 对不上就不归还
         * (归还错地址会把池切坏)。 */
        br_log_error("mem: dma_free 地址不一致 vaddr=%p dma=0x%lx",
                     b->vaddr, (unsigned long)b->dma_addr);
        return BR_ERR(BR_EINVAL);
    }
    contig_free(&s_dma, b->vaddr, b->size);
    return BR_OK;
}

/* =====================================================================
 * 初始化(§6 / 3-04 §3): 认领 region, 建 TLSF + 页位图 + contig/DMA 池
 * ===================================================================== */

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
        /* region 粒度校验: 池必须是 4 KiB 对齐、4 KiB 整数倍(3-01 §6 的
         * BR_PAGE_SIZE 口径; br_mm_region_add 已挡一次, 这里是第二道)。 */
        if (((r->base & (br_uintptr_t)(BR_PAGE_SIZE - 1u)) != 0u) ||
            (r->size == 0u) ||
            ((r->size & (br_size_t)(BR_PAGE_SIZE - 1u)) != 0u)) {
            br_log_error("mem: region[%u] 粒度不合法 base=%p size=%lu",
                         (unsigned)i, (void *)r->base, (unsigned long)r->size);
            return BR_ERR(BR_EINVAL);
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
            break;   /* IMAGE/STACK/MMIO/RESERVED: 不是池, 不认领 */
        }
    }

    if ((heap.size == 0u) || (contig.size == 0u) || (page.size == 0u)) {
        br_log_error("mem: 缺必需池(heap=%lu contig=%lu page=%lu)",
                     (unsigned long)heap.size, (unsigned long)contig.size,
                     (unsigned long)page.size);
        return BR_ERR(BR_ENODEV);
    }

    /* 池太小: 分别给出下界, 避免建出一个"永远分配不出东西"的池。 */
    if (heap.size < ((br_size_t)BR_TLSF_OVERHEAD * 2u)) {
        return BR_ERR(BR_ENOMEM);
    }
    if (contig.size < ((br_size_t)BR_TLSF_ALIGN * 2u)) {
        return BR_ERR(BR_ENOMEM);
    }
    if (page.size < (br_size_t)BR_PAGE_SIZE) {
        return BR_ERR(BR_ENOMEM);
    }
    if ((have_dma == BR_TRUE) && (dma.size < (br_size_t)BR_PAGE_SIZE)) {
        return BR_ERR(BR_ENOMEM);
    }

    int rc = br_tlsf_init((void *)heap.base, heap.size);
    if (rc != BR_OK) {
        br_log_error("mem: TLSF 初始化失败 rc=%d", rc);
        return rc;
    }
    rc = br_page_pool_init(page.base, page.size);
    if (rc != BR_OK) {
        br_log_error("mem: 页池初始化失败 rc=%d", rc);
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
        return s_mem.init_rc;   /* 幂等: 二次调用返回首次结果, 不重建 */
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
        return BR_ERR(BR_ENODEV);
    }
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    *out = s_mem.layout;
    return BR_OK;
}
