/*
 * brickOS prototype v0.1.0 — QEMU virt (aarch64) page-table construction (identity map + region attrs)
 *
 * Design basis:
 *   - `docs/3-os-core/3-04-memory.md` §1: v1 virtual memory policy = **identity map + attribute isolation**;
 *   - `docs/3-os-core/3-01-core-api-list.md` §7 (the four mechanical paths of `br_mm_ops_t`:
 *     activate / map_region / set_attrs / query) and §13.6 (map/protect are P4 = machine,
 *     declarable only by platform — the basis for "page tables in platform");
 *   - CA-8: page granularity = platform compile-time constant; this prototype fixes 4 KiB
 *     (`BR_PAGE_SIZE`), so page pool / page table / L3 descriptors agree.
 *
 * This file is only the "mechanical path", never policy:
 *   - policy (which RAM is which pool / which span is MMIO) lives in `src/memmap.c`'s region table;
 *   - the state machine (unregistered → registered → activated) lives in core's `br_mm_activate()`;
 *   - this file **reads the region table into page tables** and **reads page tables back** into attrs (query).
 *
 * ★ `WORKAROUND(br-wa-isa-001)`: the design assigns "page-table construction + aarch64 cache
 *   coherency protocol" to the **ISA shared library (not a plugin)** (3-04 §2, 3-01 §7 header),
 *   but the prototype has no such layer — so this file sits temporarily in the platform plugin
 *   directory and keeps layering via the **file boundary**: only "changes with arch/granularity"
 *   descriptor sequences here, board facts in `memmap.c`. See WORKAROUNDS.md for the debt.
 *
 * ★ Page-table memory lives in .bss (inside the image), so the **identity map must cover the page
 *   tables themselves**: the fetch right after writing SCTLR_EL1.M, and every table walk after it,
 *   must be translatable by the new tables. The table base is the linker-provided image address,
 *   inside the 8 MiB RAM window (see the window guard in `br_plat_memmap_init`).
 */
#include <br/core/br_mm.h>
#include <br/core/br_error.h>
#include <br/core/br_log.h>

#include <br/platform/br_mmu.h>

/* =====================================================================
 * Constants and descriptor encoding
 * ===================================================================== */
#define MMU_ENTRIES          512u                  /* entries per table level */
#define MMU_GRANULE          4096u                 /* 4 KiB granularity (TG0=0b00) */
#define MMU_L2_BLOCK_SIZE    0x00200000u           /* 2 MiB L2 block */
#define MMU_RAM_L3           4u                    /* 8 MiB / 2 MiB = 4 L3 tables */
#define MMU_CACHE_LINE       64u                   /* aarch64 minimum cache line (maintenance granularity) */

/* Descriptor encoding (4 KiB granularity, 39-bit VA ⇒ walk starts at L1, 4 levels):
 *
 *   Table descriptor (L1/L2 → next table):  next_table_pa | 0b11
 *   L2 block descriptor (2 MiB):            pa            | 0b01 | attrs
 *   L3 page descriptor (4 KiB):             pa            | 0b11 | attrs
 *
 *   Common bits: AF(bit10) = 1
 *   Normal:   SH(bits 9:8) = 0b11 (inner shareable), AttrIndx(bits 4:2) = 0 (MAIR Attr0)
 *   Device:   SH(bits 9:8) = 0b00 (non-shareable), AttrIndx(bits 4:2) = 1 (MAIR Attr1)
 *   AP[2:1](bits 7:6): 0b00 = RW, 0b10 = RO(bit7 = 1)
 *   NX:       PXN(bit53) = 1 and UXN(bit54) = 1
 */
#define DESC_TYPE_MASK       0x3ull
#define DESC_TABLE          0x3ull        /* table descriptor valid + table */
#define DESC_BLOCK          0x1ull        /* L2 block descriptor valid + block */
#define DESC_PAGE           0x3ull        /* L3 page descriptor valid + page */
#define DESC_AF             (1ull << 10)
#define DESC_SH_SHIFT       8u
#define DESC_SH_INNER       0x3ull        /* Normal: inner shareable */
#define DESC_SH_NONE        0x0ull        /* Device: non-shareable */
#define DESC_AP_SHIFT       6u
#define DESC_AP_RO          (0x2ull << DESC_AP_SHIFT)   /* AP[2] = bit7 = 1 ⇒ EL1 read-only */
#define DESC_ATTRIDX_SHIFT  2u
#define DESC_ATTRIDX_NORMAL 0ull          /* MAIR Attr0 = 0xFF(Normal WB RA/WA) */
#define DESC_ATTRIDX_DEVICE 1ull          /* MAIR Attr1 = 0x00(Device-nGnRnE) */
#define DESC_PXN            (1ull << 53)
#define DESC_UXN            (1ull << 54)

/* Attribute bit mask: when changing attrs **keep base and type bits**, flip only these */
#define DESC_ATTR_MASK      (DESC_AF | (0x3ull << DESC_SH_SHIFT) | (0x3ull << DESC_AP_SHIFT) \
                             | (0x7ull << DESC_ATTRIDX_SHIFT) | DESC_PXN | DESC_UXN)

/* Output address masks (per the ARMv8 4 KiB-granularity descriptor layout) */
#define MM_ADDR_MASK        0x0000FFFFFFFFF000ull   /* bits 47:12 */
#define MM_BLOCK_MASK_L2    0x0000FFFFFFE00000ull   /* bits 47:21 (2 MiB block) */

/* MAIR_EL1: Attr0 = 0xFF (Normal WB RA/WA, cacheable RAM), Attr1 = 0x00 (Device-nGnRnE),
 * Attr2 = 0x44 (Normal Non-cacheable, spare; this prototype emits no such AttrIndx) */
#define MMU_MAIR_VALUE      0x004400FFull

/*
 * TCR_EL1 (per the page-table policy in 3-04 §1):
 *   T0SZ(bits 5:0)   = 25      ⇒ 39-bit VA, walk starts at L1
 *   IRGN0(bits 9:8)  = 0b01    ⇒ table walks: inner WB WA (cacheable)
 *   ORGN0(bits 11:10)= 0b11    ⇒ table walks: outer Non-cacheable
 *   SH0(bits 13:12)  = 0b11    ⇒ table walks: inner shareable (kept even on a single core so
 *                                walks stay shareable once SMP arrives)
 *   TG0(bits 15:14)  = 0b00    ⇒ 4 KiB granularity
 *   EPD1(bit23)      = 1       ⇒ **TTBR0 only**; any address with bit63 = 1 takes a translation fault
 *   IPS(bits 34:32)  = 0b010   ⇒ 40-bit physical addresses (RAM/MMIO fits inside 4 GiB here)
 */
#define MMU_TCR_VALUE       ((25ull) | (1ull << 8) | (3ull << 10) | (3ull << 12) \
                             | (1ull << 23) | (2ull << 32))

/* SCTLR_EL1 RES1 bits: bit11/20/22/23/28/29 must be 1 (set explicitly on read-modify-write, never assume the reset value) */
#define MMU_SCTLR_RES1      ((1ull << 11) | (1ull << 20) | (1ull << 22) | (1ull << 23) \
                             | (1ull << 29) | (1ull << 28))
#define MMU_SCTLR_M         (1ull << 0)     /* MMU enable */
#define MMU_SCTLR_C         (1ull << 2)     /* data cache enable */
#define MMU_SCTLR_I         (1ull << 12)    /* instruction cache enable */

/*
 * Alignment checks: clear all three switches A / SA / SA0.
 * ★ Encoding correction (measured calibration): the work order said "clear SA(bit4)/SA0(bit5)",
 *   which are ARMv7/AArch32 field numbers. In ARMv8-A SCTLR_EL1:
 *     A   = bit1  —— **data access** alignment check (this is what faults unaligned accesses)
 *     SA  = bit3  —— SP alignment check (EL1)
 *     SA0 = bit4  —— SP alignment check (EL0)
 *   To honour the intent "unaligned accesses to Normal memory take no alignment fault", A(bit1)
 *   must be cleared, not just the SP checks. bit5 is CP15BEN (the AArch32 CP15 barrier switch),
 *   unrelated to this intent, so leave it untouched — clearing a semantically unrelated control
 *   bit would be a genuine deviation.
 */
#define MMU_SCTLR_A         (1ull << 1)
#define MMU_SCTLR_SA        (1ull << 3)
#define MMU_SCTLR_SA0       (1ull << 4)

/* =====================================================================
 * Page-table memory (static arrays: 1 L1 + 1 Device L2 + 1 RAM L2 + 4 L3 = 7 tables = 28 KiB)
 *
 * Why static and not from the page pool: the pool's precondition is "mappings already active",
 * while the page tables are the precondition for mappings being active — chicken and egg (CA-8
 * has zero v1 consumers, 3-01 §6). The linker pins these arrays in .bss, and start.S's BSS clear
 * incidentally marks every entry invalid.
 * ===================================================================== */
static br_u64 s_l1[MMU_ENTRIES]                 __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l2_dev[MMU_ENTRIES]             __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l2_ram[MMU_ENTRIES]             __attribute__((aligned(MMU_GRANULE)));
static br_u64 s_l3_ram[MMU_RAM_L3][MMU_ENTRIES] __attribute__((aligned(MMU_GRANULE)));

/* Activation state: `activate` may succeed only once (repeat ⇒ -EBUSY, same as core's state machine) */
static br_bool s_activated;

_Static_assert(MMU_RAM_L3 * MMU_L2_BLOCK_SIZE == BR_PLAT_RAM_WINDOW_SIZE,
               "L3 table count does not match the RAM mapping window");

/* =====================================================================
 * Descriptor and system-register helpers
 * ===================================================================== */

/* region attrs → descriptor attribute bits. DEVICE wins over CACHED: both set is contradictory
 * input, so take the more conservative Device (guessing wrong means cached peripheral registers,
 * far worse than slow Device accesses to RAM). */
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

/* descriptor attribute bits → region attrs. ★ This does **not synthesize the region-table kind**:
 * query answers "the attrs actually in effect in the page table right now"; kind is "what this
 * memory was declared as" — the former is a hardware fact (possibly changed by set_attrs), the
 * latter a declaration fact (region table). The split is documented in `br_mm.h`'s observation
 * contract (see its read-only observation surface): kind is answered by br_mm_region_*.
 * Merging the two would make a page whose attrs were changed (e.g. MM-RO) always read back the
 * declared kind, defeating the case. */
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

/*
 * Page-table walk: returns a **writable pointer to the last-level descriptor**, with
 * `*out_level` = 2 (2 MiB block) or 3 (4 KiB page). Unmapped / invalid entry ⇒ NULL.
 *
 * Why identity-map pointer arithmetic: this platform's policy is the identity map, so the
 * physical address in a descriptor can be dereferenced directly as a pointer (the biggest
 * convenience the identity map gives page-table construction, and one reason 3-04 §1 chose it).
 */
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

/* descriptor → physical address (frame base, no in-page offset) */
static br_uintptr_t mmu_desc_pa(br_uintptr_t va, br_u64 d, br_u32 level)
{
    if (level == 2u) {
        return (va & ~((br_uintptr_t)MMU_L2_BLOCK_SIZE - 1ull)) | (br_uintptr_t)(d & MM_BLOCK_MASK_L2);
    }
    return (va & ~((br_uintptr_t)MMU_GRANULE - 1ull)) | (br_uintptr_t)(d & MM_ADDR_MASK);
}

/* After page tables change the TLB must see it: this prototype uses the coarsest but never-wrong
 * granularity (vmalle1). Per-entry `tlbi vaae1` is an optimization, not correctness — and
 * correctness wins (4 KiB pages give only 2048 mappings anyway). */
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

/* =====================================================================
 * Table construction (internal action of activate)
 * ===================================================================== */

/* **Background mapping** for the RAM window: 4 L3 tables mapping every page, default
 * Normal WB RW executable. "Pages not covered by a declaration stay mapped" is deliberate
 * (3-04 §1): the identity map's value is that the kernel sees its own memory at unchanged
 * addresses; holes in the window would make any undeclared local object fault mysteriously. */
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

/* **Background mapping** for the low 1 GiB: 512 2 MiB Device-nGnRnE blocks (L1[0]).
 * Covers all QEMU virt peripherals/ROM/FLASH (GICD 0x08000000, GICR 0x080A0000..0x081A0000,
 * PL011 0x09000000 all inside), so MMIO declarations in the region table need **no** table
 * update — the background is already correct (see mmu_region_apply's "same attrs ⇒ no-op"
 * handling for Device spans). */
static void mmu_fill_device_span(void)
{
    const br_u64 device = mmu_desc_attrs(BR_MM_DEVICE);

    for (br_u32 i = 0; i < MMU_ENTRIES; i++) {
        const br_uintptr_t pa = (br_uintptr_t)i * MMU_L2_BLOCK_SIZE;
        s_l2_dev[i] = (br_u64)pa | DESC_BLOCK | device;
    }
    s_l1[0] = (br_u64)(br_uintptr_t)&s_l2_dev[0] | DESC_TABLE;
}

/*
 * Write one region's attrs into the page tables (shared by activate and post-activation `map_region`).
 *
 * Two spans, two granularities:
 *   - RAM window (4 KiB page area): change attrs page by page; this is where "attribute isolation"
 *     actually happens;
 *   - low 1 GiB (2 MiB Device block area): QEMU virt MMIO frames (GICD 64 KiB / GICR 1 MiB /
 *     PL011 4 KiB) are all **smaller than one block**, so at 2 MiB granularity the page table
 *     simply cannot express "change only these 4 KiB". The boundary is therefore hard: sub-block
 *     attr changes ⇒ `-ENOTSUP` (see the MM-NXDESC comment: never pretend to support what the
 *     hardware cannot express); attrs matching the background ⇒ no-op returning 0; a block-aligned
 *     span ⇒ may be changed block-wise.
 *   - outside both spans ⇒ `-EINVAL` (declaration surface and page-table layout disagree; this
 *     must not be silently ignored).
 */
static int mmu_region_apply(const br_mm_region_t *r)
{
    const br_uintptr_t base = r->base;
    const br_size_t    size = r->size;

    if (base < BR_PLAT_RAM_WINDOW_END && (base + size) > BR_PLAT_RAM_BASE) {
        const br_u64 attrs = mmu_desc_attrs(r->attrs);

        if (base < BR_PLAT_RAM_BASE || (base + size) > BR_PLAT_RAM_WINDOW_END) {
            return BR_ERR(EINVAL);       /* crosses the window boundary: 4 KiB pages cannot express it */
        }

        for (br_size_t off = 0u; off < size; off += MMU_GRANULE) {
            br_u32 level = 0u;
            br_u64 *d = mmu_desc_ptr(base + off, &level);

            if (d == BR_NULL) {
                return BR_ERR(EINVAL);   /* page table does not cover it ⇒ layout and declaration disagree */
            }
            if (level != 3u) {
                return BR_ERR(ENOTSUP);  /* sub-block attr change at block granularity: inexpressible */
            }
            *d = (*d & ~DESC_ATTR_MASK) | attrs;
        }
        mmu_tlb_flush_all();
        return 0;
    }

    if (base < BR_PLAT_DEVICE_SPAN && (base + size) <= BR_PLAT_DEVICE_SPAN) {
        const br_u64 want = mmu_desc_attrs(r->attrs);

        if (want == mmu_desc_attrs(BR_MM_DEVICE)) {
            return 0;       /* background is already Device ⇒ declaration holds, no table change */
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

/* =====================================================================
 * The six br_mm_ops_t implementations
 * ===================================================================== */

static int mmu_activate(const br_mm_region_t *regions, br_u32 count)
{
    if (s_activated) {
        return BR_ERR(EBUSY);
    }
    if (regions == BR_NULL && count != 0u) {
        return BR_ERR(EINVAL);
    }

    /* ① Background mapping: Device blocks for the low 1 GiB + Normal pages for the RAM window.
     *    (BSS was cleared by start.S; only the entries actually used are written, the rest stay
     *     invalid — outside the window and between 1 GiB..4 GiB is where translation faults come from.) */
    mmu_fill_device_span();
    mmu_fill_ram_window();

    /* ② Region-table overlay: write each declared attrs into the page tables (no second source of truth) */
    for (br_u32 i = 0; i < count; i++) {
        const int rc = mmu_region_apply(&regions[i]);
        if (rc != 0) {
            return rc;
        }
    }

    /* ③ Install the page-table base and memory attribute registers */
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"((br_u64)(br_uintptr_t)&s_l1[0]) : "memory");
    __asm__ volatile("msr tcr_el1, %0"   :: "r"(MMU_TCR_VALUE)  : "memory");
    __asm__ volatile("msr mair_el1, %0"  :: "r"(MMU_MAIR_VALUE) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* ④ Read-modify-write SCTLR_EL1 and enable the MMU.
     *    Keep the existing value (do not overwrite firmware/QEMU settings), explicitly:
     *      - set M|C|I (bit0/2/12);
     *      - clear A/SA/SA0 (bit1/3/4) — otherwise unaligned accesses to Normal memory can still
     *        take an alignment fault, while this prototype's page-table policy is exactly "Normal
     *        memory allows unaligned accesses" (see the Makefile note on -mstrict-align: it is a
     *        defensive knob now, not a hard requirement; field-number correction in MMU_SCTLR_A above);
     *      - set the RES1 bits (bit11/20/22/23/28/29) to 1 — they must be 1; never assume the reset value. */
    {
        br_u64 sctlr = mmu_read_sctlr();

        sctlr |= (MMU_SCTLR_M | MMU_SCTLR_C | MMU_SCTLR_I | MMU_SCTLR_RES1);
        sctlr &= ~(MMU_SCTLR_A | MMU_SCTLR_SA | MMU_SCTLR_SA0);
        mmu_write_sctlr(sctlr);
        __asm__ volatile("isb" ::: "memory");
    }

    s_activated = BR_TRUE;

    /*
     * The platform records its own hardware facts (page/block counts, register read-back).
     * core's `br_mem_init()` already logs its own "heap/pool" summary (see ADR-0008 stage ③) —
     * each side states its own facts and does not log for the other.
     */
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
        return BR_ERR(ENODEV);       /* page tables not built yet; activate builds them in one pass from a snapshot */
    }
    return mmu_region_apply(r);
}

static int mmu_set_attrs(br_uintptr_t addr, br_size_t size, br_u32 attrs)
{
    if ((addr & ((br_uintptr_t)MMU_GRANULE - 1ull)) != 0ull || size == 0u) {
        return BR_ERR(EINVAL);
    }

    /* Round the length down to pages (no partial protection of a trailing page: at 4 KiB granularity there is no "half page") */
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
            /* 2 MiB block: accept only block-aligned, whole-block requests (Device span boundary in the file header) */
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
     * Walk the span page by page over the pages it covers: answer only if fully mapped and all
     * attrs equal; mixed attrs (half RO / half NX) are not "one attribute", so report -EINVAL
     * rather than fudging it with the first page.
     * ★ addr need not be page-aligned — the caller asks "what attrs does the page holding this
     *   address/byte have right now" (MM-IDENT samples exactly the **unaligned** addresses of
     *   variables and stack), so only the internal page boundaries must be aligned.
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

    *out_attrs = first;     /* ★ page-table facts only, no BR_MM_KIND_* (see mmu_attrs_from_desc) */
    return 0;
}

/*
 * Cache maintenance (design 3-01 §7; around DMA, risk R4).
 * core has already decided "is this span cacheable" and "is the MMU on" (see br_mm.h semantics),
 * so this only performs mechanical actions: one maintenance instruction per 64 B line + one dsb.
 *   - flush     = `dc cvac` (clean to PoC; keep the copy, send it out)
 *   - invalidate= `dc ivac` (invalidate; drop the local copy, next read comes from memory)
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

/* =====================================================================
 * Platform observation surface (br_mmu.h)
 * ===================================================================== */

void br_plat_mmu_info(br_plat_mmu_info_t *out)
{
    if (out == BR_NULL) {
        return;
    }

    out->sctlr = mmu_read_sctlr();
    out->tcr   = mmu_read_tcr();
    out->mair  = mmu_read_mair();
    out->ttbr0 = mmu_read_ttbr0();

    /* Counting walks the page tables themselves (not a shadow count of "how much we meant to map") */
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

/* =====================================================================
 * Boot wiring (called by br_plat_early_init, see the ordering notes in br_plat.h / br_mmu.h)
 *
 * ★ This function only **registers** the platform page-table ops; "build tables + enable MMU"
 *   (br_mm_activate) is one step of core.init (design 1-01 §9), executed by core's entry
 *   `br_core_main()` after platform plugin init — mechanism in platform, orchestration/state
 *   machine in core (the three-layer boundary). See ADR-0008.
 * ===================================================================== */
int br_plat_mmu_ops_register(void)
{
    return br_mm_register(&s_mm_ops);
}
