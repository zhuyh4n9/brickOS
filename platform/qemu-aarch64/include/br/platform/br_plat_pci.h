/*
 * brickOS prototype v0.2.0 — **PCIe platform facts** for QEMU virt (aarch64) (exported by platform)
 *
 * Design basis: no design-doc entry (PCI is a new domain in this change, see
 * `docs/decisions/0021-bdev-pci-emmc.md` D-2). Ownership ruling: **board-level facts stay in
 * platform** (where ECAM is, where the MMIO window is, which line INTx starts at); generic PCI
 * machinery (ECAM access/enumeration/BAR allocation/device table) is in `framework/pci-core`.
 *
 * ★ Why framework depends on platform and not the reverse: the dependency-direction table allows
 *   `ability → platform`; and "ECAM base" is a **target fact** — putting it in framework would
 *   stuff QEMU knowledge into the generic layer. The reverse (platform calling pci-core to
 *   register host ops) would make platform depend on a plugin, which the direction table forbids.
 *   Exit condition (multi-platform / core bus service appears) in ADR-0021 §4.
 *
 * ★ `highmem=off` is a hard prerequisite: with default highmem ECAM sits at `0x40_1000_0000`
 *   (above 4 GiB) and the current MMU has only two segments ("low 1 GiB Device background +
 *   8 MiB RAM window"), so it cannot be expressed; `highmem=off` puts ECAM at `0x3f00_0000`,
 *   inside the low 1 GiB background together with the 32-bit MMIO window ⇒ only a region
 *   declaration is needed (a no-op against the Device background). See ADR-0021 D-2 and gap
 *   analysis §4.4.
 */
#ifndef BR_PLATFORM_BR_PLAT_PCI_H
#define BR_PLATFORM_BR_PLAT_PCI_H

#include <br/core/br_types.h>

/*
 * Static facts of the PCIe host bridge (all values in the little-endian 32-bit PCI view).
 *
 * `intx_virq_a`: virq of INTx **A**; pins B/C/D = +1/+2/+3.
 *   QEMU's GPEX applies the standard swizzle by device slot ⇒ the line actually used =
 *   `intx_virq_a + ((pin_index + device) % 4)` (**computed by pci-core**; platform only gives
 *   the starting number).
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

/* Fetch the PCIe host facts. Uninitialized/PCIe-less boards return a negative errno (-ENODEV).
 * The platform can answer before the EARLY phase (pure data, no MMIO access) ⇒ pci-core's CORE init can read it at any time. */
int br_plat_pci_host(br_plat_pci_host_t *out);

#endif /* BR_PLATFORM_BR_PLAT_PCI_H */
