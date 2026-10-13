/*
 * Generic PCI(e) mechanism contract (outward face of framework/pci-core). Board facts (ECAM base,
 * MMIO window, INTx A virq) come from the platform; this file does ECAM config access, bus
 * enumeration, BAR sizing/allocation and INTx swizzle ((pin_index + slot) % 4). br_pci_init runs in
 * the CORE phase; the device table is read-only afterwards, so queries are lock-free.
 */
#ifndef BR_PCI_BR_PCI_H
#define BR_PCI_BR_PCI_H

#include <br/core/br_types.h>

/* Static bounds (prototype: no dynamic allocation). */
#define BR_PCI_MAX_DEVS  8u
#define BR_PCI_MAX_BARS  6u

/* class code in PCI config space (high = class, low = subclass); System SDHCI = 0x0805. */
#define BR_PCI_CLASS_SDHCI  0x0805u

/* One enumerated PCI function (BARs already allocated). bar[i] == 0 means the BAR is absent or
 * unallocated, not "base address 0" (0 is a reserved invalid address). */
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

/* Enumerate + allocate BARs (idempotent: repeated calls return the first result). Failure =
 * negative errno (-ENODEV: platform has no PCIe; -ENOSPC: window too small). Called by this
 * file's CORE init. */
int br_pci_init(void);

/* Device table queries (read-only; not inited or no match => 0/NULL). */
br_u32               br_pci_dev_count(void);
const br_pci_dev_t  *br_pci_dev_at(br_u32 index);
const br_pci_dev_t  *br_pci_find(br_u16 vendor, br_u16 device);

/* Raw config-space access (32-bit aligned) for drivers walking vendor-private registers. off low 2
 * bits are rounded down to 4. Returns 0 or a negative errno. */
int br_pci_cfg_read32 (br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 *out);
int br_pci_cfg_write32(br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 value);

#endif /* BR_PCI_BR_PCI_H */
