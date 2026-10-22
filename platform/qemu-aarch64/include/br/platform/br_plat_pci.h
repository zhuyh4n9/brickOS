/*
 * PCIe platform facts for QEMU virt (aarch64), exported by the platform plugin.
 * Board-level facts (ECAM base, MMIO window, INTx start line) stay here; generic PCI machinery
 * (ECAM access/enumeration/BAR allocation/device table) is in hal/pci-core.
 * highmem=off is a prerequisite: with default highmem ECAM sits above 4 GiB, which the two-segment
 * MMU cannot express; highmem=off puts it at 0x3f000000, inside the low 1 GiB Device background.
 */
#ifndef BR_PLATFORM_BR_PLAT_PCI_H
#define BR_PLATFORM_BR_PLAT_PCI_H

#include <br/core/br_types.h>

/*
 * Static facts of the PCIe host bridge (all values in the little-endian 32-bit PCI view).
 * `intx_virq_a` = virq of INTx A; pins B/C/D = +1/+2/+3. GPEX swizzles by device slot, so the line
 * used is `intx_virq_a + ((pin_index + device) % 4)`, computed by pci-core.
 */
typedef struct br_plat_pci_host {
    br_uintptr_t ecam_base;     /* ECAM (config space) base */
    br_size_t    ecam_size;
    br_uintptr_t mmio_base;     /* 32-bit MMIO window base (BARs allocated from here) */
    br_size_t    mmio_size;
    br_u32       bus_first;     /* bus number range (inclusive) */
    br_u32       bus_last;      /* inclusive */
    br_u32       intx_virq_a;   /* virq of INTx A (BR_IRQ_PCIE_INTX_A) */
} br_plat_pci_host_t;

/* Fetch the PCIe host facts; uninitialized/PCIe-less boards return -ENODEV. Pure data (no MMIO
 * access), so pci-core's CORE init may call it at any time. */
int br_plat_pci_host(br_plat_pci_host_t *out);

#endif /* BR_PLATFORM_BR_PLAT_PCI_H */
