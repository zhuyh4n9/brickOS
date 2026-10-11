/*
 * third_party/uboot-mmc — bdev 对接 + PCIe 绑定 + 生命周期钩子
 *
 * 设计依据: `docs/7-storage/7-02-bdev.md` §1/§4(形状与"DMA/PIO 要能指出")、
 * `docs/decisions/0021-bdev-pci-emmc.md` D-3(隔离)/D-4(中断 + ADMA2/PIO)/D-5(flush 口径)。
 *
 * ## 为什么硬件初始化在 **start** 相而不是 core 相
 *   插件管理器的启动链是 `EARLY/CORE/LATE → 全局开中断 → START`(见 plugin_mgr 的文件头)。
 *   本驱动用**中断**等命令/数据完成 ⇒ CORE 相里全局中断还没开, ISR/bh 不会跑, 等下去必超时。
 *   所以: `early_init`/`init` 都空转, 真正的"探测 + 识别 + 注册 bdev"落在 `start`
 *   —— 那时中断已开, 且 platform 的 timer 也已就位(它是本件的传递依赖)。
 *
 * ## 设备名与 bdev 语义
 *   `emmc0`(dev-core 命名规则 [a-z][a-z0-9]*); `geometry` 的 DMA/PIO 位**每次调用现算**
 *   ⇒ 运行期 `SET_PATH` 之后, 下一次几何查询就看到新通路(ADR-0021 D-4)。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_bdev.h>
#include <br/dev/br_dev.h>
#include <br/pci/br_pci.h>

#include "emmc_internal.h"

/* 生命周期钩子原型(名 = symbol_prefix + 相; 生成物 `plugin_desc.c` 引用) */
int uboot_mmc_early_init(void);
int uboot_mmc_init(void);
int uboot_mmc_start(void);

/* QEMU `-device sdhci-pci` 的身份(与 framework/pci-core 的自检同源; 实测取得) */
#define BR_EMMC_PCI_VENDOR  0x1b36u
#define BR_EMMC_PCI_DEVICE  0x0007u

static br_mmc_t       s_mmc;
static br_bdev_geom_t s_geom;       /* n_sectors/max_rw 等在 start 里填; flags 每次现算 */
static br_uintptr_t   s_base;       /* SDHCI 寄存器窗口(BAR0), 供 GET_INFO 回答 */

/* ==================================================================== bdev ops */

static int emmc_read(void *priv, br_u64 lba, void *buf, br_size_t n_lba)
{
    return br_mmc_read((br_mmc_t *)priv, lba, buf, n_lba);
}

static int emmc_write(void *priv, br_u64 lba, const void *buf, br_size_t n_lba)
{
    return br_mmc_write((br_mmc_t *)priv, lba, buf, n_lba);
}

static int emmc_flush(void *priv)
{
    return br_mmc_flush((br_mmc_t *)priv);
}

static int emmc_geometry(void *priv, br_bdev_geom_t *g)
{
    (void)priv;
    if (g == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    br_bdev_geom_t x = s_geom;
    x.flags = (br_sdhci_get_path() == BR_MMC_PATH_PIO) ? BR_BDEV_F_PIO : BR_BDEV_F_DMA;
    *g = x;
    return 0;
}

static int emmc_ioctl(void *priv, br_u32 cmd, void *arg)
{
    (void)priv;

    if (BR_IOC_TYPE(cmd) != (br_u32)BR_EMMC_IOC_MAGIC) {
        return BR_ERR(BR_ENOTSUP);
    }

    switch (BR_IOC_NR(cmd)) {
    case BR_EMMC_IOC_GET_INFO: {
        if (arg == BR_NULL) {
            return BR_ERR(BR_EINVAL);
        }
        br_emmc_info_t *info = (br_emmc_info_t *)arg;
        info->base             = s_base;
        info->capacity_sectors = s_mmc.capacity_sectors;
        info->sector_size      = s_geom.sector_size;
        info->max_rw           = s_geom.max_rw;
        info->bus_width        = s_mmc.bus_width;
        info->clock_hz         = s_mmc.clock;
        info->path             = br_sdhci_get_path();
        info->card_type        = s_mmc.card_type;
        return 0;
    }
    case BR_EMMC_IOC_SET_PATH: {
        if (arg == BR_NULL) {
            return BR_ERR(BR_EINVAL);
        }
        const br_u32 path = *(const br_u32 *)arg;
        if (path != BR_MMC_PATH_DMA && path != BR_MMC_PATH_PIO) {
            return BR_ERR(BR_EINVAL);
        }
        br_sdhci_set_path(path);
        br_log_info("uboot-mmc: 数据通路切到 %s", (path == BR_MMC_PATH_PIO) ? "PIO" : "ADMA2");
        return 0;
    }
    default:
        return BR_ERR(BR_ENOTSUP);
    }
}

static int emmc_suspend(void *priv) { (void)priv; return BR_ERR(BR_ENOTSUP); }   /* D22 预留 */
static int emmc_resume (void *priv) { (void)priv; return BR_ERR(BR_ENOTSUP); }

static const br_bdev_ops_t s_emmc_ops = {
    .read     = emmc_read,
    .write    = emmc_write,
    .flush    = emmc_flush,
    .geometry = emmc_geometry,
    .ioctl    = emmc_ioctl,
    .suspend  = emmc_suspend,
    .resume   = emmc_resume,
};

/* ==================================================================== 生命周期 */

int uboot_mmc_early_init(void)
{
    return 0;   /* 此刻无关中断/无堆; 且寄存器窗口要等 pci-core 的 CORE 枚举 */
}

int uboot_mmc_init(void)
{
    return 0;   /* 见文件头: 真正的初始化在 start(全局中断已开) */
}

int uboot_mmc_start(void)
{
    const br_pci_dev_t *d = br_pci_find(BR_EMMC_PCI_VENDOR, BR_EMMC_PCI_DEVICE);
    if (d == BR_NULL) {
        br_log_error("uboot-mmc: 组合里有 pci-core, 但没找到 sdhci-pci (%04x:%04x)",
                     (unsigned)BR_EMMC_PCI_VENDOR, (unsigned)BR_EMMC_PCI_DEVICE);
        return BR_ERR(BR_ENODEV);
    }
    if (d->bar[0] == 0u || d->bar_size[0] < 0x100u) {
        br_log_error("uboot-mmc: sdhci BAR0 未分配或过小(bar0=0x%lx size=%u)",
                     (unsigned long)d->bar[0], (unsigned)d->bar_size[0]);
        return BR_ERR(BR_ENODEV);
    }

    int rc = br_sdhci_probe(d->bar[0], d->intx_virq);
    if (rc != 0) {
        br_log_error("uboot-mmc: SDHCI 探测失败 rc=%d", rc);
        return rc;
    }
    s_base = d->bar[0];

    rc = br_mmc_init(&s_mmc, br_sdhci_host_ops(), BR_NULL);
    if (rc != 0) {
        br_log_error("uboot-mmc: MMC 识别失败 rc=%d", rc);
        return rc;
    }

    s_geom.sector_size = s_mmc.read_bl_len;
    s_geom.n_sectors   = s_mmc.capacity_sectors;
    s_geom.max_rw      = BR_EMMC_MAX_RW_SECTORS;
    s_geom.align       = 4u;      /* ADMA2 要求字对齐; 不满足时驱动自动落 PIO */
    s_geom.flags       = BR_BDEV_F_DMA;

    rc = br_bdev_register(BR_EMMC_DEV_NAME, &s_emmc_ops, &s_mmc);
    if (rc != 0) {
        br_log_error("uboot-mmc: bdev 注册失败 rc=%d (name=%s)", rc, BR_EMMC_DEV_NAME);
        return rc;
    }

    br_log_info("uboot-mmc: /dev/%s 就绪 (%lu sectors, %u-bit, %u Hz, path=%s)",
                BR_EMMC_DEV_NAME, (unsigned long)s_geom.n_sectors,
                (unsigned)s_mmc.bus_width, (unsigned)s_mmc.clock,
                (br_sdhci_get_path() == BR_MMC_PATH_PIO) ? "PIO" : "ADMA2");
    return 0;
}
