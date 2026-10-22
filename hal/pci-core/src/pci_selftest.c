/*
 * Selftest suite of hal/pci-core: observable facts of enumeration and BAR allocation
 * (TC-PCI-*): table bounds, the 1b36:0007 target endpoint, BAR0 allocated inside the platform MMIO
 * window, command register (Memory Space|Bus Master, INTx enabled), INTx swizzle, and the public
 * config-space port. Returns the number of failures; logs [PCICONF] PASS/FAIL.
 */
#include <br/pci/br_pci.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/platform/br_plat_pci.h>

/* Referenced by the generated plugin_desc.c .selftest hook. */
int pci_core_selftest(void);

/* QEMU -device sdhci-pci identity (1b36:0007, class 0x0805): the target under test, so the case
 * turns red if enumeration regresses. */
#define PCI_CONF_SDHCI_VENDOR  0x1b36u
#define PCI_CONF_SDHCI_DEVICE  0x0007u

static br_u32 s_pc_pass;
static br_u32 s_pc_fail;

static void pc_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pc_pass++;
        br_log_info("[PCICONF] PASS %s %s", tag, what);
    } else {
        s_pc_fail++;
        br_log_info("[PCICONF] FAIL %s %s", tag, what);
    }
}

int pci_core_selftest(void)
{
    s_pc_pass = 0u;
    s_pc_fail = 0u;

    br_log_info("[PCICONF] pci-core conformance (enumerate/BAR/command/INTx)");

    const br_u32 n = br_pci_dev_count();
    const br_pci_dev_t *sdhci = br_pci_find(PCI_CONF_SDHCI_VENDOR, PCI_CONF_SDHCI_DEVICE);

    /* TC-PCI-001: device table non-empty and within bounds. */
    pc_conf((n > 0u) && (n <= (br_u32)BR_PCI_MAX_DEVS), "TC-PCI-001",
            "enumerated >= 1 function and <= BR_PCI_MAX_DEVS");

    /* TC-PCI-002: target endpoint found and class correct. */
    pc_conf((sdhci != BR_NULL) && (sdhci->class_code == (br_u16)BR_PCI_CLASS_SDHCI),
            "TC-PCI-002", "br_pci_find(1b36:0007) hit and class = 0x0805 (System SDHCI)");

    /* TC-PCI-003: BAR0 allocated, inside the platform MMIO window, size-aligned. */
    {
        br_bool ok = BR_FALSE;
        if (sdhci != BR_NULL && sdhci->bar[0] != 0u && sdhci->bar_size[0] >= 256u) {
            br_plat_pci_host_t h;
            if (br_plat_pci_host(&h) == 0) {
                const br_uintptr_t b = sdhci->bar[0];
                const br_u32       sz = sdhci->bar_size[0];
                ok = (b >= h.mmio_base) && ((b + sz) <= (h.mmio_base + h.mmio_size)) &&
                     ((b % sz) == 0u);
            }
        }
        pc_conf(ok, "TC-PCI-003",
                "sdhci BAR0 non-zero, inside the mmio window, size-aligned");
    }

    /* TC-PCI-004: command register = Memory Space + Bus Master, INTx not disabled. */
    {
        br_bool ok = BR_FALSE;
        if (sdhci != BR_NULL) {
            br_u32 cmd = 0u;
            if (br_pci_cfg_read32(sdhci->bus, sdhci->dev, sdhci->fn, 0x04u, &cmd) == 0) {
                ok = ((cmd & 0x2u) != 0u) && ((cmd & 0x4u) != 0u) && ((cmd & 0x400u) == 0u);
            }
        }
        pc_conf(ok, "TC-PCI-004", "command register: Memory Space|Bus Master set, INTx Disable clear");
    }

    /* TC-PCI-005: INTx swizzle matches the platform fact (pin A => virq_a + (slot % 4)). */
    {
        br_bool ok = BR_FALSE;
        if (sdhci != BR_NULL && sdhci->intx_pin == 1u) {
            br_plat_pci_host_t h;
            if (br_plat_pci_host(&h) == 0) {
                ok = (sdhci->intx_virq == (h.intx_virq_a + ((br_u32)sdhci->dev % 4u)));
            }
        }
        pc_conf(ok, "TC-PCI-005", "pin A virq == platform intx_virq_a + (slot % 4)");
    }

    /* TC-PCI-006: public config-space port: round trip + argument validation. */
    {
        br_bool ok = BR_FALSE;
        if (sdhci != BR_NULL) {
            br_u32 v = 0u;
            const int r_ok  = br_pci_cfg_read32(sdhci->bus, sdhci->dev, sdhci->fn, 0x00u, &v);
            const int r_bad = br_pci_cfg_read32(sdhci->bus, sdhci->dev, sdhci->fn, 0x02u, &v);
            const int r_nul = br_pci_cfg_read32(sdhci->bus, sdhci->dev, sdhci->fn, 0x00u, BR_NULL);
            ok = (r_ok == 0) && (v == ((br_u32)PCI_CONF_SDHCI_DEVICE << 16 | PCI_CONF_SDHCI_VENDOR)) &&
                 (r_bad == BR_ERR(EINVAL)) && (r_nul == BR_ERR(EINVAL));
        }
        pc_conf(ok, "TC-PCI-006",
                "cfg_read32 vendor round trip correct; misaligned/null out-param ⇒ -EINVAL");
    }

    /* TC-PCI-007: miss and out of range. */
    {
        const br_bool ok = (br_pci_find(0xFFFFu, 0xFFFFu) == BR_NULL) &&
                           (br_pci_dev_at(n) == BR_NULL) &&
                           (br_pci_dev_at(n + 1u) == BR_NULL);
        pc_conf(ok, "TC-PCI-007", "find(nonexistent) and dev_at(out of range) ⇒ NULL");
    }

    /* TC-PCI-008: BAR allocations do not overlap. */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 i = 0u; i < n && ok == BR_TRUE; i++) {
            const br_pci_dev_t *a = br_pci_dev_at(i);
            for (br_u32 j = 0u; j < BR_PCI_MAX_BARS; j++) {
                if (a->bar[j] == 0u) {
                    continue;
                }
                for (br_u32 k = 0u; k < n; k++) {
                    const br_pci_dev_t *b = br_pci_dev_at(k);
                    for (br_u32 l = 0u; l < BR_PCI_MAX_BARS; l++) {
                        if (b->bar[l] == 0u || (k == i && l == j)) {
                            continue;
                        }
                        if ((a->bar[j] < (b->bar[l] + b->bar_size[l])) &&
                            (b->bar[l] < (a->bar[j] + a->bar_size[j]))) {
                            ok = BR_FALSE;
                        }
                    }
                }
            }
        }
        pc_conf(ok, "TC-PCI-008", "all allocated BAR address ranges are pairwise non-overlapping");
    }

    br_log_info("[PCICONF] SUMMARY pass=%u fail=%u total=%u",
                s_pc_pass, s_pc_fail, s_pc_pass + s_pc_fail);
    return (int)s_pc_fail;
}
