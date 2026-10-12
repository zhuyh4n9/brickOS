/*
 * brickOS prototype v0.1.0 — platform observation surface of the QEMU virt (aarch64)
 * memory mapping subsystem
 *
 * Design basis:
 *   - `docs/3-os-core/3-04-memory.md` §1: v1 virtual memory policy = **identity map +
 *     attribute isolation**;
 *   - `docs/3-os-core/3-01-core-api-list.md` §7 (br-mm group: region table + map/unmap +
 *     cache maintenance) and §13.6 (memory three axes: ops × granularity × regions;
 *     map/protect are P4, declarable only by platform);
 *   - `docs/6-test/6-01-test.md` §3.5/§3.6 (expected TC-MEM-* and TC-MM-* on target).
 *
 * Where the three-layer model (design 1-01 §8) lands in this subsystem:
 *   interface in core (`br/core/br_mm.h`: `br_mm_ops_t` / `br_mm_register` / `br_mm_activate`),
 *   construction in this platform (page tables in `src/mmu.c` + region table in `src/memmap.c`),
 *   data also in this platform (QEMU virt RAM/MMIO addresses and sizes).
 *
 * Why these functions are not part of the core observation contract (CA-5 "small surface" rule):
 *   they are a **platform self-test / debug surface** — the conformance cases (`src/mm_conf.c`)
 *   compare "the real result in the page tables" against "the case's expectation", while
 *   `br_mm_query` answers only attributes, not "which PA does this VA translate to".
 *   The identity-map criterion can only be "PA in the page table == VA frame", so
 *   `br_plat_mmu_translate()` is the **only reliable** translation observation point this
 *   platform exposes to cases (same treatment as the platform increments registered in ADR-0003).
 */
#ifndef BR_PLATFORM_BR_MMU_H
#define BR_PLATFORM_BR_MMU_H

#include <br/core/br_types.h>

/* ---------------------------------------------------------------- platform memory layout
 * These constants are the **single source of truth shared by the page table layout and the
 * region table** (design 3-04 §1: page table layout belongs to platform).
 * Kept in the platform public header instead of hidden in the .c: conformance cases need the
 * same window boundary to build "unmapped addresses", and if a case wrote its own literals a
 * changed window would silently leave the case testing the old boundary.
 */

/* RAM base (QEMU virt: -m 128M gives RAM = 0x40000000..0x48000000) */
#define BR_PLAT_RAM_BASE        0x40000000ul

/* **RAM mapping window** of the 4 KiB page tables = 8 MiB (L1[1] → L2 → 4 L3 tables).
 * Outside the window (the rest of this RAM segment) entries **stay invalid**, so "accessing
 * undeclared RAM" raises a translation fault — the only way cases build a recoverable fault
 * (TC-MM-* / MM-UNMAP).
 * Why 8 MiB: image + boot stack + five pools (2.5 MiB total) all fit inside, and mapping more
 * would only waste page table memory. */
#define BR_PLAT_RAM_WINDOW_SIZE 0x00800000ul
#define BR_PLAT_RAM_WINDOW_END  (BR_PLAT_RAM_BASE + BR_PLAT_RAM_WINDOW_SIZE)

/* Low 1 GiB = all 512 2 MiB Device-nGnRnE blocks (L1[0]).
 * Why the whole span instead of "map only the 1 MiB in use": every QEMU virt peripheral/ROM/FLASH
 * lives in the low 1 GiB and the GICR frames reach 0x081A0000 — per-device mapping is both fragile
 * (one missed block is a bizarre fault) and needs extra page tables. */
#define BR_PLAT_DEVICE_SPAN     0x40000000ul

/* Alignment granularity of pool bases: 2 MiB. A pool base is `ALIGN_UP(&__stack_top, 2 MiB)`,
 * which naturally leaves a < 2 MiB gap between image/stack and the pools (the region table
 * forbids overlap; once merged into one IMAGE region this gap separates them). */
#define BR_PLAT_POOL_ALIGN      0x00200000ul

/* Address used by conformance cases: outside the RAM segment window and page-table invalid.
 * 0x42000000 lies outside the 8 MiB window (0x40000000+0x800000 = 0x40800000) but inside the
 * 128 MiB RAM segment — QEMU does not give a bus error for "memory that does not exist", so the
 * invalid page table entry is the deterministic fault source. */
#define BR_PLAT_CONF_UNMAPPED   0x42000000ul

/* ---------------------------------------------------------------- MMU observation surface */

/* Read-back snapshot of page tables/registers. `sctlr/tcr/mair/ttbr0` are read straight from
 * hardware with `mrs` (not a copy of a shadow variable we wrote) — "we programmed it" and
 * "the hardware really took it" are two different things, and cases judge the latter. */
typedef struct {
    br_u64 sctlr;           /* SCTLR_EL1: M(bit0)=1 means MMU is on */
    br_u64 tcr;             /* TCR_EL1: T0SZ/EPD1/IPS/TG0 etc. */
    br_u64 mair;            /* MAIR_EL1: Attr0=Normal WB / Attr1=Device-nGnRnE */
    br_u64 ttbr0;           /* TTBR0_EL1: L1 table physical base */
    br_u32 mapped_pages;    /* currently valid 4 KiB page count (inside the RAM window) */
    br_u32 device_blocks;   /* currently valid 2 MiB Device block count (low 1 GiB backdrop) */
    br_u32 regions;         /* core region table entry count (br_mm_region_count) */
} br_plat_mmu_info_t;

/* Read back the snapshot above. No-op when `out == NULL` (callers need not check first). */
void br_plat_mmu_info(br_plat_mmu_info_t *out);

/* Single-page attribute probe: thin wrapper over `br_mm_query(addr, 4 KiB, out_attrs)`.
 * Why this layer: cases read "the page table's real attributes" and can go through core's query
 * dispatch, but repeating `br_mm_query(a, BR_PAGE_SIZE, &v)` in cases drowns the intent in
 * boilerplate (see 6-01 §3.6). */
int br_plat_mmu_probe(br_uintptr_t addr, br_u32 *out_attrs);

/* Page table translation: return the physical address of the frame for `va` (base part, without
 * the in-page offset).
 * Unmapped / invalid entry ⇒ `-EFAULT`; `pa == NULL` ⇒ `-EINVAL`.
 * ★ This is the **only criterion** for the identity-map policy (MM-IDENT):
 *   `br_mm_query` only says "mapped with which attributes", not "translated where"; only walking
 *   the page table and taking the base can assert PA == VA frame. */
int br_plat_mmu_translate(br_uintptr_t va, br_uintptr_t *pa);

/* ---------------------------------------------------------------- boot wiring
 * `br_plat_early_init()` calls these two steps in this order (**platform-side data and
 * mechanism only**).
 *
 * ★ The pool **claim** (`br_mem_init`) and page table **creation** (`br_mm_activate`) are not
 *   here — they are one cell of core.init (design 1-01 §9: "heap · interrupt framework ·
 *   registry · scheduler framework objects"), executed by the core entry `br_core_main()` after
 *   platform plugin init. Division of labor: platform provides region data + page table
 *   construction mechanism, core provides the abstraction and performs the creation action.
 *   See `docs/decisions/0008-core-main-boot-chain.md` and stage ③ of `core/src/main.c`.
 */

/* ① Declare the region table (image/stack/five pools/MMIO). **Register only; do not claim pools
 * or touch page tables**.
 * Returns 0 or a negative errno (overlap / bad granularity / table full = boot failure, caller
 * must halt). */
int br_plat_memmap_init(void);

/* ② Register the platform page table ops (`br_mm_register`) — fills in only the "build tables +
 * enable MMU" mechanism.
 * Returns 0 or a negative errno (duplicate registration ⇒ core reports -EBUSY). */
int br_plat_mmu_ops_register(void);

#endif /* BR_PLATFORM_BR_MMU_H */
