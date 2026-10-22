/*
 * Board IRQ numbers (single source of truth for virq), QEMU virt aarch64.
 * Number model: logical name / virq (only drivers see it) / hwirq (ISA layer). virq layout:
 * 0..47 direct PIC lines; 48..55 IPI (reserved for v2b); 56..63 cascaded-domain window
 * (BR_IRQ_DOMAIN_BASE). Hand-written; brickie v0.1 only generates plugin descriptors.
 */
#ifndef BR_BOARD_IRQ_H
#define BR_BOARD_IRQ_H

#include <br/core/br_types.h>

/* ---- direct PIC lines (virq; binding table in board_irq.c) ---- */

/* INTID 30 = EL1 physical timer (PPI). */
#define BR_IRQ_TIMER          0u
/* INTID 33 = PL011 UART0 (SPI 1). Unused (polling console); bound for the SPI mask/prio path. */
#define BR_IRQ_UART0          1u
/* INTID 0 = SGI 0: software trigger for selftest/conformance; per-core CPU-interface line. */
#define BR_IRQ_TEST_SGI       2u
/* INTID 1 = SGI 1: ISR self-retrigger, for the storm case (software livelock). */
#define BR_IRQ_TEST_SGI_STORM 3u
/* INTID 2 = SGI 2: parent of the fake FAST cascaded domain (members in virq 56..). */
#define BR_IRQ_DOMAIN_PARENT  4u
/* INTID 4 = SGI 4: parent of the fake SLOW cascaded domain (demux runs in the bh). */
#define BR_IRQ_DOMAIN_SLOW    5u
/* INTID 5 = SGI 5: BH line (BR_IRQ_F_DISPATCH_BH); ISR runs in the bh, not interrupt context. */
#define BR_IRQ_BH_LINE        6u

/* ---- PCIe INTx (virq 7..10 -> INTID 35..38) ----
 * GPEX maps INTx A..D to SPI 3..6 and swizzles by slot: (pin_index + device) % 4 (A=0..D=3).
 * pci-core picks the virq from slot + PCI_INTERRUPT_PIN; platform gives only the start number.
 * Dispatch = BH; trigger = level, so the source must be cleared device-side or it livelocks.
 */
#define BR_IRQ_PCIE_INTX_A    7u
#define BR_IRQ_PCIE_INTX_B    8u
#define BR_IRQ_PCIE_INTX_C    9u
#define BR_IRQ_PCIE_INTX_D   10u

/* Direct-line entry count (binding table length; domain members need no entry, P-IRQ-1). */
#define BR_IRQ_BOARD_NR      11u

/* SGI (INTID 3) left unbound: delivery with no owner; used by TC-IRQ-008 (spurious_owned). */
#define BR_IRQ_TEST_SGI_UNBOUND   3u

#endif /* BR_BOARD_IRQ_H */
