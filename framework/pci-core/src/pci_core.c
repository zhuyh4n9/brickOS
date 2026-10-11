/*
 * brickOS prototype v0.2.0 — 通用 PCI(e) 机制(framework/pci-core)
 *
 * 设计依据: **无设计文档条目**(PCI 是本刀新增的域); 裁定见 `docs/decisions/0021-bdev-pci-emmc.md`
 * D-2(板级事实留 platform、通用机制在本件、`highmem=off`)与 D-4(INTx 走中断)。
 *
 * ## 本文件的四件职责(别的都不做)
 *   ① ECAM 配置空间访问(32 位对齐; 小端; ECAM 落在低 1 GiB 的 **Device** 背景映射里);
 *   ② 总线枚举(bus×dev×fn; vendor == 0xFFFF = 空槽; header-type bit7 判多功能);
 *   ③ BAR 尺寸探测(BAR 写全 1 → 读回取反 + 1)与**从平台 MMIO 窗口分配**,
 *      随后打开端点的 Memory Space + Bus Master 并保持 INTx 使能(命令寄存器 bit10 = 0);
 *   ④ INTx 线的 **swizzle**: QEMU GPEX 按 `(pin_index + slot) % 4` 把 INTx A..D 轮转映射到
 *      SPI 3..6;平台只给"INTx A 的 virq", 具体哪条由本件算(ADR-0021)。
 *
 * ## 为什么 BAR 分配是本件的活(而不是"等固件")
 *   本镜像是 `-kernel` 直启, **没有固件**, PCIe 端点的 BAR 出厂是 0 且未映射(实测
 *   `info pci` 显示 `BAR0: 32 bit memory (not mapped)`)。不分配 BAR 就没有寄存器窗口可写。
 *   这是"补 platform PCIe 枚举"里最实的一块, 不是可省的一步。
 *
 * ## 无锁说明
 *   设备表在 CORE 相的 `br_pci_init` 里一次填好, 之后只读 ⇒ 查询面天然线程安全。
 *   唯一"会写"的是 `br_pci_cfg_write32`(给驱动遍历扩展能力/厂商寄存器用): 调用方自锁。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/pci/br_pci.h>
#include <br/platform/br_plat_pci.h>

/* 生命周期钩子原型(名 = symbol_prefix + 相; 由生成物 `plugin_desc.c` 引用; 不进 [[export]]) */
int pci_core_early_init(void);
int pci_core_init(void);
int pci_core_start(void);

/* ==================================================================== 状态 */

static br_plat_pci_host_t s_host;
static br_pci_dev_t       s_devs[BR_PCI_MAX_DEVS];
static br_u32             s_n;
static br_bool            s_inited;

/* ==================================================================== 配置空间访问
 *
 * ECAM 布局(PCI 规范): addr = ecam + (bus << 20) + (dev << 15) + (fn << 12) + (off & 0xFFC)。
 * ECAM 在 Device-nGnRnE 背景里 ⇒ 只做 32 位对齐访问(`-mstrict-align` 下这更是硬约束)。 */

#define PCI_CFG_VENDOR      0x00u
#define PCI_CFG_COMMAND     0x04u
#define PCI_CFG_REVISION    0x08u
#define PCI_CFG_CLASS       0x0Au
#define PCI_CFG_HEADER_TYPE 0x0Eu
#define PCI_CFG_BAR0        0x10u
#define PCI_CFG_INTPIN      0x3Du

#define PCI_CMD_MEM_SPACE   (1u << 1)
#define PCI_CMD_BUS_MASTER  (1u << 2)
#define PCI_CMD_INTX_DIS    (1u << 10)

static volatile br_u32 *pci_cfg_ptr(br_u32 bus, br_u32 dev, br_u32 fn, br_u32 off)
{
    const br_uintptr_t a = s_host.ecam_base
                         + ((br_uintptr_t)bus << 20)
                         + ((br_uintptr_t)dev << 15)
                         + ((br_uintptr_t)fn << 12)
                         + (br_uintptr_t)(off & 0xFFCu);
    return (volatile br_u32 *)a;
}

static br_u32 pci_cfg_read32_raw(br_u32 bus, br_u32 dev, br_u32 fn, br_u32 off)
{
    return *pci_cfg_ptr(bus, dev, fn, off);
}

static void pci_cfg_write32_raw(br_u32 bus, br_u32 dev, br_u32 fn, br_u32 off, br_u32 v)
{
    *pci_cfg_ptr(bus, dev, fn, off) = v;
}

static br_u16 pci_cfg_read16_raw(br_u32 bus, br_u32 dev, br_u32 fn, br_u32 off)
{
    const br_u32 w = pci_cfg_read32_raw(bus, dev, fn, off & ~3u);
    return (br_u16)((w >> ((off & 2u) * 8u)) & 0xFFFFu);
}

static br_u8 pci_cfg_read8_raw(br_u32 bus, br_u32 dev, br_u32 fn, br_u32 off)
{
    const br_u32 w = pci_cfg_read32_raw(bus, dev, fn, off & ~3u);
    return (br_u8)((w >> ((off & 3u) * 8u)) & 0xFFu);
}

/* ==================================================================== 小工具 */

static br_uintptr_t pci_align_up(br_uintptr_t v, br_uintptr_t a)
{
    return (v + (a - 1u)) & ~(a - 1u);
}

/* ==================================================================== BAR 分配
 *
 * 标准手法:
 *   ① 保存原值; ② 写全 1; ③ 读回 ⇒ 可写位掩码; ④ size = ~mask + 1(低 4 位保留);
 *   ⑤ 把原值/新基址写回。64 位 BAR 连高半一起探; 高半非 0 ⇒ 需要 > 4 GiB ⇒ 本件不支持, 跳过。
 * 分配游标 `*next` 沿平台 MMIO 窗口前进(每个 BAR 按自身尺寸对齐)。
 */
static int pci_assign_bars(br_u32 bus, br_u32 dev, br_u32 fn,
                           br_pci_dev_t *out, br_uintptr_t *next)
{
    for (br_u32 i = 0u; i < BR_PCI_MAX_BARS; i++) {
        const br_u32 off = PCI_CFG_BAR0 + (i * 4u);
        const br_u32  orig  = pci_cfg_read32_raw(bus, dev, fn, off);
        const br_bool is_io = ((orig & 0x1u) != 0u) ? BR_TRUE : BR_FALSE;
        const br_bool is64  = (((orig >> 1) & 0x3u) == 0x2u) ? BR_TRUE : BR_FALSE;

        out->bar[i] = 0u;
        out->bar_size[i] = 0u;

        /* ★ 不能拿 `orig == 0` 当"这个 BAR 不存在": **未分配的内存 BAR 地址就是 0**
         *   (本镜像无固件, 所有端点都处于这个状态)。正确判据是**探测** —— 写全 1 再读回,
         *   两个半字都不接受写入才是"未实现"。实测踩到过: 用 orig==0 判空 ⇒ sdhci 的
         *   BAR0 永远分不到地址, TC-PCI-003 如实报红。 */
        pci_cfg_write32_raw(bus, dev, fn, off, 0xFFFFFFFFu);
        const br_u32 mask_lo = pci_cfg_read32_raw(bus, dev, fn, off) & ~0xFu;

        br_u32 mask_hi = 0u;
        if (is64 == BR_TRUE) {
            pci_cfg_write32_raw(bus, dev, fn, off + 4u, 0xFFFFFFFFu);
            mask_hi = pci_cfg_read32_raw(bus, dev, fn, off + 4u);
        }

        pci_cfg_write32_raw(bus, dev, fn, off, orig);          /* 复原 */
        if (is64 == BR_TRUE) {
            pci_cfg_write32_raw(bus, dev, fn, off + 4u, 0u);
        }

        if (mask_lo == 0u && mask_hi == 0u) {
            continue;                       /* 未实现的 BAR */
        }
        if (is_io == BR_TRUE) {
            if (is64 == BR_TRUE) {
                i++;                        /* 吃掉高半格 */
            }
            continue;                       /* I/O BAR: 本件不分配 I/O 空间(只做 MEM) */
        }

        /* 尺寸按 **BAR 位宽**算:
         *   - 32 位 BAR: 只有低半是它的地址位 ⇒ `~mask_lo + 1` 必须在 **32 位**里取反
         *     (拿 64 位取反会把高 32 位当成"不可写"而算出一个 ~4 GiB 的假尺寸 —— 实测踩到);
         *   - 64 位 BAR: 合并掩码才决定尺寸(只测低半会把 16 KiB 的 BAR 误判成 > 4 GiB)。 */
        const br_u64 size64 = (is64 == BR_TRUE)
                            ? ((~((((br_u64)mask_hi) << 32) | (br_u64)mask_lo)) + 1u)
                            : ((br_u64)((~mask_lo) + 1u));

        if (size64 == 0u || size64 > (br_u64)s_host.mmio_size) {
            br_log_warn("pci: %02x:%02x.%u BAR%u 尺寸 0x%lx 不可放入 MMIO 窗口, 跳过",
                        bus, dev, fn, i, (unsigned long)size64);
            if (is64 == BR_TRUE) {
                i++;
            }
            continue;
        }

        const br_u32       size = (br_u32)size64;
        const br_uintptr_t base = pci_align_up(*next, (br_uintptr_t)size);

        if ((base + (br_uintptr_t)size) > (s_host.mmio_base + (br_uintptr_t)s_host.mmio_size)) {
            return BR_ERR(BR_ENOSPC);
        }

        pci_cfg_write32_raw(bus, dev, fn, off, (br_u32)base & ~0xFu);
        if (is64 == BR_TRUE) {
            pci_cfg_write32_raw(bus, dev, fn, off + 4u, 0u);   /* 高半 = 0(基址在 32 位内) */
        }
        out->bar[i] = base;
        out->bar_size[i] = size;
        *next = base + (br_uintptr_t)size;

        br_log_debug("pci: %02x:%02x.%u BAR%u -> 0x%lx (size=%u)",
                     bus, dev, fn, i, (unsigned long)base, size);

        if (is64 == BR_TRUE) {
            i++;                            /* 64 位 BAR 占两个配置双字 */
        }
    }
    return 0;
}

/* ==================================================================== 枚举 */

static int pci_scan(void)
{
    br_uintptr_t next = s_host.mmio_base;

    for (br_u32 bus = s_host.bus_first; bus <= s_host.bus_last; bus++) {
        for (br_u32 dev = 0u; dev < 32u; dev++) {
            for (br_u32 fn = 0u; fn < 8u; fn++) {
                const br_u16 vendor = pci_cfg_read16_raw(bus, dev, fn, PCI_CFG_VENDOR);

                if (vendor == 0xFFFFu) {
                    if (fn == 0u) {
                        break;              /* 功能 0 不在 ⇒ 整槽不在 */
                    }
                    continue;
                }
                if (s_n >= (br_u32)BR_PCI_MAX_DEVS) {
                    br_log_warn("pci: 设备表满(%u), 后面的设备被忽略", (unsigned)BR_PCI_MAX_DEVS);
                    return 0;
                }

                br_pci_dev_t *d = &s_devs[s_n];
                d->vendor     = vendor;
                d->device     = pci_cfg_read16_raw(bus, dev, fn, PCI_CFG_VENDOR + 2u);
                d->class_code = pci_cfg_read16_raw(bus, dev, fn, PCI_CFG_CLASS);
                d->revision   = pci_cfg_read8_raw(bus, dev, fn, PCI_CFG_REVISION);
                d->bus        = (br_u8)bus;
                d->dev        = (br_u8)dev;
                d->fn         = (br_u8)fn;
                d->intx_pin   = 0u;
                d->intx_virq  = 0u;

                const int rc = pci_assign_bars(bus, dev, fn, d, &next);
                if (rc != 0) {
                    return rc;
                }

                /* INTx: pin(1..4) + 槽号 swizzle ⇒ 平台的 virq 起点 + 偏移 */
                const br_u8 pin = pci_cfg_read8_raw(bus, dev, fn, PCI_CFG_INTPIN);
                if (pin >= 1u && pin <= 4u) {
                    d->intx_pin  = pin;
                    d->intx_virq = s_host.intx_virq_a + (((br_u32)pin - 1u + dev) % 4u);
                }

                /* 打开端点的内存空间 + Bus Master; 保持 INTx 使能(bit10 = 0)。
                 * 根桥(dev 0)不动它的命令寄存器 —— 它不解码任何我们分配的 BAR。 */
                if (dev != 0u) {
                    br_u32 cmd = pci_cfg_read32_raw(bus, dev, fn, PCI_CFG_COMMAND);
                    cmd |= (PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
                    cmd &= ~PCI_CMD_INTX_DIS;
                    pci_cfg_write32_raw(bus, dev, fn, PCI_CFG_COMMAND, cmd);
                }

                s_n++;

                /* 多功能判定: 功能 0 的 header-type bit7 = 0 ⇒ 单功能, 不再扫 1..7 */
                if (fn == 0u && (pci_cfg_read8_raw(bus, dev, fn, PCI_CFG_HEADER_TYPE) & 0x80u) == 0u) {
                    break;
                }
            }
        }
    }
    return 0;
}

int br_pci_init(void)
{
    if (s_inited == BR_TRUE) {
        return 0;
    }

    const int rc = br_plat_pci_host(&s_host);
    if (rc != 0) {
        return rc;                          /* 平台无 PCIe ⇒ -ENODEV, 不假装枚举出 0 个设备 */
    }
    if (s_host.ecam_base == 0u || s_host.ecam_size < 0x100000u ||
        s_host.mmio_size == 0u || s_host.intx_virq_a == 0u) {
        return BR_ERR(BR_EINVAL);
    }

    s_n = 0u;
    const int src = pci_scan();
    if (src != 0) {
        return src;
    }

    s_inited = BR_TRUE;
    br_log_info("pci: %u function(s) enumerated (ecam=0x%lx+0x%lx, mmio=0x%lx+0x%lx)",
                (unsigned)s_n,
                (unsigned long)s_host.ecam_base, (unsigned long)s_host.ecam_size,
                (unsigned long)s_host.mmio_base, (unsigned long)s_host.mmio_size);
    return 0;
}

/* ==================================================================== 查询面 */

br_u32 br_pci_dev_count(void)
{
    return s_n;
}

const br_pci_dev_t *br_pci_dev_at(br_u32 index)
{
    return (index < s_n) ? &s_devs[index] : BR_NULL;
}

const br_pci_dev_t *br_pci_find(br_u16 vendor, br_u16 device)
{
    for (br_u32 i = 0u; i < s_n; i++) {
        if (s_devs[i].vendor == vendor && s_devs[i].device == device) {
            return &s_devs[i];
        }
    }
    return BR_NULL;
}

int br_pci_cfg_read32(br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 *out)
{
    if (s_inited == BR_FALSE) {
        return BR_ERR(BR_ENODEV);
    }
    if (out == BR_NULL || (off & 0x3u) != 0u) {
        return BR_ERR(BR_EINVAL);
    }
    *out = pci_cfg_read32_raw(bus, dev, fn, off);
    return 0;
}

int br_pci_cfg_write32(br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 value)
{
    if (s_inited == BR_FALSE) {
        return BR_ERR(BR_ENODEV);
    }
    if ((off & 0x3u) != 0u) {
        return BR_ERR(BR_EINVAL);
    }
    pci_cfg_write32_raw(bus, dev, fn, off, value);
    return 0;
}

/* ==================================================================== 生命周期 */

int pci_core_early_init(void)
{
    return 0;   /* ECAM 在低 1 GiB 的 Device 背景里, EARLY 相即可访问; 但枚举留 CORE 相 */
}

/* CORE 相: 枚举 + BAR 分配。失败 = 启动失败(不静默降级 —— 与 platform 的 IRQ 初始化同纪律)。 */
int pci_core_init(void)
{
    const int rc = br_pci_init();
    if (rc != 0) {
        br_log_error("pci: 枚举失败 rc=%d", rc);
        return rc;
    }
    return 0;
}

/* START 相: 只留一条设备摘要(逐条设备明细在 CORE 相的 debug 里)。 */
int pci_core_start(void)
{
    for (br_u32 i = 0u; i < s_n; i++) {
        const br_pci_dev_t *d = &s_devs[i];
        br_log_info("pci: %02x:%02x.%u %04x:%04x class=%04x bar0=0x%lx intx=%u",
                    d->bus, d->dev, d->fn, d->vendor, d->device, d->class_code,
                    (unsigned long)d->bar[0], (unsigned)d->intx_virq);
    }
    return 0;
}
