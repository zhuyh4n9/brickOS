/*
 * brickOS prototype v0.1.0 — 内存映射子系统一致性用例(in-image conformance)
 *
 * 设计依据:
 *   - `docs/6-test/6-01-test.md` §3.5(TC-MEM-* 内存组)/§3.6(TC-MM-* MMU 组);
 *   - `docs/3-os-core/3-04-memory.md` §1(v1 政策 = 恒等映射 + 属性隔离);
 *   - `docs/3-os-core/3-01-core-api-list.md` §6/§7(三池 + region 表 + cache 维护)。
 *
 * 为什么是 **in-image** 而不是 host 用例:
 *   6-01 §3.6 已把 MMU 组标成 target-only —— "恒等映射""真保护页""翻译 fault"这三件事
 *   在 host 上根本不存在(host 侧只验算法性质, 见 `make mem-test`)。本文件把用例编进
 *   镜像, 由 `br_plat_mem_conformance()` 在启动期跑一遍, 每项打一行
 *   `[MEMCONF] PASS/FAIL <tag> <说明>`, 末尾打 `[MEMCONF] SUMMARY pass=N fail=0 total=N`
 *   —— 于是红绿由 `make smoke` / `make dbg-test` 的 grep 判定, 不靠人眼。
 *
 * 覆盖:
 *   MM-ACTIVE  MMU 真的开着(br_mm_active() + SCTLR_EL1.M 硬件回读)
 *   MM-IDENT   抽样 8 个地址: 已映射 + 页表 PA == VA 页框(恒等映射的唯一判据)
 *   MM-POOLS   四池与 region 表一致 / 页总数换算 / 互不重叠 / 都在 8 MiB 窗口内
 *   MM-REGION  region 表非空、kind 可识别、find 命中与未命中
 *   MM-RO      **真保护**: RO 页上的写探针必须取 permission fault
 *   MM-NXDESC  NX 位能被 query 读回(不真的跳过去执行, 见该用例注释)
 *   MM-UNMAP   窗口外未映射地址 ⇒ translation fault, 且可恢复
 *   TC-MEM-001..007  三池 + DMA + 记账的最小断言(6-01 §3.5 在 target 上的投影)
 *   TC-MM-001..003   region_add 的错误码 / DEVICE 区 cache_flush / map-unmap 未实现
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>

#include <br/platform/br_mmu.h>
#include <br/platform/br_plat.h>   /* br_plat_mem_conformance 的入口原型(唯一真值) */

/* =====================================================================
 * 断言与统计(与 irq_conf.c 同型: 失败数就是返回值, 供 Makefile 判红绿)
 * ===================================================================== */
static br_u32 s_pass;
static br_u32 s_fail;

static void conf_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_pass++;
        br_log_info("[MEMCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[MEMCONF] FAIL %s %s", tag, what);
    }
}

/* =====================================================================
 * extable 探针(3-02 §10.4; 与 irq_conf.c 的 conf_probe_read32 同型)
 *
 * ★ 返回值的**唯一来源**是 core 按表项 `reg`/`errno_val` 写进异常帧的那个寄存器(x0);
 *   成功路径自己 `mov %w[ret], wzr`。修复标签本身不造错误码 —— 否则这个探针就验证不了
 *   "表项必须带 reg + errno_val"这条设计结论(§10.4 的易错点)。
 * ===================================================================== */
static int conf_probe_read32(br_uintptr_t addr, br_u32 *out)
{
    register br_u32       val __asm__("x1");
    register br_s32       ret __asm__("x0");
    register br_uintptr_t a   __asm__("x2");

    a = addr;
    __asm__ volatile(
        ".Lmconf_ld" "%=:\n\t"
        "   ldr %w[val], [%[addr]]\n\t"
        "   mov %w[ret], wzr\n\t"
        "   b   .Lmconf_ldone" "%=\n\t"
        ".Lmconf_lfix" "%=:\n\t"
        "   b   .Lmconf_ldone" "%=\n\t"
        ".Lmconf_ldone" "%=:\n\t"
        BR_EXTABLE_ENTRY(.Lmconf_ld%=, .Lmconf_lfix%=, 0, -14)
        : [val] "=&r"(val), [ret] "=&r"(ret)
        : [addr] "r"(a)
        : "memory");

    if (ret == 0 && out != BR_NULL) {
        *out = val;
    }
    return ret;
}

/* 写探针: MM-RO 要验的正是"只读页上的写取 permission fault"——读探针验不了这件事。
 * 与读探针唯一的差别是故障指令是 `str`, 其余(表项 reg=0 / errno=-14 / 修复标签)照抄。 */
static int conf_probe_write32(br_uintptr_t addr, br_u32 val)
{
    register br_s32       ret __asm__("x0");
    register br_u32       v   __asm__("x1");
    register br_uintptr_t a   __asm__("x2");

    a = addr;
    v = val;
    __asm__ volatile(
        ".Lmconf_st" "%=:\n\t"
        "   str %w[val], [%[addr]]\n\t"
        "   mov %w[ret], wzr\n\t"
        "   b   .Lmconf_sdone" "%=\n\t"
        ".Lmconf_sfix" "%=:\n\t"
        "   b   .Lmconf_sdone" "%=\n\t"
        ".Lmconf_sdone" "%=:\n\t"
        BR_EXTABLE_ENTRY(.Lmconf_st%=, .Lmconf_sfix%=, 0, -14)
        : [ret] "=&r"(ret)
        : [addr] "r"(a), [val] "r"(v)
        : "memory");

    return ret;
}

/* =====================================================================
 * 小工具
 * ===================================================================== */
static const br_mm_region_t *conf_region_of_kind(br_u32 kind)
{
    const br_u32 n = br_mm_region_count();

    for (br_u32 i = 0u; i < n; i++) {
        const br_mm_region_t *r = br_mm_region_get(i);

        if (r != BR_NULL && BR_MM_KIND(r->attrs) == kind) {
            return r;
        }
    }
    return BR_NULL;
}

static br_bool conf_kind_known(br_u32 kind)
{
    return (kind >= BR_MM_KIND_IMAGE && kind <= BR_MM_KIND_RESERVED) ? BR_TRUE : BR_FALSE;
}

/* region 属性构造: kind 必须走 BR_MM_WITH_KIND 移位, 直接 `| BR_MM_KIND_x` 会把
 * 取值 8 写进属性低半字节(那一位不属于 kind 字段) —— 这是本头文件的经典误用。 */
static br_u32 conf_attrs_kind(br_u32 attrs, br_u32 kind)
{
    return BR_MM_WITH_KIND(attrs, kind);
}

/* =====================================================================
 * MM-ACTIVE / MM-IDENT
 * ===================================================================== */
static br_u32 s_marker = 0x1D3E7u;      /* 镜像内变量(MM-IDENT 的抽样点之一) */

static void conf_mm_active(void)
{
    br_plat_mmu_info_t info;

    br_plat_mmu_info(&info);

    const br_bool m_bit = ((info.sctlr & 1ull) != 0ull) ? BR_TRUE : BR_FALSE;
    br_log_info("[MEMCONF] info SCTLR_EL1=0x%lx M=%u TCR_EL1=0x%lx MAIR_EL1=0x%lx TTBR0_EL1=0x%lx",
                info.sctlr, (br_u32)m_bit, info.tcr, info.mair, info.ttbr0);

    conf_report(br_mm_active() && m_bit, "MM-ACTIVE",
                "br_mm_active() 为真且 SCTLR_EL1.M(bit0)==1(MMU 已在跑)");
}

static void conf_mm_ident(void)
{
    br_mem_layout_t lay;
    volatile br_u32 stack_local = 0x5A5A5A5Au;
    br_uintptr_t    addrs[8];
    br_u32          bad = 0u;

    if (br_mem_layout(&lay) != 0) {
        conf_report(BR_FALSE, "MM-IDENT", "取池布局失败, 无法抽样池地址");
        return;
    }

    const br_mm_region_t *res = conf_region_of_kind(BR_MM_KIND_RESERVED);
    if (res == BR_NULL) {
        conf_report(BR_FALSE, "MM-IDENT", "region 表里没有 RESERVED 区");
        return;
    }

    /* 8 个抽样点: 镜像内变量 / 栈上局部 / 堆池首尾 / 页池首尾 / contig 池首 / 保留区首 */
    addrs[0] = (br_uintptr_t)&s_marker;
    addrs[1] = (br_uintptr_t)&stack_local;
    addrs[2] = lay.heap.base;
    addrs[3] = lay.heap.base + lay.heap.size - BR_PAGE_SIZE;
    addrs[4] = lay.page.base;
    addrs[5] = lay.page.base + lay.page.size - BR_PAGE_SIZE;
    addrs[6] = lay.contig.base;
    addrs[7] = res->base;

    for (br_u32 i = 0u; i < BR_ARRAY_SIZE(addrs); i++) {
        br_u32       attrs = 0u;
        br_uintptr_t pa    = 0u;

        const int rq = br_plat_mmu_probe(addrs[i], &attrs);
        const int rt = br_plat_mmu_translate(addrs[i], &pa);
        const br_bool identity = (pa == (addrs[i] & ~(br_uintptr_t)(BR_PAGE_SIZE - 1u)))
                                 ? BR_TRUE : BR_FALSE;

        br_log_info("[MEMCONF] info ident[%u] va=0x%lx pa=0x%lx attrs=0x%x query=%d translate=%d",
                    i, (br_u64)addrs[i], (br_u64)pa, attrs, rq, rt);

        if (rq != 0 || rt != 0 || !identity) {
            bad++;
        }
    }

    conf_report(bad == 0u, "MM-IDENT",
                "抽样 8 个地址(镜像/栈/堆首尾/页池首尾/contig/保留区): 已映射且页表 PA == VA 页框");
}

/* =====================================================================
 * MM-POOLS
 * ===================================================================== */
static void conf_mm_pools(void)
{
    br_mem_layout_t lay;
    const br_pool_t *pools[4];
    br_u32           kinds[4] = { BR_MM_KIND_HEAP, BR_MM_KIND_CONTIG,
                                  BR_MM_KIND_PAGE, BR_MM_KIND_DMA };
    br_u32           mismatch = 0u;
    br_u32           outside  = 0u;
    br_u32           overlap  = 0u;

    if (br_mem_layout(&lay) != 0) {
        conf_report(BR_FALSE, "MM-POOLS", "br_mem_layout() 返回非 0(池未就绪)");
        return;
    }

    pools[0] = &lay.heap;
    pools[1] = &lay.contig;
    pools[2] = &lay.page;
    pools[3] = &lay.dma;

    /* (a) 四池 base/size 与 region 表逐条一致: 池是"认领 region"来的, 不该有第二个几何 */
    for (br_u32 i = 0u; i < 4u; i++) {
        const br_mm_region_t *r = conf_region_of_kind(kinds[i]);

        if (r == BR_NULL || r->base != pools[i]->base || r->size != pools[i]->size) {
            br_log_info("[MEMCONF] info pool[%u] kind=%u layout=(0x%lx,%lu) region=%s",
                        i, kinds[i], (br_u64)pools[i]->base, (br_u64)pools[i]->size,
                        (r == BR_NULL) ? "(none)" : "mismatch");
            mismatch++;
        }

        if (pools[i]->base < BR_PLAT_RAM_BASE
            || (pools[i]->base + pools[i]->size) > BR_PLAT_RAM_WINDOW_END) {
            outside++;
        }
    }
    conf_report(mismatch == 0u, "MM-POOLS",
                "四池 base/size 与 region 表逐条一致(池几何 = 声明面, 无第二真值)");

    /* (b) 页池换算: page_total 页 × 4 KiB == 页池字节数 */
    conf_report(lay.page_size == BR_PAGE_SIZE
                && (br_size_t)lay.page_total * BR_PAGE_SIZE == lay.page.size,
                "MM-POOLS",
                "page_size==4096 且 page_total*4096 == 页池 size");

    /* (c) 四池两两不重叠 */
    for (br_u32 i = 0u; i < 4u; i++) {
        for (br_u32 j = i + 1u; j < 4u; j++) {
            const br_uintptr_t ai = pools[i]->base;
            const br_uintptr_t bi = ai + pools[i]->size;
            const br_uintptr_t aj = pools[j]->base;
            const br_uintptr_t bj = aj + pools[j]->size;

            if (ai < bj && aj < bi) {
                overlap++;
            }
        }
    }
    conf_report(overlap == 0u, "MM-POOLS", "四池两两不重叠");

    /* (d) 都在 8 MiB 页表窗口内(窗口外就是未映射 ⇒ 池一旦越界, 第一次分配就 fault) */
    conf_report(outside == 0u, "MM-POOLS", "四池都落在 8 MiB RAM 映射窗口内");
}

/* =====================================================================
 * MM-REGION
 * ===================================================================== */
static void conf_mm_region(void)
{
    const br_u32 n = br_mm_region_count();
    br_u32       bad_kind = 0u;

    br_log_info("[MEMCONF] info region table: %u entries (max %u)", n, (br_u32)BR_MM_MAX_REGIONS);
    conf_report(n > 0u && n <= BR_MM_MAX_REGIONS, "MM-REGION",
                "br_mm_region_count() > 0 且未越表容量");

    for (br_u32 i = 0u; i < n; i++) {
        const br_mm_region_t *r = br_mm_region_get(i);

        if (r == BR_NULL || !conf_kind_known(BR_MM_KIND(r->attrs))) {
            bad_kind++;
            continue;
        }
        br_log_info("[MEMCONF] info   [%u] base=0x%lx size=%lu attrs=0x%x kind=%u",
                    i, (br_u64)r->base, (br_u64)r->size, r->attrs, BR_MM_KIND(r->attrs));
    }
    conf_report(bad_kind == 0u, "MM-REGION", "每条 region 的 kind 都能被识别(image..reserved)");

    const br_mm_region_t *img = conf_region_of_kind(BR_MM_KIND_IMAGE);
    const br_mm_region_t *hit = (img != BR_NULL) ? br_mm_region_find(img->base) : BR_NULL;

    conf_report(img != BR_NULL && hit != BR_NULL
                && hit->base == img->base && hit->size == img->size,
                "MM-REGION", "region_find(镜像基址) 命中且 base/size 与声明一致");

    conf_report(br_mm_region_find((br_uintptr_t)BR_PLAT_CONF_UNMAPPED) == BR_NULL,
                "MM-REGION", "未声明地址(0x42000000, 窗口外)⇒ region_find == NULL");
}

/* =====================================================================
 * MM-RO / MM-NXDESC / MM-UNMAP(真保护与可恢复 fault)
 * ===================================================================== */
static void conf_mm_ro(void)
{
    const br_mm_region_t *res = conf_region_of_kind(BR_MM_KIND_RESERVED);

    if (res == BR_NULL || res->size < (2u * BR_PAGE_SIZE)) {
        conf_report(BR_FALSE, "MM-RO", "保留区缺失或不足两页");
        return;
    }

    const br_uintptr_t page = res->base;
    br_u32             attrs = 0u;

    const int rs = br_mm_set_attrs(page, BR_PAGE_SIZE,
                                   conf_attrs_kind(BR_MM_CACHED | BR_MM_RO, BR_MM_KIND_RESERVED));
    const int rq = br_mm_query(page, BR_PAGE_SIZE, &attrs);

    br_log_info("[MEMCONF] info RO page=0x%lx set_attrs=%d query_attrs=0x%x",
                (br_u64)page, rs, attrs);
    conf_report(rs == 0 && rq == 0 && (attrs & BR_MM_RO) != 0u, "MM-RO",
                "set_attrs(RO) 成功且 query 从**页表**读回 BR_MM_RO");

    /* 真保护: 写探针必须命中修复(permission fault, EC=0x25/DFSC=0x0F 一类) */
    const int wr = conf_probe_write32(page, 0xDEADBEEFu);
    conf_report(wr == BR_ERR(BR_EFAULT), "MM-RO",
                "只读页上的写取 permission fault ⇒ extable 修复为 -EFAULT(真保护, 非影子)");

    /* 收尾: 属性是可撤销的, 改回 RW 后必须能正常写 —— 否则"保护"与"砖"没区别 */
    const int rs2 = br_mm_set_attrs(page, BR_PAGE_SIZE,
                                    conf_attrs_kind(BR_MM_CACHED, BR_MM_KIND_RESERVED));
    const int wr2 = conf_probe_write32(page, 0x5A5A5A5Au);
    br_u32    back = 0u;
    const int rd = conf_probe_read32(page, &back);

    conf_report(rs2 == 0 && wr2 == 0 && rd == 0 && back == 0x5A5A5A5Au, "MM-RO",
                "改回 RW 后可正常写回读(保护可撤销, 不把页留在只读态给后续用例)");
}

static void conf_mm_nxdesc(void)
{
    const br_mm_region_t *res = conf_region_of_kind(BR_MM_KIND_RESERVED);

    if (res == BR_NULL || res->size < (2u * BR_PAGE_SIZE)) {
        conf_report(BR_FALSE, "MM-NXDESC", "保留区缺失或不足两页");
        return;
    }

    const br_uintptr_t page = res->base + BR_PAGE_SIZE;
    br_u32             attrs = 0u;

    const int rs = br_mm_set_attrs(page, BR_PAGE_SIZE,
                                   conf_attrs_kind(BR_MM_CACHED | BR_MM_NX, BR_MM_KIND_RESERVED));
    const int rq = br_mm_query(page, BR_PAGE_SIZE, &attrs);

    br_log_info("[MEMCONF] info NX page=0x%lx set_attrs=%d query_attrs=0x%x",
                (br_u64)page, rs, attrs);
    conf_report(rs == 0 && rq == 0 && (attrs & BR_MM_NX) != 0u, "MM-NXDESC",
                "set_attrs(NX) 后 query 读回 BR_MM_NX(PXN|UXN 同时置位)");

    /*
     * ★ 刻意**不**真的跳到这一页上取指: executing-NX 走 instruction abort(EC=0x20/0x21),
     *   而 core 的 extable 只覆盖 data abort(3-02 §10.4/irq_fault.c 的 EC 判定)⇒
     *   跳过去必然走 fatal panic, 而不是"降级为错误码"。NX 的落点验证止于"描述符位生效",
     *   这是本原型的能力边界, 不是漏测。
     */
    const int rs2 = br_mm_set_attrs(page, BR_PAGE_SIZE,
                                    conf_attrs_kind(BR_MM_CACHED, BR_MM_KIND_RESERVED));
    conf_report(rs2 == 0, "MM-NXDESC", "恢复可执行(RW 非 NX), 不把 NX 泄漏给后续用例");
}

/*
 * Device 2 MiB 块的 set_attrs 边界(页表在 2 MiB 粒度上表达不了子块属性)。
 * 这条裁定值一条显式用例: 边界若被"顺手放宽"成"按 2 MiB 生效", 一个只想保护 4 KiB
 * 的调用会把整块外设窗口变成只读 —— 症状出现在下一次 ISR 访问寄存器时, 离根因很远。
 */
static void conf_mm_devblk(void)
{
    /* ① 子块范围(PL011 帧只有 4 KiB, 落在一个 2 MiB 块里)⇒ 明确 -ENOTSUP */
    const int r_sub = br_mm_set_attrs(0x09000000ul, BR_PAGE_SIZE, BR_MM_DEVICE | BR_MM_RO);

    /* ② 整块对齐的范围可以整块改。挑一块**没有任何人在用**的设备窗口
     *    (0x0E000000: QEMU virt 上既不是 GIC/GICR, 也不是 UART/virtio),
     *    免得把正在用的寄存器帧改成只读 —— 那会变成下一次 ISR 里的 permission fault。 */
    const br_uintptr_t blk = 0x0E000000ul;
    br_u32 attrs = 0u;
    const int r_w = br_mm_set_attrs(blk, 0x200000ul, BR_MM_DEVICE | BR_MM_RO);
    const int r_q = br_mm_query(blk, BR_PAGE_SIZE, &attrs);
    const int r_r = br_mm_set_attrs(blk, 0x200000ul, BR_MM_DEVICE);

    br_u32 back = 0u;
    const int r_b = br_mm_query(blk, BR_PAGE_SIZE, &back);

    br_log_info("[MEMCONF] info devblk sub=%d whole=%d query=%d attrs=0x%x restore=%d back=0x%x",
                r_sub, r_w, r_q, attrs, r_r, back);
    conf_report(r_sub == BR_ERR(BR_ENOTSUP), "MM-DEVBLK",
                "Device 2 MiB 块上的子块 set_attrs(4 KiB)⇒ -ENOTSUP(硬件表达不了就不假装支持)");
    conf_report(r_w == 0 && r_q == 0 && (attrs & BR_MM_RO) != 0u && (attrs & BR_MM_DEVICE) != 0u
                && r_r == 0 && r_b == 0 && (back & BR_MM_RO) == 0u,
                "MM-DEVBLK", "整块对齐的 Device 2 MiB 范围可整块改 RO 并读回, 随后恢复 RW");
}

static void conf_mm_unmap(void)
{
    br_uintptr_t pa = 0u;
    br_u32       v  = 0u;

    /* 先确认它真的未映射(翻译失败), 再确认"访问它"取的是可恢复 fault */
    const int rt = br_plat_mmu_translate((br_uintptr_t)BR_PLAT_CONF_UNMAPPED, &pa);
    br_log_info("[MEMCONF] info unmap probe va=0x%lx translate=%d", (br_u64)BR_PLAT_CONF_UNMAPPED, rt);
    conf_report(rt == BR_ERR(BR_EFAULT), "MM-UNMAP",
                "窗口外地址(0x42000000)在页表里 invalid ⇒ 翻译失败(-EFAULT)");

    const int r1 = conf_probe_read32((br_uintptr_t)BR_PLAT_CONF_UNMAPPED, &v);
    conf_report(r1 == BR_ERR(BR_EFAULT), "MM-UNMAP",
                "读未映射地址 ⇒ translation fault 被 extable 修复为 -EFAULT, 系统存活");

    /* 第二次: 若 in_fault 没复位, 这一次会被当成 double fault(裸 panic) */
    const int r2 = conf_probe_read32((br_uintptr_t)BR_PLAT_CONF_UNMAPPED, &v);
    conf_report(r2 == BR_ERR(BR_EFAULT), "MM-UNMAP",
                "再次读仍为 -EFAULT(in_fault 已复位 ⇒ 故障是可恢复的)");
}

/* =====================================================================
 * TC-MEM-001..007(6-01 §3.5 在 target 上的最小断言)
 * ===================================================================== */
static void conf_tc_mem_001(void)
{
    void *p = br_malloc(37u);
    void *q = br_malloc(200u);

    const br_bool ok = (p != BR_NULL) && (q != BR_NULL)
                       && (((br_uintptr_t)p % BR_MALLOC_ALIGN) == 0u)
                       && (((br_uintptr_t)q % BR_MALLOC_ALIGN) == 0u);

    br_log_info("[MEMCONF] info malloc(37)=%p malloc(200)=%p align=%u",
                p, q, (br_u32)BR_MALLOC_ALIGN);
    br_free(p);
    br_free(q);
    conf_report(ok, "TC-MEM-001", "malloc 返回地址 16 B 对齐(>= BR_MALLOC_ALIGN)");
}

static void conf_tc_mem_002(void)
{
    enum { CONF_BLOCKS = 128 };
    void     *blk[CONF_BLOCKS];
    br_size_t sz[CONF_BLOCKS];
    br_u32    bad = 0u;

    /* 显式清零点: 让"分配失败时 sz[i] 仍是确定的 0"成立, 也让后面的校验循环
     * 不必依赖"编译器看得懂 blk[i] 与 sz[i] 是同一次迭代一起赋值的"这种推理 */
    for (br_u32 i = 0u; i < (br_u32)CONF_BLOCKS; i++) {
        blk[i] = BR_NULL;
        sz[i]  = 0u;
    }

    for (br_u32 i = 0u; i < (br_u32)CONF_BLOCKS; i++) {
        sz[i]  = 32u + ((br_size_t)(i % 7u) * 16u);
        blk[i] = br_malloc(sz[i]);

        if (blk[i] == BR_NULL) {
            bad++;
            continue;
        }
        br_u8 *p = (br_u8 *)blk[i];
        for (br_size_t k = 0u; k < sz[i]; k++) {
            p[k] = (br_u8)(i + (br_u32)k);
        }
    }

    /* 交错释放一半(偶数号), 校验存活块内容 —— 这就是"碎片压力"的形态 */
    for (br_u32 i = 0u; i < (br_u32)CONF_BLOCKS; i += 2u) {
        if (blk[i] == BR_NULL) {
            continue;
        }
        const br_u8 *p = (const br_u8 *)blk[i];
        for (br_size_t k = 0u; k < sz[i]; k++) {
            if (p[k] != (br_u8)(i + (br_u32)k)) {
                bad++;
            }
        }
        br_free(blk[i]);
        blk[i] = BR_NULL;
    }
    for (br_u32 i = 1u; i < (br_u32)CONF_BLOCKS; i += 2u) {
        if (blk[i] == BR_NULL) {
            continue;
        }
        const br_u8 *p = (const br_u8 *)blk[i];
        for (br_size_t k = 0u; k < sz[i]; k++) {
            if (p[k] != (br_u8)(i + (br_u32)k)) {
                bad++;
            }
        }
    }

    const br_u32 dirty = br_heap_check();

    br_heap_stats_t st;
    const int rs = br_heap_stats_get(&st);
    const br_bool frag_ok = (rs == 0)
                            && (st.max_free_block >= (st.total / 2u));

    br_log_info("[MEMCONF] info heap after pressure: check=%u used=%lu total=%lu max_free=%lu",
                dirty, (br_u64)st.used, (br_u64)st.total, (br_u64)st.max_free_block);

    conf_report(bad == 0u && dirty == 0u && rs == 0, "TC-MEM-002",
                "128 次交错 alloc/free 后内容完整且 br_heap_check()==0");
    conf_report(frag_ok, "TC-MEM-002",
                "最大空闲块 >= 池的 50%(TLSF 有界碎片, 6-01 §3.5)");

    /* 收尾: 释放剩下的一半, 不泄漏 */
    for (br_u32 i = 1u; i < (br_u32)CONF_BLOCKS; i += 2u) {
        if (blk[i] != BR_NULL) {
            br_free(blk[i]);
        }
    }
}

static void conf_tc_mem_003(void)
{
    br_u8 *c = (br_u8 *)br_calloc(16u, 4u);     /* 64 B */
    br_bool zeroed = (c != BR_NULL) ? BR_TRUE : BR_FALSE;

    if (c != BR_NULL) {
        for (br_size_t i = 0u; i < 64u; i++) {
            if (c[i] != 0u) {
                zeroed = BR_FALSE;
            }
        }
        for (br_size_t i = 0u; i < 8u; i++) {
            c[i] = (br_u8)(0xA0u + (br_u32)i);
        }
    }

    br_u8 *c2 = (c != BR_NULL) ? (br_u8 *)br_realloc(c, 128u) : BR_NULL;
    br_bool kept = (c2 != BR_NULL) ? BR_TRUE : BR_FALSE;

    if (c2 != BR_NULL) {
        for (br_size_t i = 0u; i < 8u; i++) {
            if (c2[i] != (br_u8)(0xA0u + (br_u32)i)) {
                kept = BR_FALSE;
            }
        }
    }

    br_free(c2);
    conf_report(zeroed, "TC-MEM-003", "calloc(16,4) 返回的 64 B 全零");
    conf_report(kept, "TC-MEM-003", "realloc 到 128 B 后前缀 8 B 内容保留");
}

static void conf_tc_mem_004(void)
{
    void *p = BR_NULL;
    void *q = BR_NULL;

    const int r1 = br_mem_alloc_contig(8192u, BR_PAGE_SIZE, &p);
    br_bool   ok = (r1 == 0) && (p != BR_NULL)
                   && (((br_uintptr_t)p % BR_PAGE_SIZE) == 0u);

    if (r1 == 0) {
        br_mem_free_contig(p, 8192u);
    }

    const int r2 = br_mem_alloc_contig(8192u, BR_PAGE_SIZE, &q);
    ok = ok && (r2 == 0) && (q != BR_NULL);

    if (r2 == 0) {
        br_mem_free_contig(q, 8192u);
    }

    br_log_info("[MEMCONF] info contig[0]=%p contig[1]=%p", p, q);
    conf_report(ok, "TC-MEM-004", "contig align=4096 分配页对齐, free 后可复分配(CA-7)");
}

static void conf_tc_mem_005(void)
{
    const br_size_t n = 3u;
    void     *p = br_page_alloc(n, BR_PAGE_F_ZERO);
    br_bool   ok = (p != BR_NULL) && (((br_uintptr_t)p % BR_PAGE_SIZE) == 0u);

    br_mem_layout_t lay;
    if (ok && br_mem_layout(&lay) == 0) {
        /* "连续"在本原型 = 一次分配的一段地址全落在页池内且逐页相邻 */
        ok = ((br_uintptr_t)p >= lay.page.base)
             && (((br_uintptr_t)p + (n * BR_PAGE_SIZE)) <= (lay.page.base + lay.page.size));
    }

    if (p != BR_NULL) {
        br_page_free(p, n);
    }

    void *q = br_page_alloc(n, 0u);
    ok = ok && (q != BR_NULL);
    if (q != BR_NULL) {
        br_page_free(q, n);
    }

    br_log_info("[MEMCONF] info page_alloc(3)=%p refill=%p", p, q);
    conf_report(ok, "TC-MEM-005", "page alloc(3 页) 页对齐且连续, free 后可复分配(CA-8)");
}

static void conf_tc_mem_006(void)
{
    br_dma_buf_t b;

    b.vaddr    = BR_NULL;
    b.dma_addr = 0u;
    b.size     = 0u;

    const int r = br_dma_alloc(256u, BR_DMA_F_ALIGN4K, &b);
    const br_bool ok = (r == 0) && (b.vaddr != BR_NULL)
                       && (b.dma_addr == (br_uintptr_t)b.vaddr)
                       && (b.size >= 256u);

    br_log_info("[MEMCONF] info dma_alloc vaddr=%p dma_addr=0x%lx size=%lu",
                b.vaddr, (br_u64)b.dma_addr, (br_u64)b.size);
    if (r == 0) {
        (void)br_dma_free(&b);
    }
    conf_report(ok, "TC-MEM-006", "dma_alloc: dma_addr == vaddr(v1 恒等, CA-6/INV-6)");
}

static void conf_tc_mem_007(void)
{
    br_size_t u0 = 0u, t0 = 0u, u1 = 0u, t1 = 0u, u2 = 0u, t2 = 0u;

    const int r0 = br_heap_usage(&u0, &t0);
    void *p = br_malloc(4096u);
    const int r1 = br_heap_usage(&u1, &t1);
    br_free(p);
    const int r2 = br_heap_usage(&u2, &t2);

    br_log_info("[MEMCONF] info heap_usage before=(%lu,%lu) alloc=(%lu,%lu) free=(%lu,%lu)",
                (br_u64)u0, (br_u64)t0, (br_u64)u1, (br_u64)t1, (br_u64)u2, (br_u64)t2);

    conf_report(r0 == 0 && r1 == 0 && r2 == 0 && p != BR_NULL && u1 > u0 && t1 == t0,
                "TC-MEM-007", "allocate 后 used 上升(total 不变)");
    conf_report(u2 == u0 && t2 == t0, "TC-MEM-007", "free 后 used/total 回基线(记账增减匹配)");
}

/* =====================================================================
 * TC-MM-001..003(6-01 §3.6)
 * ===================================================================== */
static void conf_tc_mm_001(void)
{
    const br_mm_region_t *img = conf_region_of_kind(BR_MM_KIND_IMAGE);

    int r_dup = BR_ERR(BR_EINVAL);
    if (img != BR_NULL) {
        r_dup = br_mm_region_add(img);      /* 同 base/size 复用: 表里已有 ⇒ -EEXIST */
    }
    conf_report(img != BR_NULL && r_dup == BR_ERR(BR_EEXIST), "TC-MM-001",
                "复用已存在的 region(同 base/size)⇒ -EEXIST");

    br_mm_region_t r;
    r.base  = BR_PLAT_CONF_UNMAPPED + 1u;   /* 未声明区 + 非 4 KiB 对齐 */
    r.size  = BR_PAGE_SIZE;
    r.attrs = conf_attrs_kind(BR_MM_CACHED, BR_MM_KIND_RESERVED);

    const int rc = br_mm_region_add(&r);
    br_log_info("[MEMCONF] info region_add(base=0x%lx unaligned) rc=%d", (br_u64)r.base, rc);
    conf_report(rc == BR_ERR(BR_EINVAL), "TC-MM-001", "base 非 4 KiB 对齐 ⇒ -EINVAL");
}

static void conf_tc_mm_002(void)
{
    /*
     * PL011 UART0 = 已声明的 MMIO(Device, 非 CACHED)。对非缓存区做 cache 维护
     * **必须是安静的无操作**: 真去 `dc cvac` 一个设备寄存器地址在架构上是不可预测的。
     * core 按 region 属性短路(见 br_mm.h 的语义), 所以这里判的是"core 真的短路了"。
     */
    const int rc = br_mm_cache_flush((void *)(br_uintptr_t)0x09000000ul, 64u);

    conf_report(rc == 0, "TC-MM-002",
                "cache_flush(PL011 MMIO, 64 B) ⇒ 0(DEVICE 非缓存区无操作)");
}

static void conf_tc_mm_003(void)
{
    const br_mm_region_t *res = conf_region_of_kind(BR_MM_KIND_RESERVED);

    int r_map = BR_ERR(BR_EINVAL);
    if (res != BR_NULL) {
        r_map = br_mm_map(res);
    }
    conf_report(r_map == BR_ERR(BR_ENOTSUP), "TC-MM-003",
                "br_mm_map() 在 v1 ⇒ -ENOTSUP(签名先行, 实现归 v2 重定位)");

    const br_uintptr_t addr = (res != BR_NULL) ? res->base : (br_uintptr_t)BR_PLAT_CONF_UNMAPPED;
    const int r_unmap = br_mm_unmap(addr, BR_PAGE_SIZE);
    conf_report(r_unmap == BR_ERR(BR_ENOTSUP), "TC-MM-003",
                "br_mm_unmap() 在 v1 ⇒ -ENOTSUP(主文档风险 R4 的签名先行验证)");
}

/* =====================================================================
 * 入口(由 platform 的 `qemu_aarch64_start()` 在 START 相调用; 见 br_plat.h)
 * ===================================================================== */
int br_plat_mem_conformance(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[MEMCONF] memory mapping conformance (identity 4 KiB pages + region attrs)");

    conf_mm_active();
    conf_mm_ident();
    conf_mm_pools();
    conf_mm_region();
    conf_mm_ro();
    conf_mm_nxdesc();
    conf_mm_devblk();
    conf_mm_unmap();

    conf_tc_mem_001();
    conf_tc_mem_002();
    conf_tc_mem_003();
    conf_tc_mem_004();
    conf_tc_mem_005();
    conf_tc_mem_006();
    conf_tc_mem_007();

    conf_tc_mm_001();
    conf_tc_mm_002();
    conf_tc_mm_003();

    br_log_info("[MEMCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
