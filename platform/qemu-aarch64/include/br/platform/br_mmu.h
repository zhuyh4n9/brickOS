/*
 * Platform observation surface of the QEMU virt memory mapping subsystem.
 * Interface in core (br/core/br_mm.h), page tables in src/mmu.c, region table in src/memmap.c.
 * These helpers are a platform self-test/debug surface, not part of core's API: cases need
 * "which PA does this VA translate to", which br_mm_query cannot answer.
 */
#ifndef BR_PLATFORM_BR_MMU_H
#define BR_PLATFORM_BR_MMU_H

#include <br/core/br_types.h>

/* Platform memory layout: single source of truth shared by the page table layout and the region
 * table. Kept public so cases use the same window boundary, not their own literals. */

/* RAM base (QEMU virt: -m 128M gives RAM = 0x40000000..0x48000000) */
#define BR_PLAT_RAM_BASE        0x40000000ul

/* RAM mapping window of the 4 KiB page tables = 8 MiB (L1[1] -> L2 -> 4 L3 tables); entries outside
 * stay invalid, so undeclared RAM faults. 8 MiB fits image + boot stack + five pools. */
#define BR_PLAT_RAM_WINDOW_SIZE 0x00800000ul
#define BR_PLAT_RAM_WINDOW_END  (BR_PLAT_RAM_BASE + BR_PLAT_RAM_WINDOW_SIZE)

/* Low 1 GiB = all 512 2 MiB Device-nGnRnE blocks (L1[0]); every QEMU virt peripheral/ROM/FLASH
 * lives there (GICR frames reach 0x081A0000), so per-device mapping needs extra tables. */
#define BR_PLAT_DEVICE_SPAN     0x40000000ul

/* Pool base alignment: 2 MiB, leaving a < 2 MiB gap between image/stack and the pools (the region
 * table forbids overlap). */
#define BR_PLAT_POOL_ALIGN      0x00200000ul

/* Conformance-only address, outside the RAM window and page-table invalid; QEMU gives no bus error
 * for nonexistent memory, so the invalid entry is the deterministic fault source. */
#define BR_PLAT_CONF_UNMAPPED   0x42000000ul

/* ---- MMU observation surface ---- */

/* Read-back snapshot; sctlr/tcr/mair/ttbr0 come straight from hardware via mrs, not from shadow
 * variables (cases judge what the hardware took, not what we wrote). */
typedef struct {
    br_u64 sctlr;           /* SCTLR_EL1: M(bit0)=1 means MMU is on */
    br_u64 tcr;             /* TCR_EL1: T0SZ/EPD1/IPS/TG0 etc. */
    br_u64 mair;            /* MAIR_EL1: Attr0=Normal WB / Attr1=Device-nGnRnE */
    br_u64 ttbr0;           /* TTBR0_EL1: L1 table physical base */
    br_u32 mapped_pages;    /* currently valid 4 KiB page count (inside the RAM window) */
    br_u32 device_blocks;   /* currently valid 2 MiB Device block count (low 1 GiB backdrop) */
    br_u32 regions;         /* core region table entry count (br_mm_region_count) */
} br_plat_mmu_info_t;

/* Read back the snapshot above. No-op when `out == NULL`. */
void br_plat_mmu_info(br_plat_mmu_info_t *out);

/* Single-page attribute probe: br_mm_query(addr, 4 KiB, out_attrs). */
int br_plat_mmu_probe(br_uintptr_t addr, br_u32 *out_attrs);

/* Page-table translation: PA of the frame for `va` (base part, no in-page offset).
 * Unmapped / invalid entry => -EFAULT; `pa == NULL` => -EINVAL. The only criterion for the
 * identity-map policy: br_mm_query answers attributes, not where a VA translates. */
int br_plat_mmu_translate(br_uintptr_t va, br_uintptr_t *pa);

/* Boot wiring, called by br_plat_early_init in this order: platform-side data and mechanism only.
 * Pool claiming and page table creation are core boot-chain steps. */

/* Declare the region table (register only; no pool claim, no page tables). Returns 0 or a negative
 * errno (overlap / bad granularity / table full = boot failure, caller must halt). */
int br_plat_memmap_init(void);

/* Register the platform page table ops (duplicate registration => core reports -EBUSY). */
int br_plat_mmu_ops_register(void);

#endif /* BR_PLATFORM_BR_MMU_H */
