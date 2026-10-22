/*
 * QEMU virt (aarch64) page-table construction (identity map + region attributes).
 * Mechanical path only: policy (which RAM is which pool / which span is MMIO) is the region table
 * in src/memmap.c, and the registration state machine is core's br_mm_activate().
 * WORKAROUND(br-wa-isa-001): the design assigns page-table construction to an ISA shared library;
 * this prototype has none, so it sits in the platform plugin, with layering kept by file boundary.
 * Page tables live in .bss, so the identity map must cover the tables themselves.
 */
#include <br/hal/br_mm.h>
#include <br/core/br_error.h>
#include <br/core/br_log.h>

#include <br/runtime/br_mmu.h>

/* ---- constants and descriptor encoding ---- */

/* Entries per table level; 4 KiB granularity; 2 MiB L2 block; 8 MiB / 2 MiB = 4 L3 tables;
 * aarch64 minimum cache line (cache maintenance granularity). */
#define MMU_ENTRIES          512u
#define MMU_GRANULE          4096u
#define MMU_L2_BLOCK_SIZE    0x00200000u
#define MMU_RAM_L3           4u
#define MMU_CACHE_LINE       64u

/* Descriptor encoding (4 KiB granularity, 39-bit VA => walk starts at L1):
 *   table descriptor: next_table_pa | 0b11;  L2 block (2 MiB): pa | 0b01 | attrs;
 *   L3 page (4 KiB): pa | 0b11 | attrs;  AF(bit10) = 1.
 *   Normal: SH=0b11 (inner shareable), AttrIndx=0 (MAIR Attr0);  Device: SH=0b00, AttrIndx=1.
 *   AP[2:1](bits 7:6): 0b00 = RW, 0b10 = RO;  NX: PXN(bit53) = 1 and UXN(bit54) = 1. */
#define DESC_TYPE_MASK       0x3ull
#define DESC_TABLE          0x3ull
#define DESC_BLOCK          0x1ull
#define DESC_PAGE           0x3ull
#define DESC_AF             (1ull << 10)
#define DESC_SH_SHIFT       8u
#define DESC_SH_INNER       0x3ull
#define DESC_SH_NONE        0x0ull
#define DESC_AP_SHIFT       6u
#define DESC_AP_RO          (0x2ull << DESC_AP_SHIFT)
#define DESC_ATTRIDX_SHIFT  2u
#define DESC_ATTRIDX_NORMAL 0ull
#define DESC_ATTRIDX_DEVICE 1ull
#define DESC_PXN            (1ull << 53)
#define DESC_UXN            (1ull << 54)

/* Attribute bits changed by set_attrs; base and type bits are preserved. */
#define DESC_ATTR_MASK      (DESC_AF | (0x3ull << DESC_SH_SHIFT) | (0x3ull << DESC_AP_SHIFT) \
                             | (0x7ull << DESC_ATTRIDX_SHIFT) | DESC_PXN | DESC_UXN)

/* Output address masks: bits 47:12 (page) and 47:21 (2 MiB block). */
#define MM_ADDR_MASK        0x0000FFFFFFFFF000ull
#define MM_BLOCK_MASK_L2    0x0000FFFFFFE00000ull

/* MAIR_EL1: Attr0 = 0xFF (Normal WB RA/WA), Attr1 = 0x00 (Device-nGnRnE), Attr2 = 0x44 (Normal
 * Non-cacheable, spare; never emitted as an AttrIndx here). */
#define MMU_MAIR_VALUE      0x004400FFull

/* TCR_EL1: T0SZ=25 (39-bit VA, walk starts at L1); IRGN0=0b01 (walks inner WB WA); ORGN0=0b11
 * (walks outer Non-cacheable); SH0=0b11 (walks inner shareable, kept for future SMP); TG0=0b00
 * (4 KiB); EPD1=1 (TTBR0 only, addresses with bit63=1 fault); IPS=0b010 (40-bit PA). */
#define MMU_TCR_VALUE       ((25ull) | (1ull << 8) | (3ull << 10) | (3ull << 12) \
                             | (1ull << 23) | (2ull << 32))

/* SCTLR_EL1 RES1 bits (11/20/22/23/28/29) must be 1; never assume the reset value. */
#define MMU_SCTLR_RES1      ((1ull << 11) | (1ull << 20) | (1ull << 22) | (1ull << 23) \
                             | (1ull << 29) | (1ull << 28))
#define MMU_SCTLR_M         (1ull << 0)
#define MMU_SCTLR_C         (1ull << 2)
#define MMU_SCTLR_I         (1ull << 12)

/*
 * Clear the alignment-check switches A(bit1, data access), SA(bit3) and SA0(bit4): Normal memory
 * must allow unaligned accesses. Note ARMv8 field numbers differ from ARMv7/AArch32 (where SA/SA0
 * are bits 4/5); bit5 here is CP15BEN, unrelated, left untouched.
 */
#define MMU_SCTLR_A         (1ull << 1)
#define MMU_SCTLR_SA        (1ull << 3)
#define MMU_SCTLR_SA0       (1ull << 4)

/* ---- page-table memory ----
 * Static arrays: 1 L1 + 1 Device L2 + 1 RAM L2 + 4 L3 = 7 tables = 28 KiB. Static, not from the
 * page pool: the pool needs active mappings, which these tables provide. start.S's BSS clear
 * incidentally marks every entry invalid. */
static br_u64 s_l1[MMU_ENTRIES]                 __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l2_dev[MMU_ENTRIES]             __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l2_ram[MMU_ENTRIES]             __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l3_ram[MMU_RAM_L3][MMU_ENTRIES] __attribute__((aligned(MMU_GRANULE)));

/* activate may succeed only once (repeat => -EBUSY). */
static br_bool s_activated;

_Static_assert(MMU_RAM_L3 * MMU_L2_BLOCK_SIZE == BR_PLAT_RAM_WINDOW_SIZE,
               "L3 table count does not match the RAM mapping window");

/* ---- descriptor and system-register helpers ---- */

/* Region attrs -> descriptor bits. DEVICE wins over CACHED: both set is contradictory, and cached
 * peripheral registers would be far worse than slow Device accesses to RAM. */
static br_u64 mmu_desc_attrs(br_u32 attrs)
{
    br_u64 d = DESC_AF;

    if ((attrs & BR_MM_DEVICE) != 0u) {
        d |= (DESC_SH_NONE << DESC_SH_SHIFT) | (DESC_ATTRIDX_DEVICE << DESC_ATTRIDX_SHIFT);
    } else {
        d |= (DESC_SH_INNER << DESC_SH_SHIFT) | (DESC_ATTRIDX_NORMAL << DESC_ATTRIDX_SHIFT);
    }

    if ((attrs & BR_MM_RO) != 0u) {
        d |= DESC_AP_RO;
    }
    if ((attrs & BR_MM_NX) != 0u) {
        d |= DESC_PXN | DESC_UXN;
    }
    return d;
}

/* Descriptor bits -> region attrs; does not synthesize the region-table kind. Kind is a declaration
 * fact answered by br_mm_region_*, while query reports the attrs in effect in the page table. */
static br_u32 mmu_attrs_from_desc(br_u64 d)
{
    br_u32 a = 0u;

    if ((d & DESC_AF) == 0ull) {
        return 0u;      /* AF=0 descriptor is unusable; mmu_query treats it as unmapped */
    }

    if (((d >> DESC_ATTRIDX_SHIFT) & 0x7ull) == DESC_ATTRIDX_DEVICE) {
        a |= BR_MM_DEVICE;
    } else {
        a |= BR_MM_CACHED;      /* Attr0 = Normal WB; Attr2 (spare) also counts as "non-Device" */
    }
    if ((d & DESC_AP_RO) != 0ull) {
        a |= BR_MM_RO;
    }
    if ((d & (DESC_PXN | DESC_UXN)) != 0ull) {
        a |= BR_MM_NX;
    }
    return a;
}

/* Page-table walk: writable pointer to the last-level descriptor, *out_level = 2 (2 MiB block) or
 * 3 (4 KiB page); NULL if unmapped/invalid. Identity-map pointer arithmetic: the PA in a descriptor
 * is directly dereferenceable. */
static br_u64 *mmu_desc_ptr(br_uintptr_t va, br_u32 *out_level)
{
    const br_uintptr_t l1_index = (va >> 30) & 0x1FFull;
    const br_uintptr_t l2_index = (va >> 21) & 0x1FFull;
    const br_uintptr_t l3_index = (va >> 12) & 0x1FFull;

    br_u64 *tbl = s_l1;
    br_u64  d   = tbl[l1_index];

    if ((d & DESC_TYPE_MASK) != DESC_TABLE) {
        return BR_NULL;
    }

    tbl = (br_u64 *)(br_uintptr_t)(d & MM_ADDR_MASK);
    d   = tbl[l2_index];
    if ((d & DESC_TYPE_MASK) == DESC_BLOCK) {
        *out_level = 2u;
        return &tbl[l2_index];
    }
    if ((d & DESC_TYPE_MASK) != DESC_TABLE) {
        return BR_NULL;
    }

    tbl = (br_u64 *)(br_uintptr_t)(d & MM_ADDR_MASK);
    d   = tbl[l3_index];
    if ((d & DESC_TYPE_MASK) != DESC_PAGE) {
        return BR_NULL;
    }
    *out_level = 3u;
    return &tbl[l3_index];
}

/* Descriptor -> physical address (frame base, no in-page offset). */
static br_uintptr_t mmu_desc_pa(br_uintptr_t va, br_u64 d, br_u32 level)
{
    if (level == 2u) {
        return (va & ~((br_uintptr_t)MMU_L2_BLOCK_SIZE - 1ull)) | (br_uintptr_t)(d & MM_BLOCK_MASK_L2);
    }
    return (va & ~((br_uintptr_t)MMU_GRANULE - 1ull)) | (br_uintptr_t)(d & MM_ADDR_MASK);
}

/* After page tables change the TLB must see it; vmalle1 is the coarsest but never-wrong
 * granularity (per-entry vaae1 is an optimization, not correctness). */
static void mmu_tlb_flush_all(void)
{
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static br_u64 mmu_read_sctlr(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    return v;
}

static void mmu_write_sctlr(br_u64 v)
{
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(v) : "memory");
}

static br_u64 mmu_read_tcr(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(v));
    return v;
}

static br_u64 mmu_read_mair(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(v));
    return v;
}

static br_u64 mmu_read_ttbr0(void)
{
    br_u64 v;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v));
    return v;
}

/* ---- table construction (internal to activate) ---- */

/* RAM window background: 4 L3 tables mapping every page Normal WB RW executable. Pages not covered
 * by a declaration stay mapped, so undeclared local objects never fault mysteriously. */
static void mmu_fill_ram_window(void)
{
    const br_u64 normal = mmu_desc_attrs(BR_MM_CACHED);

    for (br_u32 t = 0; t < MMU_RAM_L3; t++) {
        s_l2_ram[t] = (br_u64)(br_uintptr_t)&s_l3_ram[t][0] | DESC_TABLE;

        for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
            const br_uintptr_t pa = (br_uintptr_t)BR_PLAT_RAM_BASE
                                    + ((br_uintptr_t)t * MMU_L2_BLOCK_SIZE)
                                    + ((br_uintptr_t)i * MMU_GRANULE);
            s_l3_ram[t][i] = (br_u64)pa | DESC_PAGE | normal;
        }
    }
    s_l1[1] = (br_u64)(br_uintptr_t)&s_l2_ram[0] | DESC_TABLE;
}

/* Low 1 GiB background: 512 2 MiB Device-nGnRnE blocks (L1[0]), covering all QEMU virt
 * peripherals/ROM/FLASH (GICD 0x08000000, GICR up to 0x081A0000, PL011 0x09000000), so MMIO
 * declarations need no table update. */
static void mmu_fill_device_span(void)
{
    const br_u64 device = mmu_desc_attrs(BR_MM_DEVICE);

    for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
        const br_uintptr_t pa = (br_uintptr_t)i * MMU_L2_BLOCK_SIZE;
        s_l2_dev[i] = (br_u64)pa | DESC_BLOCK | device;
    }
    s_l1[0] = (br_u64)(br_uintptr_t)&s_l2_dev[0] | DESC_TABLE;
}

/* Write one region's attrs into the page tables (activate and post-activation map_region).
 * RAM window (4 KiB pages): change page by page; crossing the window boundary => -EINVAL.
 * Low 1 GiB (2 MiB Device blocks): QEMU virt MMIO frames are all smaller than one block, so
 * sub-block attr changes => -ENOTSUP; attrs matching the background => no-op 0; a block-aligned
 * span => change block-wise. Outside both spans => -EINVAL (layout and declaration disagree). */
static int mmu_region_apply(const br_mm_region_t *r)
{
    const br_uintptr_t base = r->base;
    const br_size_t    size = r->size;

    if (base < BR_PLAT_RAM_WINDOW_END && (base + size) > BR_PLAT_RAM_BASE) {
        const br_u64 attrs = mmu_desc_attrs(r->attrs);

        if (base < BR_PLAT_RAM_BASE || (base + size) > BR_PLAT_RAM_WINDOW_END) {
            return BR_ERR(EINVAL);       /* crosses the window boundary: inexpressible at 4 KiB */
        }

        for (br_size_t off = 0u; off < size; off += MMU_GRANULE) {
            br_u32 level = 0u;
            br_u64 *d = mmu_desc_ptr(base + off, &level);

            if (d == BR_NULL) {
                return BR_ERR(EINVAL);   /* not covered by the page table: layout mismatch */
            }
            if (level != 3u) {
                return BR_ERR(ENOTSUP);  /* sub-block attr change: inexpressible at 2 MiB */
            }
            *d = (*d & ~DESC_ATTR_MASK) | attrs;
        }
        mmu_tlb_flush_all();
        return 0;
    }

    if (base < BR_PLAT_DEVICE_SPAN && (base + size) <= BR_PLAT_DEVICE_SPAN) {
        const br_u64 want = mmu_desc_attrs(r->attrs);

        if (want == mmu_desc_attrs(BR_MM_DEVICE)) {
            return 0;       /* background already Device => no table change */
        }
        if ((base % MMU_L2_BLOCK_SIZE) == 0ull && (size % MMU_L2_BLOCK_SIZE) == 0u) {
            for (br_size_t off = 0u; off < size; off += MMU_L2_BLOCK_SIZE) {
                br_u32 level = 0u;
                br_u64 *d = mmu_desc_ptr(base + off, &level);

                if (d == BR_NULL || level != 2u) {
                    return BR_ERR(EINVAL);
                }
                *d = (*d & ~DESC_ATTR_MASK) | want;
            }
            mmu_tlb_flush_all();
            return 0;
        }
        return BR_ERR(ENOTSUP);
    }

    return BR_ERR(EINVAL);
}

/* ---- the six br_mm_ops_t implementations ---- */

static int mmu_activate(const br_mm_region_t *regions, br_u32 count)
{
    if (s_activated) {
        return BR_ERR(EBUSY);
    }
    if (regions == BR_NULL && count != 0u) {
        return BR_ERR(EINVAL);
    }

    /* Background mapping: Device blocks for the low 1 GiB + Normal pages for the RAM window.
     * BSS was cleared by start.S; unwritten entries stay invalid, which is where translation
     * faults come from. */
    mmu_fill_device_span();
    mmu_fill_ram_window();

    /* Region-table overlay: write each declared attrs into the page tables. */
    for (br_u32 i = 0; i < count; i++) {
        const int rc = mmu_region_apply(&regions[i]);
        if (rc != 0) {
            return rc;
        }
    }

    /* Install the page-table base and memory attribute registers. */
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((br_u64)(br_uintptr_t)&s_l1[0]) : "memory");
    __asm__ volatile("msr tcr_el1, %0"   :: "r"(MMU_TCR_VALUE)  : "memory");
    __asm__ volatile("msr mair_el1, %0"  :: "r"(MMU_MAIR_VALUE) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* Read-modify-write SCTLR_EL1 and enable the MMU, keeping the existing value: set M|C|I,
     * clear A/SA/SA0 (Normal memory must allow unaligned accesses; see MMU_SCTLR_A), set the RES1
     * bits. Never assume the reset value. */
    {
        br_u64 sctlr = mmu_read_sctlr();

        sctlr |= (MMU_SCTLR_M | MMU_SCTLR_C | MMU_SCTLR_I | MMU_SCTLR_RES1);
        sctlr &= ~(MMU_SCTLR_A | MMU_SCTLR_SA | MMU_SCTLR_SA0);
        mmu_write_sctlr(sctlr);
        __asm__ volatile("isb" ::: "memory");
    }

    s_activated = BR_TRUE;

    /* Log the platform's own hardware facts; core's br_mem_init() logs its own pool summary. */
    {
        br_plat_mmu_info_t info;

        br_plat_mmu_info(&info);
        br_log_info("mem: identity map on (SCTLR=0x%lx, regions=%u, 4 KiB pages=%u, 2 MiB device blocks=%u)",
                    info.sctlr, info.regions, info.mapped_pages, info.device_blocks);
        br_log_info("mem: TTBR0=0x%lx TCR=0x%lx MAIR=0x%lx",
                    info.ttbr0, info.tcr, info.mair);
    }
    return 0;
}

static int mmu_map_region(const br_mm_region_t *r)
{
    if (r == BR_NULL || r->size == 0u) {
        return BR_ERR(EINVAL);
    }
    if (!s_activated) {
        return BR_ERR(ENODEV);       /* not activated yet; activate builds the tables in one pass */
    }
    return mmu_region_apply(r);
}

static int mmu_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs)
{
    if ((addr & ((br_uintptr_t)MMU_GRANULE - 1ull)) != 0ull || size == 0u) {
        return BR_ERR(EINVAL);
    }

    /* Round the length down to pages: at 4 KiB granularity there is no partial page. */
    br_size_t remaining = size & ~((br_size_t)MMU_GRANULE - 1ull);
    br_uintptr_t cur = addr;
    const br_u64 new_attrs = mmu_desc_attrs(attrs);

    while (remaining > 0u) {
        br_u32 level = 0u;
        br_u64 *d = mmu_desc_ptr(cur, &level);

        if (d == BR_NULL) {
            return BR_ERR(EINVAL);       /* attr changes on unmapped spans are not allowed */
        }

        if (level == 2u) {
            /* 2 MiB block: accept only block-aligned, whole-block requests. */
            if ((cur & ((br_uintptr_t)MMU_L2_BLOCK_SIZE - 1ull)) != 0ull
                || remaining < (br_size_t)MMU_L2_BLOCK_SIZE) {
                return BR_ERR(ENOTSUP);
            }
            *d = (*d & ~DESC_ATTR_MASK) | new_attrs;
            cur += MMU_L2_BLOCK_SIZE;
            remaining -= MMU_L2_BLOCK_SIZE;
        } else {
            *d = (*d & ~DESC_ATTR_MASK) | new_attrs;
            cur += MMU_GRANULE;
            remaining -= MMU_GRANULE;
        }
    }

    mmu_tlb_flush_all();
    return 0;
}

static int mmu_query(br_uintptr_t addr, br_size_t size, br_u32 *out_attrs)
{
    if (out_attrs == BR_NULL || size == 0u) {
        return BR_ERR(EINVAL);
    }
    if ((addr + size) < addr) {
        return BR_ERR(EINVAL);       /* span wraparound */
    }

    /*
     * Walk the span page by page: answer only if fully mapped and all attrs equal; mixed attrs are
     * not "one attribute", so return -EINVAL. addr itself need not be page-aligned (callers sample
     * unaligned variable/stack addresses); only the internal page boundaries are aligned.
     */
    const br_uintptr_t lo = addr & ~((br_uintptr_t)MMU_GRANULE - 1ull);
    const br_uintptr_t hi = (addr + size - 1ull) & ~((br_uintptr_t)MMU_GRANULE - 1ull);
    br_u32 first = 0u;
    br_u32 index = 0u;

    for (br_uintptr_t cur = lo; ; cur += MMU_GRANULE) {
        br_u32 level = 0u;
        const br_u64 *d = mmu_desc_ptr(cur, &level);

        if (d == BR_NULL || (*d & DESC_AF) == 0ull) {
            return BR_ERR(EFAULT);
        }

        const br_u32 a = mmu_attrs_from_desc(*d);
        if (index == 0u) {
            first = a;
        } else if (a != first) {
            return BR_ERR(EINVAL);
        }
        index++;

        if (cur >= hi) {
            break;
        }
    }

    *out_attrs = first;     /* page-table attrs only, no BR_MM_KIND_* */
    return 0;
}

/*
 * Cache maintenance around DMA. core already decided "cacheable" and "MMU on", so this is purely
 * mechanical: one instruction per 64 B line + dsb. flush = dc cvac (clean to PoC);
 * invalidate = dc ivac (drop the local copy).
 */
static void mmu_cache_flush(void *addr, br_size_t size)
{
    const br_uintptr_t base = (br_uintptr_t)addr;

    for (br_size_t off = 0u; off < size; off += MMU_CACHE_LINE) {
        __asm__ volatile("dc cvac, %0" :: "r"(base + off) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
}

static void mmu_cache_invalidate(void *addr, br_size_t size)
{
    const br_uintptr_t base = (br_uintptr_t)addr;

    for (br_size_t off = 0u; off < size; off += MMU_CACHE_LINE) {
        __asm__ volatile("dc ivac, %0" :: "r"(base + off) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
}

static const br_mm_ops_t s_mm_ops = {
    .activate         = mmu_activate,
    .map_region       = mmu_map_region,
    .set_attrs        = mmu_set_attrs,
    .query            = mmu_query,
    .cache_flush      = mmu_cache_flush,
    .cache_invalidate = mmu_cache_invalidate,
};

/* ---- platform observation surface (br_mmu.h) ---- */

void br_plat_mmu_info(br_plat_mmu_info_t *out)
{
    if (out == BR_NULL) {
        return;
    }

    out->sctlr = mmu_read_sctlr();
    out->tcr   = mmu_read_tcr();
    out->mair  = mmu_read_mair();
    out->ttbr0 = mmu_read_ttbr0();

    /* Counting walks the page tables, not a shadow count of what we meant to map. */
    out->mapped_pages = 0u;
    for (br_u32 t = 0; t < MMU_RAM_L3; t++) {
        for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
            if ((s_l3_ram[t][i] & DESC_TYPE_MASK) == DESC_PAGE) {
                out->mapped_pages++;
            }
        }
    }

    out->device_blocks = 0u;
    for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
        if ((s_l2_dev[i] & DESC_TYPE_MASK) == DESC_BLOCK) {
            out->device_blocks++;
        }
    }

    out->regions = br_mm_region_count();
}

int br_plat_mmu_probe(br_uintptr_t addr, br_u32 *out_attrs)
{
    return br_mm_query(addr, (br_size_t)MMU_GRANULE, out_attrs);
}

int br_plat_mmu_translate(br_uintptr_t va, br_uintptr_t *pa)
{
    if (pa == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (!s_activated) {
        return BR_ERR(ENODEV);
    }

    br_u32 level = 0u;
    const br_u64 *d = mmu_desc_ptr(va, &level);

    if (d == BR_NULL || (*d & DESC_AF) == 0ull) {
        return BR_ERR(EFAULT);
    }
    *pa = mmu_desc_pa(va, *d, level);
    return 0;
}

/* ---- boot wiring (called by br_plat_early_init) ----
 * Only registers the platform page-table ops; "build tables + enable MMU" (br_mm_activate) is a
 * core boot-chain step, executed by br_core_main() after platform plugin init. */
int br_plat_mmu_ops_register(void)
{
    return br_mm_register(&s_mm_ops);
}
