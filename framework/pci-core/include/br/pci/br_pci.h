/*
 * brickOS prototype v0.2.0 — generic PCI(e) mechanism contract (outward face of framework/pci-core)
 *
 * Design basis: **no design-doc entry** — PCI is a domain added by this cut (ADR-0021 D-2).
 * Ownership rulings in brief:
 *   - board facts (where ECAM is / where the MMIO window is / INTx base line) stay in
 *     `platform/qemu-aarch64` (`br_plat_pci_host`); the generic mechanism is **here**;
 *   - this file reads those facts through an `init` dependency on platform (the dependency
 *     direction table allows `ability → platform`); exit conditions (multiple platforms /
 *     a core bus service) are in ADR-0021 §4.
 *
 * ## What this file does (R-S4's line of defense)
 *   ① **ECAM config-space access** (32-bit; little-endian; aligned accesses only — ECAM is in
 *      the Device background mapping);
 *   ② **bus enumeration** (bus×dev×fn scan from bus 0, match vendor≠0xFFFF);
 *   ③ **BAR size probing and allocation** (standard trick: write all 1s, read back to size →
 *      carve from the platform MMIO window → write back), plus enabling the endpoint's
 *      **Memory Space + Bus Master** and keeping **INTx enabled** (command register bit10=0).
 *   Also a static device table with `br_pci_find` (consumers look up by vendor/device),
 *   and **swizzle computation** for the INTx line (QEMU GPEX: `(pin_index + slot) % 4`, ADR-0021).
 *
 * ## What this file does **not** do
 *   - it knows no concrete device (sdhci-pci etc.): that is the driver consuming it;
 *   - no MSI/MSI-X (endpoints use INTx; see ADR-0021 D-4), no PCIe link training / power management;
 *   - no bus-number assignment behind bridges (v1 faces root-bus endpoints only; see the
 *     comment on `br_pci_find`).
 *
 * ## Context
 *   `br_pci_init` runs in the CORE phase (plugin init); the device table is read-only afterwards.
 *   A consumer (driver) init depends on this file via an `init` edge ⇒ topological order
 *   guarantees "the table is ready". The query surface is callable from any thread (lock-free:
 *   table is read-only).
 */
#ifndef BR_PCI_BR_PCI_H
#define BR_PCI_BR_PCI_H

#include <br/core/br_types.h>

/* Static bounds (prototype stance: no dynamic allocation, "stop when sufficient" as in dev-core/cdev-core). */
#define BR_PCI_MAX_DEVS  8u
#define BR_PCI_MAX_BARS  6u

/* class code in PCI config space (high = class, low = subclass); System SDHCI = 0x0805. */
#define BR_PCI_CLASS_SDHCI  0x0805u

/*
 * One enumerated PCI function (BARs already allocated).
 * `bar[i] == 0` means the BAR is absent / unallocated (not "base address 0" — 0 is a
 * reserved invalid address).
 */
typedef struct br_pci_dev {
    br_u16       vendor;
    br_u16       device;
    br_u16       class_code;    /* class<<8 | subclass */
    br_u8        revision;
    br_u8        bus;
    br_u8        dev;           /* device number (0..31); used by INTx swizzle */
    br_u8        fn;
    br_u8        intx_pin;      /* config space 0x3D: 1..4 = INTx A..D; 0 = none */
    br_u8        _pad[3];
    br_u32       intx_virq;     /* virq bound by the platform; 0 = no INTx */
    br_uintptr_t bar[BR_PCI_MAX_BARS];
    br_u32       bar_size[BR_PCI_MAX_BARS];
} br_pci_dev_t;

/*
 * Enumerate + allocate BARs (idempotent: repeated calls return the first result).
 * Failure = negative errno (platform has no PCIe ⇒ -ENODEV; window too small for BARs ⇒ -ENOSPC).
 * Called by this file's CORE init; consumers normally need not call it explicitly.
 */
int br_pci_init(void);

/* Device table queries (read-only; not inited or no match ⇒ 0/NULL). */
br_u32               br_pci_dev_count(void);
const br_pci_dev_t  *br_pci_dev_at(br_u32 index);
const br_pci_dev_t  *br_pci_find(br_u16 vendor, br_u16 device);

/* Raw config-space access (32-bit aligned). An escape hatch for a driver walking vendor-private
 * registers / extended capabilities. `off` low 2 bits are ignored (rounded down to 4). Returns 0 or negative errno. */
int br_pci_cfg_read32 (br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 *out);
int br_pci_cfg_write32(br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 value);

#endif /* BR_PCI_BR_PCI_H */
