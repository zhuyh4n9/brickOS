/*
 * brickOS prototype v0.1.0 — QEMU virt (aarch64) region table declaration + pool init
 *
 * Design basis:
 *   - `docs/3-os-core/3-04-memory.md` §1/§2: platform declares the region table in
 *     early_init; "core does not own RAM, it only claims regions" (3-01 §6: the
 *     platform region table splits RAM into heap / contig pool / page pool, ratio =
 *     manifest budget);
 *   - `docs/3-os-core/3-01-core-api-list.md` §7 (`br_mm_region_add` belongs to Platform =
 *     map/protect is P4/machine, §13.6) and §6 (`br_mem_init` claims pools by kind);
 *   - `docs/6-test/6-01-test.md` §3.5/§3.6: MM-POOLS/MM-REGION check that the declared
 *     table matches the pools core claims.
 *
 * This file is **data**: which memory is what (QEMU virt addresses and sizes).
 * The mechanism lives in `src/mmu.c` (page-table construction) and the core
 * `br_mem`/`br_mm` implementations.
 *
 * ★ This file **only declares the region table**: pool claiming (`br_mem_init`) and
 *   page-table construction (`br_mm_activate`) are stage ③ of the core four-stage
 *   boot chain (see `core/src/main.c` / ADR-0008).
 *
 * ★ Debts still on record (see WORKAROUNDS.md):
 *   - `WORKAROUND(br-wa-mem-001)`: the **pool sizes are hard-coded here**
 *     (1 MiB / 256 KiB / 1 MiB / 256 KiB / 16 KiB), not generated from manifest
 *     `[budget]`/`[[res]]` — design 3-04 §2 wants "ratio = manifest budget", and
 *     v0.1 has no such generation path yet.
 *
 * ★ Two hard constraints on the region table (from the br_mm.h contract, enforced here):
 *   ① base/size must be 4 KiB aligned and page-granular — the page table expresses
 *      attribute isolation at 4 KiB granularity, so a finer declaration has no carrier;
 *   ② no overlap — overlap = "one page with two attribute truths", an unexecutable claim.
 */
#include <br/core/br_error.h>
#include <br/core/br_mem.h>      /* BR_PAGE_SIZE (page granularity) */
#include <br/core/br_mm.h>

#include <br/platform/br_mmu.h>
#include <br/platform/br_plat.h>

/* Image/stack bounds from the linker script (single truth: platform/qemu-aarch64/src/link.ld) */
extern char __image_start[];
extern char __stack_top[];

#define MEM_ALIGN_UP(v, a)  (((v) + ((br_uintptr_t)(a) - 1ull)) & ~((br_uintptr_t)(a) - 1ull))

/* Pool sizes. Rationale (prototype budget ruling; enough to finish the consistency
 * cases and all inside the 8 MiB window):
 *   HEAP    1 MiB   — TLSF heap; covers TC-MEM-002's 128-block stress + 4 KiB round trips
 *   CONTIG  256 KiB — contiguous pool; enough for DMA/relocation block allocation demos
 *   PAGE    1 MiB   — page pool = 256 pages (CA-8 bitmap size)
 *   DMA     256 KiB — DMA buffer pool (CA-6: built on contig semantics)
 *   RESERVED 16 KiB — never allocated: consistency cases use it for real RO/NX protection
 *                     checks (and there is room to clean up if broken)
 */
#define MEM_HEAP_SIZE       (1024u * 1024u)
#define MEM_CONTIG_SIZE     (256u * 1024u)
#define MEM_PAGE_POOL_SIZE  (1024u * 1024u)
#define MEM_DMA_SIZE        (256u * 1024u)
#define MEM_RESERVED_SIZE   (16u * 1024u)

/* MMIO windows (QEMU virt addresses, matching the GICv3/PL011 bases in platforms/...) */
#define MEM_GICD_BASE       0x08000000ul
#define MEM_GICD_SIZE       0x00010000ul    /* GICD distributor frame: 64 KiB */
#define MEM_GICR_BASE       0x080A0000ul
#define MEM_GICR_SIZE       0x00100000ul    /* GICR redistributor frames: 1 MiB (to 0x081A0000) */
#define MEM_UART0_BASE      0x09000000ul
#define MEM_UART0_SIZE      0x00001000ul    /* PL011: 4 KiB */

/* PCIe (ADR-0021 D-2; values shared with br_plat_pci_host() in src/pci_plat.c):
 *   ECAM        0x3f000000 + 16 MiB  — low window under **highmem=off**
 *   MMIO window 0x10000000 + ~752 MiB — BARs are allocated from here
 * Both live in the low 1 GiB Device background mapping ⇒ declaring them is a page-table
 * no-op (it only gives core a queryable table). */
#define MEM_PCIE_ECAM_BASE  0x3f000000ul
#define MEM_PCIE_ECAM_SIZE  0x01000000ul
#define MEM_PCIE_MMIO_BASE  0x10000000ul
#define MEM_PCIE_MMIO_SIZE  0x2eff0000ul

/* Boilerplate for registering one region (kept near the declarations for easy reading) */
static int memmap_add(br_uintptr_t base, br_size_t size, br_u32 attrs)
{
    br_mm_region_t r;

    r.base  = base;
    r.size  = size;
    r.attrs = attrs;
    return br_mm_region_add(&r);
}

static int memmap_declare_regions(void)
{
    const br_uintptr_t img_base  = (br_uintptr_t)&__image_start[0];
    const br_uintptr_t stack_top = (br_uintptr_t)&__stack_top[0];
    int rc;

    /*
     * ① IMAGE: image (text/rodata/data/bss) + boot stack.
     *
     * Why one merged region (instead of IMAGE / STACK): in link.ld `__stack_bottom`
     * directly follows `__image_end` with only a 16-byte alignment gap — two regions
     * rounded up to 4 KiB each would necessarily cover the same page frame and hit the
     * region table's **no-overlap rule**. This is a real constraint of page-granular
     * declarations, not laziness: stack and image share pages, so they share attributes.
     * Size = from image base up to __stack_top rounded up to a page (the gap from stack
     * top to pool base is naturally left by BR_PLAT_POOL_ALIGN's 2 MiB alignment; it is
     * not declared ⇒ those pages stay unused).
     */
    rc = memmap_add(img_base, MEM_ALIGN_UP(stack_top - img_base, BR_PAGE_SIZE),
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_IMAGE));
    if (rc != 0) {
        return rc;
    }

    /*
     * ② Pool base: 2 MiB aligned (< 2 MiB headroom, and keeps all five pools inside the
     * 8 MiB RAM mapping window). The five regions below are packed back-to-back with no
     * overlap.
     */
    const br_uintptr_t pool_base     = MEM_ALIGN_UP(stack_top, BR_PLAT_POOL_ALIGN);
    const br_uintptr_t heap_base     = pool_base;
    const br_uintptr_t contig_base   = heap_base + MEM_HEAP_SIZE;
    const br_uintptr_t page_base     = contig_base + MEM_CONTIG_SIZE;
    const br_uintptr_t dma_base      = page_base + MEM_PAGE_POOL_SIZE;
    const br_uintptr_t reserved_base = dma_base + MEM_DMA_SIZE;
    const br_uintptr_t pool_end      = reserved_base + MEM_RESERVED_SIZE;

    /* Guard: the whole pool window must land in the page table's 4 KiB page area.
     * Overflow ⇒ declaration and page-table layout disagree; fail loudly (ENOSPC)
     * instead of "half mapped" or silent truncation. */
    if (pool_end > BR_PLAT_RAM_WINDOW_END) {
        return BR_ERR(ENOSPC);
    }

    /* TLSF byte heap: the only source for malloc/calloc/realloc/free (3-01 §6) */
    rc = memmap_add(heap_base, MEM_HEAP_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_HEAP));
    if (rc != 0) {
        return rc;
    }

    /* Physically contiguous pool (CA-7): bypasses TLSF, for DMA engines and v2
     * relocation bulk moves */
    rc = memmap_add(contig_base, MEM_CONTIG_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_CONTIG));
    if (rc != 0) {
        return rc;
    }

    /* Page pool (CA-8): static bitmap + first-fit contiguous runs; 256 pages @ 4 KiB */
    rc = memmap_add(page_base, MEM_PAGE_POOL_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_PAGE));
    if (rc != 0) {
        return rc;
    }

    /* DMA buffer pool (CA-6): physically contiguous guaranteed (v1 identity ⇒ dma_addr == vaddr) */
    rc = memmap_add(dma_base, MEM_DMA_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_DMA));
    if (rc != 0) {
        return rc;
    }

    /* Reserved region: given to no pool; consistency cases do real RO/NX protection checks here */
    rc = memmap_add(reserved_base, MEM_RESERVED_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_RESERVED));
    if (rc != 0) {
        return rc;
    }

    /*
     * ③ Three MMIO regions: all inside the low 1 GiB Device background mapping (L1[0]
     *    2 MiB blocks), so the page table needs nothing further — the point of declaring
     *    them is to give core a queryable attribute table (cache maintenance deciding
     *    "is it cacheable", dump/ramdump deciding "can this be read").
     *    Sizes are the frames' **actual sizes**, not the covering 2 MiB block: the region
     *    table is for software queries, and over-declaring addresses would distort the
     *    "undeclared address" predicate (MM-REGION/MM-UNMAP).
     */
    rc = memmap_add(MEM_GICD_BASE, MEM_GICD_SIZE,
                    BR_MM_DEVICE | BR_MM_WITH_KIND(0u, BR_MM_KIND_MMIO));
    if (rc != 0) {
        return rc;
    }

    rc = memmap_add(MEM_GICR_BASE, MEM_GICR_SIZE,
                    BR_MM_DEVICE | BR_MM_WITH_KIND(0u, BR_MM_KIND_MMIO));
    if (rc != 0) {
        return rc;
    }

    rc = memmap_add(MEM_UART0_BASE, MEM_UART0_SIZE,
                    BR_MM_DEVICE | BR_MM_WITH_KIND(0u, BR_MM_KIND_MMIO));
    if (rc != 0) {
        return rc;
    }

    /*
     * ④ Two PCIe regions (ADR-0021 D-2): ECAM and the 32-bit MMIO window.
     *    As in ③ they fall in the low 1 GiB Device background ⇒ no page-table change;
     *    **highmem=off is a precondition** — with default highmem, ECAM sits at
     *    0x40_1000_0000, beyond `BR_PLAT_DEVICE_SPAN`, and `mmu_region_apply()` rejects
     *    it with -EINVAL (which is correct: a declaration inconsistent with the page-table
     *    layout must not be silently ignored). pci-core's BAR allocation also carves from
     *    this MMIO window.
     */
    rc = memmap_add(MEM_PCIE_ECAM_BASE, MEM_PCIE_ECAM_SIZE,
                    BR_MM_DEVICE | BR_MM_WITH_KIND(0u, BR_MM_KIND_MMIO));
    if (rc != 0) {
        return rc;
    }

    rc = memmap_add(MEM_PCIE_MMIO_BASE, MEM_PCIE_MMIO_SIZE,
                    BR_MM_DEVICE | BR_MM_WITH_KIND(0u, BR_MM_KIND_MMIO));
    return rc;
}

int br_plat_memmap_init(void)
{
    /*
     * ★ This function **only declares** the region table (which memory is what).
     *   Pool **claiming** (`br_mem_init()`) and page-table **construction**
     *   (`br_mm_activate()`) are each one slot of core.init (design 1-01 §9: "heap ·
     *   interrupt framework · registry · scheduler objects"), executed by the core entry
     *   `br_core_main()` after platform plugin init — see
     *   `docs/decisions/0008-core-main-boot-chain.md` and stage ③ in `core/src/main.c`.
     *   So platform provides only "data + the mechanism that changes per board", while
     *   memory management/allocation and address-mapping construction belong to core.
     *
     * Declaration failure (overlap / wrong granularity / table full) = boot failure
     * (3-02 §14.3 error-handling duty, same discipline as board_irq.c IRQ init).
     */
    return memmap_declare_regions();
}
