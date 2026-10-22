/*
 * QEMU virt (aarch64) PCIe platform data: ECAM/MMIO windows, bus range, INTx A virq.
 * Values measured from QEMU v10.2.1 `-M virt,dumpdtb` with highmem=off.
 * a15irqmap[VIRT_PCIE] = 3 => INTx A..D = SPI 3..6 = INTID 35..38.
 */
#include <br/board_irq.h>
#include <br/core/br_error.h>
#include <br/runtime/br_plat_pci.h>

int br_plat_pci_host(br_plat_pci_host_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    out->ecam_base   = 0x3f000000ul;
    out->ecam_size   = 0x01000000ul;
    out->mmio_base   = 0x10000000ul;
    out->mmio_size   = 0x2eff0000ul;
    out->bus_first   = 0u;
    out->bus_last    = 0x0fu;
    out->intx_virq_a = BR_IRQ_PCIE_INTX_A;
    return 0;
}
