/*
 * Memory-mapping conformance cases (in-image: the MMU group is target-only - identity mapping,
 * protection pages and translation faults do not exist on the host).
 * br_plat_mem_conformance() runs them at startup, printing "[MEMCONF] PASS/FAIL <tag> <text>" per
 * item and a final "[MEMCONF] SUMMARY pass=N fail=0 total=N" line, so red/green is decided by grep.
 * Coverage: MM-ACTIVE/IDENT/POOLS/REGION/RO/NXDESC/DEVBLK/UNMAP, TC-MEM-001..007, TC-MM-001..003.
 */
#include <br/core/br_error.h>
#include <br/hal/br_fault.h>
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>
#include <br/hal/br_mm.h>

#include <br/runtime/br_mmu.h>
#include <br/runtime/br_plat.h>

/* ---- assertions and counters (failure count is the return value) ---- */
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

/* extable probe. The return value comes solely from x0, written by core from the table entry's
 * reg/errno_val; the success path sets it to zero itself. The fixup label must not create an error
 * code, so the probe can verify "a table entry carries reg + errno_val". */
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

/* Write probe: MM-RO must see a write on a read-only page take a permission fault, which a read
 * probe cannot. Same table-entry shape as the read probe (reg=0, errno=-14, fixup label). */
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

/* ---- small helpers ---- */
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

/* Kind must go through BR_MM_WITH_KIND: a direct `| BR_MM_KIND_x` sets 8 in the low nibble. */
static br_u32 conf_attrs_kind(br_u32 attrs, br_u32 kind)
{
    return BR_MM_WITH_KIND(attrs, kind);
}

/* ---- MM-ACTIVE / MM-IDENT ---- */
static br_u32 s_marker = 0x1D3E7u;      /* in-image variable (one MM-IDENT sample point) */

static void conf_mm_active(void)
{
    br_plat_mmu_info_t info;

    br_plat_mmu_info(&info);

    const br_bool m_bit = ((info.sctlr & 1ull) != 0ull) ? BR_TRUE : BR_FALSE;
    br_log_info("[MEMCONF] info SCTLR_EL1=0x%lx M=%u TCR_EL1=0x%lx MAIR_EL1=0x%lx TTBR0_EL1=0x%lx",
                info.sctlr, (br_u32)m_bit, info.tcr, info.mair, info.ttbr0);

    conf_report(br_mm_active() && m_bit, "MM-ACTIVE",
                "br_mm_active() true and SCTLR_EL1.M (bit0)==1 (MMU is running)");
}

static void conf_mm_ident(void)
{
    br_mem_layout_t lay;
    volatile br_u32 stack_local = 0x5A5A5A5Au;
    br_uintptr_t    addrs[8];
    br_u32          bad = 0u;

    if (br_mem_layout(&lay) != 0) {
        conf_report(BR_FALSE, "MM-IDENT", "pool layout lookup failed, cannot sample pool addresses");
        return;
    }

    const br_mm_region_t *res = conf_region_of_kind(BR_MM_KIND_RESERVED);
    if (res == BR_NULL) {
        conf_report(BR_FALSE, "MM-IDENT", "no RESERVED region in the region table");
        return;
    }

    /* 8 sample points: image var / stack local / heap ends / page ends / contig / reserved */
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
                "8 sampled addresses (image/stack/heap ends/page pool ends/contig/reserved): mapped and page-table PA == VA frame");
}

/* ---- MM-POOLS ---- */
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
        conf_report(BR_FALSE, "MM-POOLS", "br_mem_layout() returned non-zero (pools not ready)");
        return;
    }

    pools[0] = &lay.heap;
    pools[1] = &lay.contig;
    pools[2] = &lay.page;
    pools[3] = &lay.dma;

    /* Each pool's base/size matches the region table entry by entry: pools come from claiming a
     * region, so there must not be a second geometry. */
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
                "all four pools' base/size match the region table entry by entry (pool geometry = declaration, no second truth)");

    /* (b) page pool math: page_total pages x 4 KiB == page pool byte size */
    conf_report(lay.page_size == BR_PAGE_SIZE
                && (br_size_t)lay.page_total * BR_PAGE_SIZE == lay.page.size,
                "MM-POOLS",
                "page_size==4096 and page_total*4096 == page pool size");

    /* (c) the four pools are pairwise non-overlapping */
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
    conf_report(overlap == 0u, "MM-POOLS", "all four pools are pairwise non-overlapping");

    /* (d) all inside the 8 MiB page-table window, else the first allocation would fault */
    conf_report(outside == 0u, "MM-POOLS", "all four pools fall inside the 8 MiB RAM mapping window");
}

/* ---- MM-REGION ---- */
static void conf_mm_region(void)
{
    const br_u32 n = br_mm_region_count();
    br_u32       bad_kind = 0u;

    br_log_info("[MEMCONF] info region table: %u entries (max %u)", n, (br_u32)BR_MM_MAX_REGIONS);
    conf_report(n > 0u && n <= BR_MM_MAX_REGIONS, "MM-REGION",
                "br_mm_region_count() > 0 and within table capacity");

    for (br_u32 i = 0u; i < n; i++) {
        const br_mm_region_t *r = br_mm_region_get(i);

        if (r == BR_NULL || !conf_kind_known(BR_MM_KIND(r->attrs))) {
            bad_kind++;
            continue;
        }
        br_log_info("[MEMCONF] info   [%u] base=0x%lx size=%lu attrs=0x%x kind=%u",
                    i, (br_u64)r->base, (br_u64)r->size, r->attrs, BR_MM_KIND(r->attrs));
    }
    conf_report(bad_kind == 0u, "MM-REGION", "every region's kind is recognizable (image..reserved)");

    const br_mm_region_t *img = conf_region_of_kind(BR_MM_KIND_IMAGE);
    const br_mm_region_t *hit = (img != BR_NULL) ? br_mm_region_find(img->base) : BR_NULL;

    conf_report(img != BR_NULL && hit != BR_NULL
                && hit->base == img->base && hit->size == img->size,
                "MM-REGION", "region_find(image base) hits and base/size match the declaration");

    conf_report(br_mm_region_find((br_uintptr_t)BR_PLAT_CONF_UNMAPPED) == BR_NULL,
                "MM-REGION", "undeclared address (0x42000000, outside the window) => region_find == NULL");
}

/* ---- MM-RO / MM-NXDESC / MM-UNMAP (real protection and recoverable faults) ---- */
static void conf_mm_ro(void)
{
    const br_mm_region_t *res = conf_region_of_kind(BR_MM_KIND_RESERVED);

    if (res == BR_NULL || res->size < (2u * BR_PAGE_SIZE)) {
        conf_report(BR_FALSE, "MM-RO", "reserved region missing or smaller than two pages");
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
                "set_attrs(RO) succeeds and query reads BR_MM_RO back from the **page table**");

    /* Real protection: the write probe must hit the fixup (permission fault). */
    const int wr = conf_probe_write32(page, 0xDEADBEEFu);
    conf_report(wr == BR_ERR(EFAULT), "MM-RO",
                "write on a read-only page takes permission fault => extable fixes up to -EFAULT (real protection, not a shadow)");

    /* Attrs are revocable: after setting back to RW, writes must work - otherwise "protection"
     * would be indistinguishable from "brick". */
    const int rs2 = br_mm_set_attrs(page, BR_PAGE_SIZE,
                                    conf_attrs_kind(BR_MM_CACHED, BR_MM_KIND_RESERVED));
    const int wr2 = conf_probe_write32(page, 0x5A5A5A5Au);
    br_u32    back = 0u;
    const int rd = conf_probe_read32(page, &back);

    conf_report(rs2 == 0 && wr2 == 0 && rd == 0 && back == 0x5A5A5A5Au, "MM-RO",
                "after restoring RW, write then read-back works (protection is revocable; the page is not left read-only for later cases)");
}

static void conf_mm_nxdesc(void)
{
    const br_mm_region_t *res = conf_region_of_kind(BR_MM_KIND_RESERVED);

    if (res == BR_NULL || res->size < (2u * BR_PAGE_SIZE)) {
        conf_report(BR_FALSE, "MM-NXDESC", "reserved region missing or smaller than two pages");
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
                "after set_attrs(NX), query reads back BR_MM_NX (PXN|UXN both set)");

    /*
     * Deliberately no real jump to this page: execute-on-NX takes an instruction abort, while
     * core's extable covers only data abort, so it would be a fatal panic. NX verification stops at
     * "the descriptor bit takes effect"; that is this prototype's capability boundary.
     */
    const int rs2 = br_mm_set_attrs(page, BR_PAGE_SIZE,
                                    conf_attrs_kind(BR_MM_CACHED, BR_MM_KIND_RESERVED));
    conf_report(rs2 == 0, "MM-NXDESC", "restore executable (RW non-NX), do not leak NX to later cases");
}

/*
 * set_attrs boundary on a Device 2 MiB block: if sub-block requests were "conveniently relaxed" to
 * per-2 MiB, protecting 4 KiB would turn the whole peripheral window read-only, and the symptom
 * would show up on the next ISR register access, far from the root cause.
 */
static void conf_mm_devblk(void)
{
    /* (1) sub-block range (PL011 frame is 4 KiB, inside one 2 MiB block) => -ENOTSUP */
    const int r_sub = br_mm_set_attrs(0x09000000ul, BR_PAGE_SIZE, BR_MM_DEVICE | BR_MM_RO);

    /* (2) a whole-block-aligned range can be changed as a block. 0x0E000000 is unused on QEMU virt
     *    (neither GIC/GICR nor UART/virtio), so no live register frame is made read-only. */
    const br_uintptr_t blk = 0x0E000000ul;
    br_u32 attrs = 0u;
    const int r_w = br_mm_set_attrs(blk, 0x200000ul, BR_MM_DEVICE | BR_MM_RO);
    const int r_q = br_mm_query(blk, BR_PAGE_SIZE, &attrs);
    const int r_r = br_mm_set_attrs(blk, 0x200000ul, BR_MM_DEVICE);

    br_u32 back = 0u;
    const int r_b = br_mm_query(blk, BR_PAGE_SIZE, &back);

    br_log_info("[MEMCONF] info devblk sub=%d whole=%d query=%d attrs=0x%x restore=%d back=0x%x",
                r_sub, r_w, r_q, attrs, r_r, back);
    conf_report(r_sub == BR_ERR(ENOTSUP), "MM-DEVBLK",
                "sub-block set_attrs (4 KiB) on a Device 2 MiB block => -ENOTSUP (do not pretend to support what the hardware cannot express)");
    conf_report(r_w == 0 && r_q == 0 && (attrs & BR_MM_RO) != 0u && (attrs & BR_MM_DEVICE) != 0u
                && r_r == 0 && r_b == 0 && (back & BR_MM_RO) == 0u,
                "MM-DEVBLK", "a whole-block-aligned Device 2 MiB range can be made RO as a block and read back, then restored to RW");
}

static void conf_mm_unmap(void)
{
    br_uintptr_t pa = 0u;
    br_u32       v  = 0u;

    /* Confirm it is really unmapped first, then that accessing it takes a recoverable fault. */
    const int rt = br_plat_mmu_translate((br_uintptr_t)BR_PLAT_CONF_UNMAPPED, &pa);
    br_log_info("[MEMCONF] info unmap probe va=0x%lx translate=%d", (br_u64)BR_PLAT_CONF_UNMAPPED, rt);
    conf_report(rt == BR_ERR(EFAULT), "MM-UNMAP",
                "address outside the window (0x42000000) is invalid in the page table => translation fails (-EFAULT)");

    const int r1 = conf_probe_read32((br_uintptr_t)BR_PLAT_CONF_UNMAPPED, &v);
    conf_report(r1 == BR_ERR(EFAULT), "MM-UNMAP",
                "read of an unmapped address => translation fault fixed up by extable to -EFAULT, system survives");

    /* Second time: if in_fault were not reset, this would be a double fault (bare panic) */
    const int r2 = conf_probe_read32((br_uintptr_t)BR_PLAT_CONF_UNMAPPED, &v);
    conf_report(r2 == BR_ERR(EFAULT), "MM-UNMAP",
                "reading again is still -EFAULT (in_fault was reset => faults are recoverable)");
}

/* ---- TC-MEM-001..007 ---- */
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
    conf_report(ok, "TC-MEM-001", "malloc returns 16 B aligned addresses (>= BR_MALLOC_ALIGN)");
}

static void conf_tc_mem_002(void)
{
    enum { CONF_BLOCKS = 128 };
    void     *blk[CONF_BLOCKS];
    br_size_t sz[CONF_BLOCKS];
    br_u32    bad = 0u;

    /* Explicit zeroing keeps sz[i] a definite 0 when an allocation fails. */
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

    /* Free even-index blocks and verify the survivors: fragmentation pressure. */
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
                "after 128 interleaved alloc/free, contents intact and br_heap_check()==0");
    conf_report(frag_ok, "TC-MEM-002",
                "largest free block >= 50% of the pool (TLSF bounded fragmentation, 6-01 §3.5)");

    /* Cleanup: free the other half, no leak. */
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
    conf_report(zeroed, "TC-MEM-003", "calloc(16,4) returns 64 B of all zeros");
    conf_report(kept, "TC-MEM-003", "after realloc to 128 B the first 8 B of content are preserved");
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
    conf_report(ok, "TC-MEM-004", "contig align=4096 allocations are page-aligned and re-allocatable after free (CA-7)");
}

static void conf_tc_mem_005(void)
{
    const br_size_t n = 3u;
    void     *p = br_page_alloc(n, BR_PAGE_F_ZERO);
    br_bool   ok = (p != BR_NULL) && (((br_uintptr_t)p % BR_PAGE_SIZE) == 0u);

    br_mem_layout_t lay;
    if (ok && br_mem_layout(&lay) == 0) {
        /* "Contiguous" here = the range lies entirely inside the page pool, page-adjacent. */
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
    conf_report(ok, "TC-MEM-005", "page alloc (3 pages) is page-aligned and contiguous, re-allocatable after free (CA-8)");
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
    conf_report(ok, "TC-MEM-006", "dma_alloc: dma_addr == vaddr (v1 is identity, CA-6/INV-6)");
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
                "TC-MEM-007", "after allocate, used rises (total unchanged)");
    conf_report(u2 == u0 && t2 == t0, "TC-MEM-007", "after free, used/total return to baseline (accounting changes match)");
}

/* ---- TC-MM-001..003 ---- */
static void conf_tc_mm_001(void)
{
    const br_mm_region_t *img = conf_region_of_kind(BR_MM_KIND_IMAGE);

    int r_dup = BR_ERR(EINVAL);
    if (img != BR_NULL) {
        r_dup = br_mm_region_add(img);      /* reused base/size: already in the table => -EEXIST */
    }
    conf_report(img != BR_NULL && r_dup == BR_ERR(EEXIST), "TC-MM-001",
                "re-adding an existing region (same base/size) => -EEXIST");

    br_mm_region_t r;
    r.base  = BR_PLAT_CONF_UNMAPPED + 1u;   /* undeclared area + not 4 KiB aligned */
    r.size  = BR_PAGE_SIZE;
    r.attrs = conf_attrs_kind(BR_MM_CACHED, BR_MM_KIND_RESERVED);

    const int rc = br_mm_region_add(&r);
    br_log_info("[MEMCONF] info region_add(base=0x%lx unaligned) rc=%d", (br_u64)r.base, rc);
    conf_report(rc == BR_ERR(EINVAL), "TC-MM-001", "base not 4 KiB aligned => -EINVAL");
}

static void conf_tc_mm_002(void)
{
    /*
     * PL011 UART0 is declared Device/non-CACHED; cache maintenance on a non-cached region must be
     * a silent no-op (dc cvac on a device register address is architecturally unpredictable). core
     * short-circuits per region attribute, so this judges that core really short-circuited.
     */
    const int rc = br_mm_cache_flush((void *)(br_uintptr_t)0x09000000ul, 64u);

    conf_report(rc == 0, "TC-MM-002",
                "cache_flush(PL011 MMIO, 64 B) => 0 (no-op on a DEVICE non-cached region)");
}

static void conf_tc_mm_003(void)
{
    const br_mm_region_t *res = conf_region_of_kind(BR_MM_KIND_RESERVED);

    int r_map = BR_ERR(EINVAL);
    if (res != BR_NULL) {
        r_map = br_mm_map(res);
    }
    conf_report(r_map == BR_ERR(ENOTSUP), "TC-MM-003",
                "br_mm_map() in v1 => -ENOTSUP (signature first, implementation belongs to v2 relocation)");

    const br_uintptr_t addr = (res != BR_NULL) ? res->base : (br_uintptr_t)BR_PLAT_CONF_UNMAPPED;
    const int r_unmap = br_mm_unmap(addr, BR_PAGE_SIZE);
    conf_report(r_unmap == BR_ERR(ENOTSUP), "TC-MM-003",
                "br_mm_unmap() in v1 => -ENOTSUP (signature-first verification of main-doc risk R4)");
}

/* ---- entry point (called by qemu_aarch64_start() in the START phase) ---- */
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
