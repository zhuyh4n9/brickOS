/*
 * brickOS prototype v0.2.0 — 通用 PCI(e) 机制契约(framework/pci-core 的对外面)
 *
 * 设计依据: **无设计文档条目** —— PCI 是本刀(ADR-0021 D-2)新增的域。归属裁定的要点:
 *   - 板级事实(ECAM 在哪 / MMIO 窗口在哪 / INTx 起始线)留 `platform/qemu-aarch64`
 *     (`br_plat_pci_host`); 通用机制在**本件**;
 *   - 本件以 `init` 依赖 platform 读那几件事实(依赖方向表允许 `ability → platform`);
 *     退出条件(多平台 / core 的 bus 服务出现)见 ADR-0021 §4。
 *
 * ## 本件做的三件(R-S4 的防线)
 *   ① **ECAM 配置空间访问**(32 位; 小端; 只做对齐访问 —— ECAM 在 Device 背景映射里);
 *   ② **总线枚举**(bus×dev×fn 扫描 bus 0 起, 匹配 vendor≠0xFFFF);
 *   ③ **BAR 尺寸探测与分配**(标准手法: 写全 1 读回算尺寸 → 从平台 MMIO 窗口切 → 写回),
 *      并打开端点的 **Memory Space + Bus Master**、保持 **INTx 使能**(命令寄存器 bit10=0)。
 *   另加一张静态设备表 + `br_pci_find`(消费者按 vendor/device 取设备),
 *   以及 INTx 线的 **swizzle 计算**(QEMU GPEX: `(pin_index + slot) % 4`, ADR-0021)。
 *
 * ## 本件**不**做什么
 *   - 不认任何具体器件(sdhci-pci 等): 那是消费本件的驱动;
 *   - 不做 MSI/MSI-X(端点用 INTx; 见 ADR-0021 D-4)、不做 PCIe 链路训练/电源管理;
 *   - 不做桥后面的总线号分配(v1 只面对 root bus 上的端点; 见 `br_pci_find` 的注释)。
 *
 * ## 上下文
 *   `br_pci_init` 在 CORE 相跑(plugin init), 之后设备表只读; 消费者(驱动)的 init 以
 *   `init` 边依赖本件 ⇒ 拓扑序保证"表已就绪"。查询面可在任意线程调用(无锁: 表只读)。
 */
#ifndef BR_PCI_BR_PCI_H
#define BR_PCI_BR_PCI_H

#include <br/core/br_types.h>

/* 静态上界(原型口径: 无动态分配, 与 dev-core/cdev-core 同款"够用即停")。 */
#define BR_PCI_MAX_DEVS  8u
#define BR_PCI_MAX_BARS  6u

/* PCI 配置空间里的 class code(高位 class, 低位 subclass); System SDHCI = 0x0805。 */
#define BR_PCI_CLASS_SDHCI  0x0805u

/*
 * 一条已枚举(且 BAR 已分配)的 PCI 功能。
 * `bar[i] == 0` 表示该 BAR 不存在 / 未分配(不是"基址 0"—— 0 是保留的无效地址)。
 */
typedef struct br_pci_dev {
    br_u16       vendor;
    br_u16       device;
    br_u16       class_code;    /* class<<8 | subclass */
    br_u8        revision;
    br_u8        bus;
    br_u8        dev;           /* 设备号(0..31); INTx swizzle 用它 */
    br_u8        fn;
    br_u8        intx_pin;      /* 配置空间 0x3D: 1..4 = INTx A..D; 0 = 无 */
    br_u8        _pad[3];
    br_u32       intx_virq;     /* 平台绑定好的 virq; 0 = 无 INTx */
    br_uintptr_t bar[BR_PCI_MAX_BARS];
    br_u32       bar_size[BR_PCI_MAX_BARS];
} br_pci_dev_t;

/*
 * 枚举 + BAR 分配(幂等: 重复调用返回首次的结果)。
 * 失败 = 负 errno(平台无 PCIe ⇒ -ENODEV; 窗口不够放 BAR ⇒ -ENOSPC)。
 * 由本件的 CORE init 调用; 消费者通常不需要显式调它。
 */
int br_pci_init(void);

/* 设备表查询(只读; 未 init 或未命中 ⇒ 0/NULL)。 */
br_u32               br_pci_dev_count(void);
const br_pci_dev_t  *br_pci_dev_at(br_u32 index);
const br_pci_dev_t  *br_pci_find(br_u16 vendor, br_u16 device);

/* 原始配置空间访问(32 位对齐)。给驱动的"厂商私有寄存器/扩展能力遍历"留的口子。
 * `off` 低 2 位被忽略(向下对齐到 4)。返回 0 或负 errno。 */
int br_pci_cfg_read32 (br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 *out);
int br_pci_cfg_write32(br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 value);

#endif /* BR_PCI_BR_PCI_H */
