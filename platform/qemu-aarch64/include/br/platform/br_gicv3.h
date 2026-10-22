/*
 * GICv3 driver contract (ISA-layer dialect).
 * Layer boundary: only controller-model facts (register offsets/bitfields/sequences) belong here;
 * QEMU/board facts (base addresses, binding table, pins) live in plat_qemu_virt.c + board_irq.c.
 * Three dialect facts core does not know: 1020-1023 in ICC_IAR1_EL1 maps to BR_PIC_NO_IRQ;
 * ICC_CTLR_EL1.EOImode must be 0; priority quantizes to the high bits and saturates toward low.
 */
#ifndef BR_PLATFORM_BR_GICV3_H
#define BR_PLATFORM_BR_GICV3_H

#include <br/core/br_types.h>
#include <br/hal/br_irq.h>
#include <br/hal/br_pic.h>

/* GICv3 INTID boundaries (architectural, board-independent); 1020-1023 are special. */
#define BR_GICV3_INTID_SGI_MAX   15u
#define BR_GICV3_INTID_PPI_BASE  16u
#define BR_GICV3_INTID_PPI_MAX   31u
#define BR_GICV3_INTID_SPI_BASE  32u
#define BR_GICV3_INTID_SPECIAL   1020u
#define BR_GICV3_INTID_MAX       1023u

/* PMR admits all implementable priorities. */
#define BR_GICV3_PMR_ALL         0xFFu

/* GICv3 instance address/size (per board). */
typedef struct {
    br_uintptr_t gicd_base;       /* distributor (GICD) */
    br_uintptr_t gicr_base;       /* redistributor (GICR) frame array base */
    br_u32       gicr_frame_size; /* per-core frame span (2 x 64 KiB) */
    br_u32       gicr_max_frames; /* scan bound for affinity matching */
    br_u32       nr_irq;          /* INTID bound (reported to caps, not used to size tables) */
} br_gicv3_cfg_t;

/* Initialize this core's GICv3 CPU interface (EARLY phase, IRQs globally masked), set EOImode==0
 * and register the PIC instance. Returns pic_id (>= 0) or a negative errno. */
int br_gicv3_register(const br_gicv3_cfg_t *cfg);

/* GICv3 facts read during init (diagnostics/selftest; all zero means not yet initialized) */
typedef struct {
    br_u8  prio_bits;      /* implemented priority bits (ICC_CTLR_EL1.PRIbits + 5) */
    br_u8  eoimode;        /* value read back after setup (must be 0) */
    br_u32 spi_lines;      /* SPI lines reported by GICD_TYPER */
    br_u32 mpidr;          /* this core's affinity */
} br_gicv3_info_t;

void br_gicv3_info(br_gicv3_info_t *out);

/* Software-trigger one SGI (INTID 0-15 only; CAP_SW_TRIGGER); same entry path as hardware. */
int br_gicv3_sgi_trigger(br_u32 intid);

/* Hardware readback for conformance/debug cross-checks (CAP_READBACK). */
int br_gicv3_readback_enabled(br_u32 intid, int *out);
int br_gicv3_readback_prio   (br_u32 intid, br_u8 *out);
int br_gicv3_readback_pending(br_u32 intid, int *out);

/* conformance only: set a line's hardware enable bit by INTID, bypassing core's binding/shadow.
 * Builds the "hardware delivers but core has no owner" scenario (TC-IRQ-008 spurious_owned). */
int br_gicv3_raw_line_enable(br_u32 intid, int enable);

#endif /* BR_PLATFORM_BR_GICV3_H */
