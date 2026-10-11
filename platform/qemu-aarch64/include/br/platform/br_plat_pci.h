/*
 * brickOS prototype v0.2.0 — QEMU virt (aarch64) 的 **PCIe 平台事实**(platform 导出)
 *
 * 设计依据: 无设计文档条目(PCI 是本刀新增的域, 见 `docs/decisions/0021-bdev-pci-emmc.md`
 * D-2)。归属裁定: **板级事实留在 platform**(ECAM 在哪、MMIO 窗口在哪、INTx 起始线是哪条),
 * 通用 PCI 机制(ECAM 访问/枚举/BAR 分配/设备表)在 `framework/pci-core`。
 *
 * ★ 为什么 framework 依赖 platform 而不是反过来: 依赖方向表允许 `ability → platform`;
 *   而"ECAM 基址"是**目标事实** —— 放进 framework 就是把 QEMU 知识塞进通用层。反过来
 *   (platform 调 pci-core 注册 host ops)会让 platform 依赖插件, 方向表不允许。
 *   退出条件(多平台 / core 的 bus 服务出现)见 ADR-0021 §4。
 *
 * ★ `highmem=off` 是硬前提: 默认 highmem 下 ECAM 在 `0x40_1000_0000`(超 4 GiB),
 *   当前 MMU 只有"低 1 GiB Device 背景 + 8 MiB RAM 窗口"两段, 表达不了;`highmem=off`
 *   把 ECAM 落到 `0x3f00_0000`, 与 32 位 MMIO 窗口一起落在低 1 GiB 背景里 ⇒ 只需 region
 *   声明(对 Device 背景是 no-op)。见 ADR-0021 D-2 与 gap 分析 §4.4。
 */
#ifndef BR_PLATFORM_BR_PLAT_PCI_H
#define BR_PLATFORM_BR_PLAT_PCI_H

#include <br/core/br_types.h>

/*
 * PCIe 主机桥的静态事实(全部是小端 32 位 PCI 视图下的数值)。
 *
 * `intx_virq_a`: INTx **A** 的 virq;pin B/C/D = +1/+2/+3。
 *   QEMU 的 GPEX 按设备槽号做标准 swizzle ⇒ 实际用哪条线 = `intx_virq_a +
 *   ((pin_index + device) % 4)`(**由 pci-core 计算**, platform 只给起始号)。
 */
typedef struct br_plat_pci_host {
    br_uintptr_t ecam_base;     /* ECAM(配置空间)基址 */
    br_size_t    ecam_size;
    br_uintptr_t mmio_base;     /* 32 位 MMIO 窗口基址(BAR 从这里分配) */
    br_size_t    mmio_size;
    br_u32       bus_first;     /* 总线号范围(含) */
    br_u32       bus_last;      /* 含 */
    br_u32       intx_virq_a;   /* INTx A 的 virq(BR_IRQ_PCIE_INTX_A) */
} br_plat_pci_host_t;

/* 取 PCIe 主机事实。未初始化/无 PCIe 的板子返回负 errno(-ENODEV)。
 * 平台在 EARLY 相之前即可回答(纯数据, 无 MMIO 访问)⇒ pci-core 的 CORE init 随时可读。 */
int br_plat_pci_host(br_plat_pci_host_t *out);

#endif /* BR_PLATFORM_BR_PLAT_PCI_H */
