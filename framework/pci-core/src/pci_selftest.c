/*
 * brickOS prototype v0.2.0 — framework/pci-core 的**自检套件**(src/pci_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0021-bdev-pci-emmc.md`(D-2 PCIe 归属与 highmem=off;
 * D-4 INTx 走中断)。
 *
 * ## 它测什么
 *   枚举结果与分配结果的**可观测事实**(自编号 `TC-PCI-*`):
 *     - 设备表非空且不越上界;
 *     - 目标端点 `1b36:0007`(QEMU 的 sdhci-pci, class 0x0805)被找到;
 *     - 它的 **BAR0 真的被分配**(非 0、在平台 MMIO 窗口内、按尺寸对齐)⇒ 后面驱动能写寄存器;
 *     - 端点的**命令寄存器**开了 Memory Space + Bus Master 且 INTx 未被禁用 ⇒ DMA 与中断可用;
 *     - **INTx 线的 swizzle** 与平台事实一致(`(pin_index + slot) % 4`);
 *     - 公共配置空间读写口的往返与参数校验(错对齐 ⇒ -EINVAL)。
 *
 * ## 为什么这些值得在真机上判(而不是只在宿主)
 *   BAR 探测/分配是**写硬件再读回**的动作: 写错一位(比如把 ROM 挂钩当 BAR)会静默改坏
 *   别的功能。真 QEMU 上的往返是唯一能证明"分配没把别人踩掉"的判据。宿主测不了 ECAM。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[PCICONF] PASS/FAIL ...` + `SUMMARY`)。
 *   失败不停机 —— 红绿由门禁判(forbid 里有 `[PCICONF] FAIL`)。
 */
#include <br/pci/br_pci.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/platform/br_plat_pci.h>

/* 自检入口原型(定义在本文件末尾; 生成物 `plugin_desc.c` 的 `.selftest` 钩子引用它)。 */
int pci_core_selftest(void);

/* QEMU `-device sdhci-pci` 的身份(实测 `info pci`: 1b36:0007, class 2053 = 0x0805)。
 * 这两个数是**被测目标**的标识,不是"内部机制" —— 写在用例里就是要它红。 */
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

    /* ---- TC-PCI-001: 设备表非空且不越上界 ---- */
    pc_conf((n > 0u) && (n <= (br_u32)BR_PCI_MAX_DEVS), "TC-PCI-001",
            "枚举到 >= 1 个功能且 <= BR_PCI_MAX_DEVS");

    /* ---- TC-PCI-002: 目标端点被发现且 class 正确 ---- */
    pc_conf((sdhci != BR_NULL) && (sdhci->class_code == (br_u16)BR_PCI_CLASS_SDHCI),
            "TC-PCI-002", "br_pci_find(1b36:0007) 命中且 class = 0x0805 (System SDHCI)");

    /* ---- TC-PCI-003: BAR0 已分配且在平台 MMIO 窗口内、按尺寸对齐 ---- */
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
                "sdhci BAR0 非 0、落在 mmio 窗口内、按尺寸对齐");
    }

    /* ---- TC-PCI-004: 命令寄存器 = Memory Space + Bus Master, 且 INTx 未禁用 ---- */
    {
        br_bool ok = BR_FALSE;
        if (sdhci != BR_NULL) {
            br_u32 cmd = 0u;
            if (br_pci_cfg_read32(sdhci->bus, sdhci->dev, sdhci->fn, 0x04u, &cmd) == 0) {
                ok = ((cmd & 0x2u) != 0u) && ((cmd & 0x4u) != 0u) && ((cmd & 0x400u) == 0u);
            }
        }
        pc_conf(ok, "TC-PCI-004", "命令寄存器: Memory Space|Bus Master 置位, INTx Disable 清位");
    }

    /* ---- TC-PCI-005: INTx swizzle 与平台事实一致(pin A ⇒ virq_a + (slot % 4)) ---- */
    {
        br_bool ok = BR_FALSE;
        if (sdhci != BR_NULL && sdhci->intx_pin == 1u) {
            br_plat_pci_host_t h;
            if (br_plat_pci_host(&h) == 0) {
                ok = (sdhci->intx_virq == (h.intx_virq_a + ((br_u32)sdhci->dev % 4u)));
            }
        }
        pc_conf(ok, "TC-PCI-005", "pin A 的 virq == 平台 intx_virq_a + (slot % 4)");
    }

    /* ---- TC-PCI-006: 公共配置空间口: 往返 + 参数校验 ---- */
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
                "cfg_read32 vendor 往返正确; 错对齐/空出参 ⇒ -EINVAL");
    }

    /* ---- TC-PCI-007: 未命中与越界 ---- */
    {
        const br_bool ok = (br_pci_find(0xFFFFu, 0xFFFFu) == BR_NULL) &&
                           (br_pci_dev_at(n) == BR_NULL) &&
                           (br_pci_dev_at(n + 1u) == BR_NULL);
        pc_conf(ok, "TC-PCI-007", "find(不存在) 与 dev_at(越界) ⇒ NULL");
    }

    /* ---- TC-PCI-008: BAR 分配互不重叠 ---- */
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
        pc_conf(ok, "TC-PCI-008", "所有已分配 BAR 的地址区间两两不重叠");
    }

    br_log_info("[PCICONF] SUMMARY pass=%u fail=%u total=%u",
                s_pc_pass, s_pc_fail, s_pc_pass + s_pc_fail);
    return (int)s_pc_fail;
}
