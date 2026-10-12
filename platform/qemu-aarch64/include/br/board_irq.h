/*
 * brickOS prototype v0.1.0 — board IRQ number header (the **single source of truth** for virq)
 *
 * Design basis: `docs/3-os-core/3-02-int.md` §3.1 (IR-2 "two-layer number model") —
 *   three layers: **logical line name** (declared in the manifest) / **virq** (fixed at compose
 *   time, the only one drivers see) / **hwirq** (number inside a PIC domain, only inside the ISA
 *   layer/PIC ops). Driver source has **zero numbers and zero board knowledge**:
 *   `#include <br/board_irq.h>` then write `br_irq_register(BR_IRQ_TIMER, ...)`.
 *
 * ★ Why this header is a **hand-written static header** and not a composer-generated artifact:
 *   3-02 §3.1 explicitly allows the fallback — "if the generated-header mechanism of 2-01/4-03 is
 *   not in place: the platform plugin exports a static header with same-shaped macros. Semantically
 *   equivalent, only 'who generates it' differs ⇒ this design does not block toolchain progress".
 *   `brickie` v0.1 only generates plugin descriptors (`build/gen/<plugin>/plugin_desc.c`), it does
 *   **not generate board_irq.h** (that mechanism is open item O-4 of 2-01) ⇒ this prototype takes
 *   that **in-design** fallback.
 *
 * Number-space layout (3-02 §3.2): 0..47 = direct PIC lines; 48..55 = IPI (reserved for v2b);
 * 56..63 = cascaded-domain sub-interrupt window (`BR_IRQ_DOMAIN_BASE`, sliced by domain_create).
 */
#ifndef BR_BOARD_IRQ_H
#define BR_BOARD_IRQ_H

#include <br/core/br_types.h>

/* ---- direct PIC lines (virq 0..4; binding table in board_irq.c) ---- */

/* INTID 30 = EL1 physical timer (PPI). M0's only ISR that is "really doing work" (3-02 §11.1 path four). */
#define BR_IRQ_TIMER          0u
/* INTID 33 = PL011 UART0 (SPI 1). v0.1 has only a polling console, the interrupt is unused; it is
 * bound in the table to exercise the SPI mask/prio/readback path. */
#define BR_IRQ_UART0          1u
/* INTID 0  = SGI 0: software trigger channel for selftest/conformance cases (3-02 §8.4).
 * SGI is a **per-core** line and can only be triggered at the CPU interface — exactly the minimal
 * vehicle for "software trigger takes the identical entry path". */
#define BR_IRQ_TEST_SGI       2u
/* INTID 1  = SGI 1: vehicle for the storm-protection case (ISR self-retriggers ⇒ software equivalent of a level-triggered livelock). */
#define BR_IRQ_TEST_SGI_STORM 3u
/* INTID 2  = SGI 2: **parent line** of the fake FAST cascaded domain (domain members live in the virq 56.. window). */
#define BR_IRQ_DOMAIN_PARENT  4u
/* INTID 4  = SGI 4: parent line of the fake **SLOW** cascaded domain (ADR-0011: demux uses the bottom half bh). */
#define BR_IRQ_DOMAIN_SLOW    5u
/* INTID 5  = SGI 5: **BH direct line** — registered with `BR_IRQ_F_DISPATCH_BH`, so its ISR does
 * not run in interrupt context but is deferred by core to the bottom half (design §11.1 path one / §12.4).
 * Why a separate line instead of reusing the FAST domain parent line: this one verifies the
 * **per-line dispatch shape**, which is a different thing from the "one line with N sub-sources"
 * domain mechanism (§11.4's "two entry kinds", which should not be merged into one). */
#define BR_IRQ_BH_LINE        6u

/* ---- PCIe INTx (ADR-0021; virq 7..10 → INTID 35..38)----
 *
 * QEMU virt's PCIe host bridge (GPEX) maps INTx A..D to SPI 3..6 (INTID 35..38) via
 * `a15irqmap[VIRT_PCIE]=3`, and **swizzles by device slot number in the standard way**:
 *     intid_offset = (pin_index + device) % 4        (pin_index: A=0..D=3)
 * So all four lines are bound here (virq A..D ↔ hwirq 35..38), and `framework/pci-core` picks the
 * correct virq from the device's slot and `PCI_INTERRUPT_PIN` — board knowledge stops at the "start number".
 *
 * Dispatch shape = **BH** (design `7-02` §5 ISR→bh→wake): the SDHCI ISR does the minimum,
 * transfer completion/error is handed to the waiting thread by the bottom half. Trigger = level
 * (PCIe INTx is LEVEL_HIGH in QEMU, see `hw/arm/virt.c::create_pcie_irq_map`), so the source
 * **must be cleared device-side** or it livelocks. */
#define BR_IRQ_PCIE_INTX_A    7u
#define BR_IRQ_PCIE_INTX_B    8u
#define BR_IRQ_PCIE_INTX_C    9u
#define BR_IRQ_PCIE_INTX_D   10u

/* number of direct-line entries (binding table length; domain members need no binding entry — see README implementation ruling P-IRQ-1) */
#define BR_IRQ_BOARD_NR      11u

/* SGI (INTID 3) deliberately **left unbound**: a sample of "hardware delivery with no owner",
 * used by TC-IRQ-008 (the spurious_owned path). */
#define BR_IRQ_TEST_SGI_UNBOUND   3u

#endif /* BR_BOARD_IRQ_H */
