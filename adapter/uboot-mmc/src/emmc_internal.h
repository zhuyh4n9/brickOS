/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * adapter/uboot-mmc — 插件内部契约(不导出)
 *
 * 分层(见 docs/analysis/0001-emmc-bdev-gap.md §8 建议架构):
 *   mmc_core.c    协议核心: CMD/ACMD 状态机、CID/CSD/EXT_CSD、块读写(在 host ops 之上)
 *   sdhci_host.c  SDHCI 主机: 寄存器/命令/数据传输(ADMA2/PIO)/中断/set_ios
 *   emmc_bdev.c   bdev 对接 + PCIe 绑定 + 生命周期钩子
 * 三者之间只有这一个小接口: `br_mmc_host_ops_t`(send_cmd/set_ios)。
 */
#ifndef BR_THIRD_PARTY_UBOOT_MMC_INTERNAL_H
#define BR_THIRD_PARTY_UBOOT_MMC_INTERNAL_H

#include "mmc_compat.h"   /* 适配层: u-boot 运行环境 → brickOS(唯一入口) */

#include "mmc_regs.h"

/* ==================================================================== 主机接口 */

typedef struct br_mmc_cmd {
    br_u32 cmdidx;
    br_u32 resp_type;      /* MMC_RSP_* */
    br_u32 cmdarg;
    br_u32 response[4];    /* 成功时被 host 填好(136 位响应给满 4 个) */
} br_mmc_cmd_t;

typedef struct br_mmc_data {
    void  *buf;            /* READ: 目的; WRITE: 源 */
    br_u32 flags;          /* MMC_DATA_READ / MMC_DATA_WRITE */
    br_u32 blocks;         /* 块数(> 0) */
    br_u32 blocksize;      /* 每块字节(本驱动 = 512) */
} br_mmc_data_t;

/*
 * 主机驱动契约(设计的"通用主机"边界):
 *   send_cmd: 同步完成一次命令(带 data 时包含数据搬运); 返回 0 或负 errno。
 *   set_ios:  应用 {时钟, 总线宽度, 模式} —— 由协议核心在切换后调用。
 */
typedef struct br_mmc_host_ops {
    int (*send_cmd)(void *host, br_mmc_cmd_t *cmd, br_mmc_data_t *data);
    int (*set_ios)(void *host, br_u32 clock, br_u32 bus_width, br_u32 mode);
} br_mmc_host_ops_t;

#define BR_MMC_MODE_LEGACY 0u
#define BR_MMC_MODE_HS     1u

/* ==================================================================== 协议核心状态 */

typedef struct br_mmc {
    const br_mmc_host_ops_t *ops;
    void    *host;
    br_u32   clock;              /* 当前时钟(Hz) */
    br_u32   bus_width;          /* 1 / 4 / 8 */
    br_u32   mode;               /* BR_MMC_MODE_* */
    br_u16   rca;
    br_u32   read_bl_len;        /* 逻辑块长(本驱动恒 512) */
    br_bool  high_capacity;      /* OCR.HCS: LBA 以块为单位 */
    br_u64   capacity_sectors;   /* 512 B 扇区总数(EXT_CSD SEC_CNT) */
    br_u32   card_type;          /* EXT_CSD CARD_TYPE */
    br_u8    ext_csd[512];
} br_mmc_t;

int br_mmc_init (br_mmc_t *m, const br_mmc_host_ops_t *ops, void *host);
int br_mmc_read (br_mmc_t *m, br_u64 lba, void *buf, br_size_t n);
int br_mmc_write(br_mmc_t *m, br_u64 lba, const void *buf, br_size_t n);
int br_mmc_flush(br_mmc_t *m);

/* ==================================================================== SDHCI 主机层 */

/* 数据通路选择(运行期可切; 见 ADR-0021 D-4) */
#define BR_MMC_PATH_DMA 0u
#define BR_MMC_PATH_PIO 1u

/*
 * SDHCI 主机**实例**(不透明)。ADR-0022: 一台 `sdhci-pci` 只挂一张卡, 而本刀要三卷 FAT
 * + 一卷 EROFS ⇒ 主机层必须从"全局单例"改成"N 个实例"。实例的句柄就是协议核心
 * `br_mmc_init(m, ops, host)` 的 `host` —— `br_mmc_host_ops_t` 本来就带这个参数, 所以
 * 协议核心一行都不用改。
 * 每个实例各有一份 {寄存器窗口, 时钟, 总线宽度, 通路, 信号量, pending 位} —— 这正是不
 * 能再用静态单例的原因: 四块卡会互相踩时钟与中断状态。
 */
typedef struct br_sdhci br_sdhci_t;

/* 探测 + 复位 + 上电 + 中断就绪(幂等: 同一 base 重复探测交回同一实例)。
 * base = BAR0 映射后的寄存器窗口; 成功时 `*out` 非空。 */
int  br_sdhci_probe(br_uintptr_t base, br_u32 intx_virq, br_sdhci_t **out);
void br_sdhci_set_path(br_sdhci_t *h, br_u32 path);      /* BR_MMC_PATH_DMA / _PIO */
br_u32 br_sdhci_get_path(const br_sdhci_t *h);
const br_mmc_host_ops_t *br_sdhci_host_ops(void);   /* 交给协议核心的主机 ops 表 */
/* 本驱动策略: 传输 < 阈值(扇区)或缓冲未 4 字节对齐 ⇒ 即使选 DMA 也落 PIO(兜底)。 */
#define BR_SDHCI_PIO_THRESHOLD_SECTORS 4u

/* 单次传输上限(扇区): 32 KiB —— 一个 ADMA2 描述符的 len 上限 65532 与超时预算的交点。 */
#define BR_EMMC_MAX_RW_SECTORS 64u

/* ==================================================================== 器件契约(私有) */

/* 本插件最多同时驱动的卡数(= `sdhci-pci` 端点数上界)。取值来自**平台事实**而不是拍脑袋:
 * QEMU virt 的 GPEX 只把 4 根 INTx 线接到 GIC(SPI 3..6), 而 core 的 IRQ 框架规定
 * "每根物理线恰一属主"(`irq_core.c` INV-A)⇒ 第 5 块卡的 INTx 会与第 1 块撞线而注册失败。
 * 所以 4 是本平台**能挂满**的卡数。 */
#define BR_EMMC_MAX_DEVS     4u

/* 设备名形如 `emmc<d>`(dev-core 命名规则 [a-z][a-z0-9]*, 见 7-02 §1)。 */
#define BR_EMMC_DEV_NAME     "emmc0"
#define BR_EMMC_NAME_MAX     8u

/*
 * 设备级用例的**擦写区**(ADR-0022): 四块卡各有用途(0/1/2 = FAT12/16/32 卷, 3 = EROFS),
 * FAT 卷被文件系统占满, 而 EROFS 是只读的 —— 它的镜像只占卡首(实测 ≤ 1 MiB), 卡尾
 * 留给"写特征模式再读回"的设备级用例。**不与任何文件系统的数据区重叠**这条性质由
 * `tools/mk-fs-images.sh` 断言(EROFS 镜像 > 1 MiB 就直接失败), 不是靠约定。
 */
#define BR_EMMC_ASSETS_DEV        "emmc3"
#define BR_EMMC_SCRATCH_LBA       4096u    /* 2 MiB 起 */
#define BR_EMMC_SCRATCH_SECTORS   1024u    /* 512 KiB 擦写区(2 MiB..2.5 MiB) */

#define BR_EMMC_IOC_MAGIC    'e'
#define BR_EMMC_IOC_GET_INFO 0u
#define BR_EMMC_IOC_SET_PATH 1u

/* GET_INFO 的回答(ioctl 载荷)。生产(emmc_bdev.c)与自检(emmc_selftest.c)共用一份布局。 */
typedef struct br_emmc_info {
    br_uintptr_t base;              /* SDHCI 寄存器窗口(BAR0) */
    br_u64       capacity_sectors;
    br_u32       sector_size;
    br_u32       max_rw;
    br_u32       bus_width;
    br_u32       clock_hz;
    br_u32       path;              /* BR_MMC_PATH_DMA / _PIO */
    br_u32       card_type;
} br_emmc_info_t;

#endif /* BR_THIRD_PARTY_UBOOT_MMC_INTERNAL_H */
