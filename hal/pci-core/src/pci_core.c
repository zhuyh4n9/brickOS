/*
 * Generic PCI(e) mechanism (hal/pci-core): ECAM config access, bus enumeration, BAR sizing
 * and allocation from the platform MMIO window, INTx swizzle (QEMU GPEX rotates INTx A..D onto SPI
 * 3..6 by (pin_index + slot) % 4). The image boots with -kernel and no firmware, so endpoint BARs
 * come up 0 and unmapped and must be allocated here. The table is filled once in CORE, read-only
 * after that (query surface is lock-free).
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/pci/br_pci.h>
#include <br/platform/br_plat_pci.h>

/* Lifecycle hooks referenced by generated plugin_desc.c (not exported). */
int pci_core_early_init(void);
int pci_core_init(void);
int pci_core_start(void);

static br_plat_pci_host_t s_host;
static br_pci_dev_t       s_devs[BR_PCI_MAX_DEVS];
static br_u32             s_n;
static br_bool            s_inited;

/* ECAM: addr = ecam_base + (bus << 20) + (dev << 15) + (fn << 12) + (off & 0xFFC). It sits in a
 * Device-nGnRnE background mapping, so only 32-bit aligned accesses are allowed. */

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

static br_uintptr_t pci_align_up(br_uintptr_t v, br_uintptr_t a)
{
    return (v + (a - 1u)) & ~(a - 1u);
}

/* Standard BAR probe: write all 1s, read back the writable mask, size = ~mask + 1 (low 4 bits
 * reserved), then restore/assign. A 64-bit BAR is probed together with its high half; a non-zero
 * high half needs > 4 GiB and is skipped. The cursor *next walks the platform MMIO window, each BAR
 * aligned to its size. */
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

        /* orig == 0 is not "BAR absent": an unassigned memory BAR has address 0 (no firmware here),
         * so probe by writing all 1s and reading back; only when neither half accepts a write is
         * the BAR unimplemented. */
        pci_cfg_write32_raw(bus, dev, fn, off, 0xFFFFFFFFu);
        const br_u32 mask_lo = pci_cfg_read32_raw(bus, dev, fn, off) & ~0xFu;

        br_u32 mask_hi = 0u;
        if (is64 == BR_TRUE) {
            pci_cfg_write32_raw(bus, dev, fn, off + 4u, 0xFFFFFFFFu);
            mask_hi = pci_cfg_read32_raw(bus, dev, fn, off + 4u);
        }

        pci_cfg_write32_raw(bus, dev, fn, off, orig);          /* restore */
        if (is64 == BR_TRUE) {
            pci_cfg_write32_raw(bus, dev, fn, off + 4u, 0u);
        }

        if (mask_lo == 0u && mask_hi == 0u) {
            continue;                       /* unimplemented BAR */
        }
        if (is_io == BR_TRUE) {
            if (is64 == BR_TRUE) {
                i++;                        /* consume the high half slot */
            }
            continue;                       /* I/O BAR: this file allocates no I/O space (MEM only) */
        }

        /* Size by BAR width: a 32-bit BAR holds its address bits only in the low half, so
         * ~mask_lo + 1 must be computed in 32 bits (64-bit inversion yields a bogus ~4 GiB);
         * a 64-bit BAR needs the merged mask (the low half alone misjudges a 16 KiB BAR as
         * > 4 GiB). */
        const br_u64 size64 = (is64 == BR_TRUE)
                            ? ((~((((br_u64)mask_hi) << 32) | (br_u64)mask_lo)) + 1u)
                            : ((br_u64)((~mask_lo) + 1u));

        if (size64 == 0u || size64 > (br_u64)s_host.mmio_size) {
            br_log_warn("pci: %02x:%02x.%u BAR%u size 0x%lx does not fit the MMIO window, skipped",
                        bus, dev, fn, i, (unsigned long)size64);
            if (is64 == BR_TRUE) {
                i++;
            }
            continue;
        }

        const br_u32       size = (br_u32)size64;
        const br_uintptr_t base = pci_align_up(*next, (br_uintptr_t)size);

        if ((base + (br_uintptr_t)size) > (s_host.mmio_base + (br_uintptr_t)s_host.mmio_size)) {
            return BR_ERR(ENOSPC);
        }

        pci_cfg_write32_raw(bus, dev, fn, off, (br_u32)base & ~0xFu);
        if (is64 == BR_TRUE) {
            pci_cfg_write32_raw(bus, dev, fn, off + 4u, 0u);   /* high half = 0 (base fits in 32 bits) */
        }
        out->bar[i] = base;
        out->bar_size[i] = size;
        *next = base + (br_uintptr_t)size;

        br_log_debug("pci: %02x:%02x.%u BAR%u -> 0x%lx (size=%u)",
                     bus, dev, fn, i, (unsigned long)base, size);

        if (is64 == BR_TRUE) {
            i++;                            /* a 64-bit BAR occupies two config dwords */
        }
    }
    return 0;
}

static int pci_scan(void)
{
    br_uintptr_t next = s_host.mmio_base;

    for (br_u32 bus = s_host.bus_first; bus <= s_host.bus_last; bus++) {
        for (br_u32 dev = 0u; dev < 32u; dev++) {
            for (br_u32 fn = 0u; fn < 8u; fn++) {
                const br_u16 vendor = pci_cfg_read16_raw(bus, dev, fn, PCI_CFG_VENDOR);

                if (vendor == 0xFFFFu) {
                    if (fn == 0u) {
                        break;              /* function 0 absent => whole slot absent */
                    }
                    continue;
                }
                if (s_n >= (br_u32)BR_PCI_MAX_DEVS) {
                    br_log_warn("pci: device table full (%u), further devices ignored", (unsigned)BR_PCI_MAX_DEVS);
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

                /* INTx: pin (1..4) + slot swizzle => platform virq base + offset */
                const br_u8 pin = pci_cfg_read8_raw(bus, dev, fn, PCI_CFG_INTPIN);
                if (pin >= 1u && pin <= 4u) {
                    d->intx_pin  = pin;
                    d->intx_virq = s_host.intx_virq_a + (((br_u32)pin - 1u + dev) % 4u);
                }

                /* Enable Memory Space + Bus Master, keep INTx enabled (bit10 = 0); the root bridge
                 * (dev 0) keeps its command register untouched (it decodes no allocated BAR). */
                if (dev != 0u) {
                    br_u32 cmd = pci_cfg_read32_raw(bus, dev, fn, PCI_CFG_COMMAND);
                    cmd |= (PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
                    cmd &= ~PCI_CMD_INTX_DIS;
                    pci_cfg_write32_raw(bus, dev, fn, PCI_CFG_COMMAND, cmd);
                }

                s_n++;

                /* Multifunction: function 0 bit7 = 0 => single function, skip 1..7 */
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
        return rc;                          /* platform has no PCIe => -ENODEV, not "0 devices found" */
    }
    if (s_host.ecam_base == 0u || s_host.ecam_size < 0x100000u ||
        s_host.mmio_size == 0u || s_host.intx_virq_a == 0u) {
        return BR_ERR(EINVAL);
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
        return BR_ERR(ENODEV);
    }
    if (out == BR_NULL || (off & 0x3u) != 0u) {
        return BR_ERR(EINVAL);
    }
    *out = pci_cfg_read32_raw(bus, dev, fn, off);
    return 0;
}

int br_pci_cfg_write32(br_u8 bus, br_u8 dev, br_u8 fn, br_u8 off, br_u32 value)
{
    if (s_inited == BR_FALSE) {
        return BR_ERR(ENODEV);
    }
    if ((off & 0x3u) != 0u) {
        return BR_ERR(EINVAL);
    }
    pci_cfg_write32_raw(bus, dev, fn, off, value);
    return 0;
}

int pci_core_early_init(void)
{
    return 0;   /* ECAM is reachable in EARLY; enumeration waits for CORE */
}

/* CORE: enumeration + BAR allocation; failure is a boot failure (no silent downgrade). */
int pci_core_init(void)
{
    const int rc = br_pci_init();
    if (rc != 0) {
        br_log_error("pci: enumeration failed rc=%d", rc);
        return rc;
    }
    return 0;
}

/* START: device summary only (per-device detail is in the CORE-phase debug output). */
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
