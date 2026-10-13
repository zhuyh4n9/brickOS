/*
 * QEMU virt (aarch64) region table declaration. Data only: which memory is what (QEMU virt
 * addresses and sizes); page-table mechanism is in src/mmu.c, claiming/activation in core
 * (br_mem_init / br_mm_activate, core boot chain stage 3).
 * WORKAROUND(br-wa-mem-001): pool sizes are hard-coded, not generated from the manifest.
 * Region table constraints: base/size 4 KiB aligned and page-granular; no overlap.
 */
#include <br/core/br_error.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>

#include <br/platform/br_mmu.h>
#include <br/platform/br_plat.h>

/* Image/stack bounds from the linker script (single truth: platform/qemu-aarch64/src/link.ld) */
extern char __image_start[];
extern char __stack_top[];

#define MEM_ALIGN_UP(v, a)  (((v) + ((br_uintptr_t)(a) - 1ull)) & ~((br_uintptr_t)(a) - 1ull))

/* Pool sizes (budget ruling; all inside the 8 MiB window):
 *   HEAP 1 MiB (TLSF; TC-MEM-002 128-block stress), CONTIG 256 KiB (DMA/relocation bulk moves),
 *   PAGE 1 MiB = 256 pages (CA-8 bitmap size), DMA 256 KiB (CA-6), RESERVED 16 KiB (never
 *   allocated; conformance uses it for real RO/NX protection checks). */
#define MEM_HEAP_SIZE       (1024u * 1024u)
#define MEM_CONTIG_SIZE     (256u * 1024u)
#define MEM_PAGE_POOL_SIZE  (1024u * 1024u)
#define MEM_DMA_SIZE        (256u * 1024u)
#define MEM_RESERVED_SIZE   (16u * 1024u)

/* MMIO windows: GICD 64 KiB, GICR frames 1 MiB (to 0x081A0000), PL011 4 KiB. */
#define MEM_GICD_BASE       0x08000000ul
#define MEM_GICD_SIZE       0x00010000ul
#define MEM_GICR_BASE       0x080A0000ul
#define MEM_GICR_SIZE       0x00100000ul
#define MEM_UART0_BASE      0x09000000ul
#define MEM_UART0_SIZE      0x00001000ul

/* PCIe (values shared with br_plat_pci_host() in src/pci_plat.c): ECAM 0x3f000000 + 16 MiB under
 * highmem=off, MMIO window 0x10000000 + ~752 MiB (BARs are allocated from here). Both are in the
 * low 1 GiB Device background, so declaring them is a page-table no-op. */
#define MEM_PCIE_ECAM_BASE  0x3f000000ul
#define MEM_PCIE_ECAM_SIZE  0x01000000ul
#define MEM_PCIE_MMIO_BASE  0x10000000ul
#define MEM_PCIE_MMIO_SIZE  0x2eff0000ul

/* Register one region. */
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
     * IMAGE: image (text/rodata/data/bss) + boot stack, merged because __stack_bottom directly
     * follows __image_end, so two page-rounded regions would overlap and violate the no-overlap
     * rule. Size = image base to __stack_top rounded up to a page; the gap up to the pool base is
     * left by BR_PLAT_POOL_ALIGN's 2 MiB alignment and stays undeclared.
     */
    rc = memmap_add(img_base, MEM_ALIGN_UP(stack_top - img_base, BR_PAGE_SIZE),
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_IMAGE));
    if (rc != 0) {
        return rc;
    }

    /*
     * Pool base: 2 MiB aligned (< 2 MiB headroom, keeps all five pools inside the 8 MiB RAM
     * mapping window); the five regions are packed back-to-back with no overlap.
     */
    const br_uintptr_t pool_base     = MEM_ALIGN_UP(stack_top, BR_PLAT_POOL_ALIGN);
    const br_uintptr_t heap_base     = pool_base;
    const br_uintptr_t contig_base   = heap_base + MEM_HEAP_SIZE;
    const br_uintptr_t page_base     = contig_base + MEM_CONTIG_SIZE;
    const br_uintptr_t dma_base      = page_base + MEM_PAGE_POOL_SIZE;
    const br_uintptr_t reserved_base = dma_base + MEM_DMA_SIZE;
    const br_uintptr_t pool_end      = reserved_base + MEM_RESERVED_SIZE;

    /* The pool window must land in the page table's 4 KiB page area; overflow means declaration
     * and page-table layout disagree, so fail loudly with -ENOSPC instead of half-mapping. */
    if (pool_end > BR_PLAT_RAM_WINDOW_END) {
        return BR_ERR(ENOSPC);
    }

    /* TLSF byte heap: the only source for malloc/calloc/realloc/free */
    rc = memmap_add(heap_base, MEM_HEAP_SIZE,
                    BR_MM_CACHED | BR_MM_WITH_KIND(0u, BR_MM_KIND_HEAP));
    if (rc != 0) {
        return rc;
    }

    /* Contiguous pool (CA-7): bypasses TLSF, for DMA engines and v2 relocation bulk moves */
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

    /* DMA buffer pool (CA-6): contiguous (v1 identity => dma_addr == vaddr) */
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
     * Three MMIO regions: all inside the low 1 GiB Device background (L1[0] 2 MiB blocks), so the
     * page table needs nothing further; they exist so core has a queryable attribute table. Sizes
     * are the frames' actual sizes, not the covering 2 MiB block (over-declaring would distort the
     * "undeclared address" predicate used by MM-REGION/MM-UNMAP).
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
     * Two PCIe regions: ECAM and the 32-bit MMIO window. As above they fall in the low 1 GiB
     * Device background, so no page-table change. highmem=off is a precondition: with default
     * highmem ECAM sits beyond BR_PLAT_DEVICE_SPAN and mmu_region_apply() rejects it with
     * -EINVAL. pci-core's BAR allocation also carves from this MMIO window.
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
     * Declares the region table only (which memory is what); pool claiming and page-table
     * construction are separate core boot-chain steps.
     * Declaration failure (overlap / wrong granularity / table full) = boot failure.
     */
    return memmap_declare_regions();
}
