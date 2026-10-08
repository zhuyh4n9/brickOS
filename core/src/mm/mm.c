/*
 * brickOS prototype v0.1.0 — MMU / region 表 / cache 派发(mm.c)
 *
 * 权威设计: `docs/3-os-core/3-01-core-api-list.md` §7(br-mm 组 5 函数)+ §13.6
 *   (特权分级的 memory 三轴: ops × granularity × regions)+
 *   `docs/3-os-core/3-04-memory.md`(恒等映射 + 属性隔离; region 表静态声明)。
 *
 * 三层模式(设计 1-01 §8): **接口在 core / 构造在 platform / 数据在 platform
 * 的 region 表**。本文件是 core 侧的两件事:
 *   ① region 表(静态数组, BR_MM_MAX_REGIONS; 查重的唯一真值);
 *   ② 对 `br_mm_ops_t` 的派发(状态机归 core, 页表构造归 platform)。
 *
 * v1 的虚拟内存政策 = 恒等映射 + 属性隔离(3-04 §1):
 *   - `br_mm_map`/`br_mm_unmap` **签名先行**(主文档风险 R4), v1 一律 -ENOTSUP
 *     (TC-MM-003);
 *   - region 表在 platform early_init 里声明, 页表由 `ops.activate()` 一次构造;
 *   - 激活之后追加的 region **顺带**经 `ops.map_region` 落表; 落表失败就回滚表项,
 *     绝不留"表与页表不一致"的第二真值(设计 §7 的一致性义务)。
 *
 * 不取锁: v0.1 单核, region 表与激活标志都是启动期/一致性用例期访问(thread-only)。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>   /* BR_PAGE_SIZE: region 粒度与池粒度同源(§6/§7) */
#include <br/core/br_mm.h>
#include <br/core/br_types.h>

static br_mm_region_t    s_regions[BR_MM_MAX_REGIONS];
static br_u32            s_count;
static const br_mm_ops_t *s_ops;
static br_bool           s_active;

/* ---------------------------------------------------------------- 只读观测面 */

br_u32 br_mm_region_count(void)
{
    return s_count;
}

const br_mm_region_t *br_mm_region_get(br_u32 index)
{
    if (index >= s_count) {
        return BR_NULL;
    }
    return &s_regions[index];
}

const br_mm_region_t *br_mm_region_find(br_uintptr_t addr)
{
    for (br_u32 i = 0u; i < s_count; i++) {
        const br_mm_region_t *r = &s_regions[i];
        if ((addr >= r->base) && (addr < (r->base + r->size))) {
            return r;
        }
    }
    return BR_NULL;
}

br_u32 br_mm_region_count_kind(br_u32 kind)
{
    br_u32 n = 0u;
    for (br_u32 i = 0u; i < s_count; i++) {
        if (BR_MM_KIND(s_regions[i].attrs) == kind) {
            n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------- native 面(§7) */

int br_mm_region_add(const br_mm_region_t *r)
{
    if ((r == BR_NULL) || (r->size == 0u)) {
        return BR_ERR(BR_EINVAL);
    }
    if (((r->base & (br_uintptr_t)(BR_PAGE_SIZE - 1u)) != 0u) ||
        ((r->size & (br_size_t)(BR_PAGE_SIZE - 1u)) != 0u)) {
        return BR_ERR(BR_EINVAL);
    }
    if ((r->base + r->size) < r->base) {
        return BR_ERR(BR_EINVAL);   /* 地址回绕 */
    }

    /* 重叠判定: 半开区间相交。**完全相同也算重叠**(TC-MM-001 要求"重叠/重复"都红),
     * 因为它会让同一个物理页出现两条属性声明 —— 页表只认一条, 另一条就是第二真值。 */
    const br_uintptr_t nlo = r->base;
    const br_uintptr_t nhi = r->base + r->size;
    for (br_u32 i = 0u; i < s_count; i++) {
        const br_uintptr_t lo = s_regions[i].base;
        const br_uintptr_t hi = s_regions[i].base + s_regions[i].size;
        if ((nlo < hi) && (lo < nhi)) {
            return BR_ERR(BR_EEXIST);
        }
    }

    if (s_count >= (br_u32)BR_MM_MAX_REGIONS) {
        return BR_ERR(BR_ENOSPC);
    }

    s_regions[s_count] = *r;
    s_count++;

    /* 已激活: 表与页表必须同步长大; 失败则回滚表项(不留"表里有、页表没有")。 */
    if (s_active == BR_TRUE) {
        if ((s_ops == BR_NULL) || (s_ops->map_region == BR_NULL)) {
            s_count--;
            return BR_ERR(BR_ENOTSUP);
        }
        const int rc = s_ops->map_region(r);
        if (rc != BR_OK) {
            s_count--;
            br_log_error("mem/mm: map_region 落表失败 rc=%d, 已回滚 region 表项", rc);
            return rc;
        }
    }
    return BR_OK;
}

int br_mm_map(const br_mm_region_t *r)
{
    (void)r;
    return BR_ERR(BR_ENOTSUP);   /* v1 签名先行(TC-MM-003) */
}

int br_mm_unmap(br_uintptr_t addr, br_size_t size)
{
    (void)addr;
    (void)size;
    return BR_ERR(BR_ENOTSUP);   /* v1 签名先行(TC-MM-003) */
}

/*
 * cache 维护(§7; 驱动 DMA 前后, 风险 R4)。语义按 §13.6 的正交表:
 *   ① 地址未声明 ⇒ -EINVAL(原型刻意不"猜"属性 —— 见 ADR-0003, TC-MM-EXTRA);
 *   ② size == 0 ⇒ 0(没有任何字节要维护);
 *   ③ region 未标 CACHED(DEVICE/普通)⇒ 0(无操作);
 *   ④ 未激活 MMU(cache 本就未开)⇒ 0;
 *   ⑤ 否则经 ops 派发; ops 缺失 ⇒ -ENOTSUP。
 */
static int mm_cache_dispatch(void *addr, br_size_t size, br_bool flush)
{
    if (size == 0u) {
        return BR_OK;
    }
    const br_mm_region_t *r = br_mm_region_find((br_uintptr_t)addr);
    if (r == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if ((r->attrs & BR_MM_CACHED) == 0u) {
        return BR_OK;
    }
    if (s_active == BR_FALSE) {
        return BR_OK;
    }
    if (s_ops == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    if (flush == BR_TRUE) {
        if (s_ops->cache_flush == BR_NULL) {
            return BR_ERR(BR_ENOTSUP);
        }
        s_ops->cache_flush(addr, size);
    } else {
        if (s_ops->cache_invalidate == BR_NULL) {
            return BR_ERR(BR_ENOTSUP);
        }
        s_ops->cache_invalidate(addr, size);
    }
    return BR_OK;
}

int br_mm_cache_flush(void *addr, br_size_t size)
{
    return mm_cache_dispatch(addr, size, BR_TRUE);
}

int br_mm_cache_invalidate(void *addr, br_size_t size)
{
    return mm_cache_dispatch(addr, size, BR_FALSE);
}

/* ---------------------------------------------------------------- platform 面 */

int br_mm_register(const br_mm_ops_t *ops)
{
    if (ops == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (s_ops != BR_NULL) {
        return BR_ERR(BR_EBUSY);   /* 一个镜像恰一个 platform 页表构造者 */
    }
    s_ops = ops;
    return BR_OK;
}

int br_mm_activate(void)
{
    if ((s_ops == BR_NULL) || (s_ops->activate == BR_NULL)) {
        return BR_ERR(BR_ENOTSUP);   /* 未注册 */
    }
    if (s_active == BR_TRUE) {
        return BR_ERR(BR_EBUSY);
    }

    const int rc = s_ops->activate(s_regions, s_count);
    if (rc != BR_OK) {
        return rc;   /* 原样上传, **不**置位: 失败后仍可重试激活 */
    }
    s_active = BR_TRUE;
    return BR_OK;
}

br_bool br_mm_active(void)
{
    return s_active;
}

int br_mm_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs)
{
    if ((s_active == BR_FALSE) || (s_ops == BR_NULL) || (s_ops->set_attrs == BR_NULL)) {
        return BR_ERR(BR_ENOTSUP);
    }
    return s_ops->set_attrs(addr, size, attrs);
}

int br_mm_query(br_uintptr_t addr, br_size_t size, br_u32 *out_attrs)
{
    if (out_attrs == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if ((s_active == BR_FALSE) || (s_ops == BR_NULL) || (s_ops->query == BR_NULL)) {
        return BR_ERR(BR_ENOTSUP);
    }
    return s_ops->query(addr, size, out_attrs);
}
