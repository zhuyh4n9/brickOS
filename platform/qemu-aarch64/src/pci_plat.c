/*
 * brickOS prototype v0.2.0 — QEMU virt (aarch64) 的 PCIe 平台数据(实现)
 *
 * 见 `include/br/platform/br_plat_pci.h` 的裁定说明与 ADR-0021 D-2。
 *
 * 数值来源(QEMU v10.2.1, `hw/arm/virt.c` 的 memmap + 本地 `-M virt,dumpdtb` 取证):
 *   VIRT_PCIE_ECAM(low, highmem=off) = 0x3f000000 + 0x01000000
 *   VIRT_PCIE_MMIO                   = 0x10000000 + 0x2eff0000
 *   bus-range(highmem=off 实测)      = 0..0x0f
 *   a15irqmap[VIRT_PCIE] = 3  ⇒ INTx A..D = SPI 3..6 = INTID 35..38
 */
#include <br/board_irq.h>
#include <br/core/br_error.h>
#include <br/platform/br_plat_pci.h>

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
