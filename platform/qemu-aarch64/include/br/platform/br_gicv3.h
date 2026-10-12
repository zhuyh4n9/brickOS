/*
 * brickOS prototype v0.1.0 — GICv3 driver contract (ISA-layer dialect)
 *
 * Design basis:
 *   `docs/1-01-architecture.md` §8 "platform capability three-layer model" —
 *     interrupt controller | `br_pic` (mask/unmask/ack/prio/eoi) | GICv3 driver | IRQ number binding, routing policy
 *   `docs/3-02-int.md` §4.1 (`br_pic_ops` table fill) / §4.1.1 (EOImode obligation) / §7.1 (priority encoding belongs to the dialect)
 *
 * ⚠ Layer note (the prototype's honest boundary, WORKAROUND br-wa-isa-001): on the design side the GICv3 register
 *   sequence belongs to an ISA shared library (a reusable library, not a plugin). This prototype has no separate ISA
 *   library layer yet, so `src/gicv3.c` sits next to the platform plugin — but the file boundary is the layer boundary: this header and
 *   `gicv3.c` describe only what changes with the controller model (register offsets/bitfields/sequences) and contain no
 *   QEMU/board facts (base addresses/binding table/pins are in `plat_qemu_virt.c` + `board_irq.c`).
 *
 * ★ Three dialect differences this driver must absorb itself (core does not know them):
 *   1. the whole 1020–1023 range of `ICC_IAR1_EL1` maps to BR_PIC_NO_IRQ (§5.3);
 *   2. `ICC_CTLR_EL1.EOImode` must be 0 (otherwise eoi takes two steps) (§4.1.1);
 *   3. quantizing logical priority (small = high) to hardware encoding: GICv3's significant bits are the
 *      high bits (MSB-aligned) and saturate toward low priority when the width is short (§7.1, never silently raise priority).
 */
#ifndef BR_PLATFORM_BR_GICV3_H
#define BR_PLATFORM_BR_GICV3_H

#include <br/core/br_types.h>
#include <br/core/br_irq.h>     /* BR_IRQ_TRIG_* / BR_IRQ_PRIO_* (the dialect interprets requests using these logical encodings) */
#include <br/core/br_pic.h>

/* GICv3 INTID boundaries (architectural constants, board-independent) */
#define BR_GICV3_INTID_SGI_MAX   15u     /* 0–15   software generated interrupts (per core) */
#define BR_GICV3_INTID_PPI_BASE  16u     /* 16–31  per-core private peripheral interrupts (arch timer = 30) */
#define BR_GICV3_INTID_PPI_MAX   31u
#define BR_GICV3_INTID_SPI_BASE  32u     /* 32+    shared peripheral interrupts */
#define BR_GICV3_INTID_SPECIAL   1020u   /* 1020–1023: core must map to BR_PIC_NO_IRQ */
#define BR_GICV3_INTID_MAX       1023u

/* PMR admits all implementable priorities (§7.1 init obligation) */
#define BR_GICV3_PMR_ALL         0xFFu

/* Platform data: address and size of the GICv3 instance (changes per board => belongs to platform) */
typedef struct {
    br_uintptr_t gicd_base;       /* distributor (GICD) */
    br_uintptr_t gicr_base;       /* redistributor (GICR) frame array base */
    br_u32       gicr_frame_size; /* per-core frame span (2 × 64 KiB) */
    br_u32       gicr_max_frames; /* scan bound for affinity matching */
    br_u32       nr_irq;          /* INTID upper bound (reported to caps; not used to size tables) */
} br_gicv3_cfg_t;

/*
 * Initialize this core's GICv3 CPU interface and register the PIC instance (EARLY phase, IRQs globally masked).
 * Internal order (3-02 §14.3 steps 1–2): `ICC_SRE_EL1` -> GICD_CTLR (ARE_NS + Grp1) ->
 * find this core's GICR frame by MPIDR affinity -> wake -> set IGROUPR0/IPRIORITYR0 ->
 * `ICC_PMR_EL1 = 0xFF` -> `ICC_IGRPEN1_EL1 = 1` -> assert `EOImode == 0`.
 * Returns pic_id (>= 0) or a negative errno.
 */
int br_gicv3_register(const br_gicv3_cfg_t *cfg);

/* GICv3 facts read during this init (diagnostics/selftest; all zero means not yet initialized) */
typedef struct {
    br_u8  prio_bits;      /* implemented priority bits (ICC_CTLR_EL1.PRIbits + 5) */
    br_u8  eoimode;        /* value read back after setup (must be 0) */
    br_u32 spi_lines;      /* SPI lines reported by GICD_TYPER */
    br_u32 mpidr;          /* this core's affinity */
} br_gicv3_info_t;

void br_gicv3_info(br_gicv3_info_t *out);

/*
 * Software-trigger one SGI (INTID 0–15 only; CAP_SW_TRIGGER).
 * Takes exactly the same entry path (ack -> ISR -> eoi), so it is the interrupt subsystem's selftest channel (§8.4).
 */
int br_gicv3_sgi_trigger(br_u32 intid);

/* Hardware readback for cross-checking (CAP_READBACK; conformance/debug only, §4.3 item 5) */
int br_gicv3_readback_enabled(br_u32 intid, int *out);
int br_gicv3_readback_prio   (br_u32 intid, br_u8 *out);
int br_gicv3_readback_pending(br_u32 intid, int *out);

/*
 * conformance only: change the hardware enable bit by INTID directly (bypassing core's binding/shadow).
 * One use only — construct the "hardware delivers but core has no owner" scenario (TC-IRQ-008 spurious_owned,
 * simulating "firmware left a line enabled but unclaimed"). Product code must not call it (see the comment in gicv3.c).
 */
int br_gicv3_raw_line_enable(br_u32 intid, int enable);

#endif /* BR_PLATFORM_BR_GICV3_H */
