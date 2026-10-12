/*
 * adapter/uboot-mmc — bdev 对接 + PCIe 绑定 + 生命周期钩子
 *
 * 设计依据: `docs/7-storage/7-02-bdev.md` §1/§4(形状与"DMA/PIO 要能指出")、
 * `docs/decisions/0021-bdev-pci-emmc.md` D-3(隔离)/D-4(中断 + ADMA2/PIO)/D-5(flush 口径)、
 * `docs/decisions/0022-erofs-fat.md`(多卡: emmc0/1/2 = FAT12/16/32 卷, emmc3 = EROFS)。
 *
 * ## 为什么硬件初始化在 **start** 相而不是 core 相
 *   插件管理器的启动链是 `EARLY/CORE/LATE → 全局开中断 → START`(见 plugin_mgr 的文件头)。
 *   本驱动用**中断**等命令/数据完成 ⇒ CORE 相里全局中断还没开, ISR/bh 不会跑, 等下去必超时。
 *   所以: `early_init`/`init` 都空转, 真正的"探测 + 识别 + 注册 bdev"落在 `start`
 *   —— 那时中断已开, 且 platform 的 timer 也已就位(它是本件的传递依赖)。
 *   这条相位事实也是 `adapter/uboot-fat` / `adapter/uboot-erofs` 必须在自己的
 *   `[[dep]]` 里声明对**本件**的 init 边的原因(它们的 mount 也落在 start 相)。
 *
 * ## 多卡(ADR-0022)
 *   一台 `sdhci-pci` 只挂一张卡(设备树与硬件都如此), 而本刀要三卷 FAT + 一卷 EROFS ⇒
 *   **枚举到的每个 sdhci-pci 端点各注册一个 bdev**, 名字依次 `emmc0`、`emmc1`、…。
 *   平台侧(platform/qemu-aarch64 的 `[build.target.qemu].extra`)给四块卡各配一个
 *   `-drive` 与一个 `-device emmc`; 驱动缺席时它们只是"没被认领的设备"。
 *   每块卡的分工写在 `tools/mk-fs-images.sh`(镜像内容)与 ADR-0022(为什么这样分)。
 *
 * ## 设备名与 bdev 语义
 *   `emmc<d>`(dev-core 命名规则 [a-z][a-z0-9]*); `geometry` 的 DMA/PIO 位**每次调用现算**
 *   ⇒ 运行期 `SET_PATH` 之后, 下一次几何查询就看到新通路(ADR-0021 D-4)。
 *   `SET_PATH` 是**按卡**的(每个实例各有一份通路状态) —— 多卡改造后自然如此。
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

/*
 * 一块卡的运行期状态。**没有全局单例**(ADR-0022): 四块卡各有自己的协议核心状态、
 * 几何、主机实例与设备名, 全部收在这个结构里; `bdev ops` 的 `priv` 就指向它。
 */
typedef struct emmc_dev {
    br_mmc_t       mmc;
    br_bdev_geom_t geom;        /* n_sectors/max_rw 在 start 里填; flags 每次现算 */
    br_sdhci_t    *host;        /* SDHCI 主机实例(通路状态挂在它上面) */
    br_uintptr_t   base;        /* SDHCI 寄存器窗口(BAR0), 供 GET_INFO 回答 */
    char           name[BR_EMMC_NAME_MAX];
} emmc_dev_t;

static emmc_dev_t s_dev[BR_EMMC_MAX_DEVS];
static br_u32     s_dev_n;

/* ==================================================================== bdev ops */

static int emmc_read(void *priv, br_u64 lba, void *buf, br_size_t n_lba)
{
    return br_mmc_read(&((emmc_dev_t *)priv)->mmc, lba, buf, n_lba);
}

static int emmc_write(void *priv, br_u64 lba, const void *buf, br_size_t n_lba)
{
    return br_mmc_write(&((emmc_dev_t *)priv)->mmc, lba, buf, n_lba);
}

static int emmc_flush(void *priv)
{
    return br_mmc_flush(&((emmc_dev_t *)priv)->mmc);
}

static int emmc_geometry(void *priv, br_bdev_geom_t *g)
{
    const emmc_dev_t *d = (const emmc_dev_t *)priv;
    if (d == BR_NULL || g == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    br_bdev_geom_t x = d->geom;
    x.flags = (br_sdhci_get_path(d->host) == BR_MMC_PATH_PIO) ? BR_BDEV_F_PIO : BR_BDEV_F_DMA;
    *g = x;
    return 0;
}

static int emmc_ioctl(void *priv, br_u32 cmd, void *arg)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    if (BR_IOC_TYPE(cmd) != (br_u32)BR_EMMC_IOC_MAGIC) {
        return BR_ERR(ENOTSUP);
    }

    switch (BR_IOC_NR(cmd)) {
    case BR_EMMC_IOC_GET_INFO: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }
        br_emmc_info_t *info = (br_emmc_info_t *)arg;
        info->base             = d->base;
        info->capacity_sectors = d->mmc.capacity_sectors;
        info->sector_size      = d->geom.sector_size;
        info->max_rw           = d->geom.max_rw;
        info->bus_width        = d->mmc.bus_width;
        info->clock_hz         = d->mmc.clock;
        info->path             = br_sdhci_get_path(d->host);
        info->card_type        = d->mmc.card_type;
        return 0;
    }
    case BR_EMMC_IOC_SET_PATH: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }
        const br_u32 path = *(const br_u32 *)arg;
        if (path != BR_MMC_PATH_DMA && path != BR_MMC_PATH_PIO) {
            return BR_ERR(EINVAL);
        }
        br_sdhci_set_path(d->host, path);
        br_log_info("uboot-mmc: %s 数据通路切到 %s", d->name,
                    (path == BR_MMC_PATH_PIO) ? "PIO" : "ADMA2");
        return 0;
    }
    default:
        return BR_ERR(ENOTSUP);
    }
}

static int emmc_suspend(void *priv) { (void)priv; return BR_ERR(ENOTSUP); }   /* D22 预留 */
static int emmc_resume (void *priv) { (void)priv; return BR_ERR(ENOTSUP); }

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

/* 把设备序号拼成 `emmc<d>`(无 snprintf, 逐个字符填; 只可能是 0..9)。 */
static void emmc_make_name(char *out, br_u32 idx)
{
    out[0] = 'e';
    out[1] = 'm';
    out[2] = 'm';
    out[3] = 'c';
    out[4] = (char)('0' + (char)(idx % 10u));
    out[5] = '\0';
}

/* 探测并注册**一块**卡。任何一步失败都返回负 errno(首败即停机是刻意的)。 */
static int emmc_bring_up(const br_pci_dev_t *pci, emmc_dev_t *d)
{
    if (pci->bar[0] == 0u || pci->bar_size[0] < 0x100u) {
        br_log_error("uboot-mmc: %s 的 sdhci BAR0 未分配或过小(bar0=0x%lx size=%u)",
                     d->name, (unsigned long)pci->bar[0], (unsigned)pci->bar_size[0]);
        return BR_ERR(ENODEV);
    }

    int rc = br_sdhci_probe(pci->bar[0], pci->intx_virq, &d->host);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s SDHCI 探测失败 rc=%d", d->name, rc);
        return rc;
    }
    d->base = pci->bar[0];

    rc = br_mmc_init(&d->mmc, br_sdhci_host_ops(), d->host);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s MMC 识别失败 rc=%d", d->name, rc);
        return rc;
    }

    d->geom.sector_size = d->mmc.read_bl_len;
    d->geom.n_sectors   = d->mmc.capacity_sectors;
    d->geom.max_rw      = BR_EMMC_MAX_RW_SECTORS;
    d->geom.align       = 4u;      /* ADMA2 要求字对齐; 不满足时驱动自动落 PIO */
    d->geom.flags       = BR_BDEV_F_DMA;

    rc = br_bdev_register(d->name, &s_emmc_ops, d);
    if (rc != 0) {
        br_log_error("uboot-mmc: bdev 注册失败 rc=%d (name=%s)", rc, d->name);
        return rc;
    }

    br_log_info("uboot-mmc: /dev/%s 就绪 (%lu sectors, %u-bit, %u Hz, path=%s)",
                d->name, (unsigned long)d->geom.n_sectors,
                (unsigned)d->mmc.bus_width, (unsigned)d->mmc.clock,
                (br_sdhci_get_path(d->host) == BR_MMC_PATH_PIO) ? "PIO" : "ADMA2");
    return 0;
}

int uboot_mmc_start(void)
{
    const br_u32 total = br_pci_dev_count();
    br_u32 found = 0u;

    for (br_u32 i = 0u; i < total; i++) {
        const br_pci_dev_t *d = br_pci_dev_at(i);
        if (d == BR_NULL) {
            continue;
        }
        if (d->vendor != BR_EMMC_PCI_VENDOR || d->device != BR_EMMC_PCI_DEVICE) {
            continue;
        }
        if (s_dev_n >= BR_EMMC_MAX_DEVS) {
            br_log_error("uboot-mmc: 卡数超上限 %u(第 %lu 个 sdhci-pci 未被接管)",
                         (unsigned)BR_EMMC_MAX_DEVS, (unsigned long)(found + 1u));
            return BR_ERR(ENOSPC);
        }

        emmc_dev_t *dev = &s_dev[s_dev_n];
        emmc_make_name(dev->name, s_dev_n);     /* 名字先定: 失败日志里要能指名道姓 */
        s_dev_n++;                              /* 占位: 失败也不回收(启动期即停机) */

        const int rc = emmc_bring_up(d, dev);
        if (rc != 0) {
            return rc;
        }
        found++;
    }

    if (found == 0u) {
        br_log_error("uboot-mmc: 组合里有 pci-core, 但没找到 sdhci-pci (%04x:%04x)",
                     (unsigned)BR_EMMC_PCI_VENDOR, (unsigned)BR_EMMC_PCI_DEVICE);
        return BR_ERR(ENODEV);
    }

    br_log_info("uboot-mmc: 共接管 %lu 块 sdhci-pci 卡", (unsigned long)found);
    return 0;
}
