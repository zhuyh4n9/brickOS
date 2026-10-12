/* adapter/uboot-mmc — `<part.h>` 的**接口类型壳**(不是 u-boot 那份的复制)
 *
 * ============================== 为什么是"壳" ==============================
 * 与 `compat/blk.h` 同源的理由: 上游 `include/part.h` 第 9-12 行 include
 * `<blk.h> <u-boot/uuid.h> <linux/errno.h> <linux/list.h>`, 而它自带**整族分区表解析器**
 * 的声明(dos/gpt/iso/amiga/efi…)。本件要的只有三件:
 *
 *   1. `LOG2()` —— ★ 这一条不显眼但必需: 上游 `mmc_startup()` 写
 *      `bdesc->log2blksz = LOG2(bdesc->blksz)`, 而 `LOG2` 的**唯一定义处**就是
 *      `include/part.h`(不在 `linux/log2.h` 里 —— 那是 `ilog2`)。少了它上游立刻编不过。
 *      下面的定义**逐字**来自上游;
 *   2. `struct disk_partition` —— 分区表解析器的输出类型, 属块层公共接口面。本件不解析
 *      分区, 但类型要在场(`CONFIG_PARTITION_UUIDS`/`CONFIG_PARTITION_TYPE_GUID`/
 *      `CONFIG_DOS_PARTITION` 全不定义 ⇒ 与上游一样只剩四个字段);
 *   3. `part_init()` —— `mmc_startup()` 末尾在 `!DM_MMC` 时会调它(扫描分区表)。
 *      本件不做软件分区(v1 暴露裸设备), 所以 glue 给一个**显式的空实现**并写明理由
 *      (而不是悄悄把上游那一行删掉 —— 那才是"改上游")。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_PART_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_PART_H

#include "blk.h"

/* ★ 逐字取自上游 `include/part.h`(它是 `LOG2` 的定义处, 不是 `linux/log2.h`)。 */
#define LOG2(x) (((x & 0xaaaaaaaa) ? 1 : 0) + ((x & 0xcccccccc) ? 2 : 0) + \
                 ((x & 0xf0f0f0f0) ? 4 : 0) + ((x & 0xff00ff00) ? 8 : 0) + \
                 ((x & 0xffff0000) ? 16 : 0))
#define LOG2_INVALID(type) ((type)((sizeof(type) << 3) - 1))

/* Part types(上游同名同值) */
#define PART_TYPE_UNKNOWN   0x00
#define PART_TYPE_MAC       0x01
#define PART_TYPE_DOS       0x02
#define PART_TYPE_ISO       0x03
#define PART_TYPE_AMIGA     0x04
#define PART_TYPE_EFI       0x05
#define PART_TYPE_MTD       0x06
#define PART_TYPE_UBI       0x07

/* device types(上游同名同值) */
#define DEV_TYPE_UNKNOWN    0xff
#define DEV_TYPE_HARDDISK   0x00
#define DEV_TYPE_TAPE       0x01
#define DEV_TYPE_CDROM      0x05
#define DEV_TYPE_OPDISK     0x07

#define PART_NAME_LEN       32
#define PART_TYPE_LEN       32
#define MAX_SEARCH_PARTITIONS 128

#define PART_BOOTABLE               ((int)BIT(0))
#define PART_EFI_SYSTEM_PARTITION   ((int)BIT(1))

struct block_drvr {
    char *name;
    int (*select_hwpart)(int dev_num, int hwpart);
};

/*
 * 分区表里的一条。★ 三个 `#if CONFIG_IS_ENABLED(...)` 的字段**不参与**(矩阵里全关),
 * 于是布局与上游"最小配置"下的形态一致。
 */
struct disk_partition {
    lbaint_t start;                  /* # of first block in partition */
    lbaint_t size;                   /* number of blocks in partition */
    ulong    blksz;                  /* block size in bytes */
    uchar    name[PART_NAME_LEN];    /* partition name */
    uchar    type[PART_TYPE_LEN];    /* string type description */
    int      bootable;               /* 见上游的位掩码口径 */
    u16      type_flags;             /* top 16 bits of GPT partition attributes */
};

/* `mmc_startup()` 末尾(`!DM_MMC`)会调它。本件不解析分区表 ⇒ glue 给空实现。 */
void part_init(struct blk_desc *desc);

/* 块层读写 shim(上游 `include/blk.h` 声明; 本件的 MMC 路径**不经过**它们, 但接口面
 * 要在场: `adapter/uboot-fat` 那类消费者正是走 `blk_dread`/`blk_dwrite`)。 */
unsigned long blk_dread(struct blk_desc *block_dev, lbaint_t start,
                        lbaint_t blkcnt, void *buffer);
unsigned long blk_dwrite(struct blk_desc *block_dev, lbaint_t start,
                         lbaint_t blkcnt, const void *buffer);

#endif /* BR_ADAPTER_UBOOT_MMC_SHIM_PART_H */
