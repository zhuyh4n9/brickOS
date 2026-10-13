/* adapter/uboot-mmc interface-type shell for `<part.h>` (not a copy of u-boot's).
 *
 * Upstream `include/part.h` pulls in a whole family of partition-table parsers; this file needs
 * only: `LOG2()` (its only definition, used by `mmc_startup()` for `log2blksz`; verbatim below),
 * `struct disk_partition` (the parser's output type; with the partition configs off only four
 * fields remain, as upstream) and `part_init()` (called at the end of `mmc_startup()` when
 * `!DM_MMC`; this file does no software partitioning -- v1 exposes the raw device -- so glue
 * provides an explicit empty implementation rather than deleting the upstream call).
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_PART_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_PART_H

#include "blk.h"

/* Verbatim from upstream `include/part.h`, the only definition of `LOG2` (not `linux/log2.h`). */
#define LOG2(x) (((x & 0xaaaaaaaa) ? 1 : 0) + ((x & 0xcccccccc) ? 2 : 0) + \
                 ((x & 0xf0f0f0f0) ? 4 : 0) + ((x & 0xff00ff00) ? 8 : 0) + \
                 ((x & 0xffff0000) ? 16 : 0))
#define LOG2_INVALID(type) ((type)((sizeof(type) << 3) - 1))

/* Part types (same names and values as upstream) */
#define PART_TYPE_UNKNOWN   0x00
#define PART_TYPE_MAC       0x01
#define PART_TYPE_DOS       0x02
#define PART_TYPE_ISO       0x03
#define PART_TYPE_AMIGA     0x04
#define PART_TYPE_EFI       0x05
#define PART_TYPE_MTD       0x06
#define PART_TYPE_UBI       0x07

/* device types (same names and values as upstream) */
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

/* One partition table entry; the three `CONFIG_IS_ENABLED(...)` fields are off in the matrix. */
struct disk_partition {
    lbaint_t start;                  /* # of first block in partition */
    lbaint_t size;                   /* number of blocks in partition */
    ulong    blksz;                  /* block size in bytes */
    uchar    name[PART_NAME_LEN];    /* partition name */
    uchar    type[PART_TYPE_LEN];    /* string type description */
    int      bootable;               /* see upstream's bitmask convention */
    u16      type_flags;             /* top 16 bits of GPT partition attributes */
};

/* Called at the end of `mmc_startup()` (`!DM_MMC`); glue provides an empty implementation. */
void part_init(struct blk_desc *desc);

/* Declared by upstream `include/blk.h`; this MMC path does not call them, but uboot-fat does. */
unsigned long blk_dread(struct blk_desc *block_dev, lbaint_t start,
                        lbaint_t blkcnt, void *buffer);
unsigned long blk_dwrite(struct blk_desc *block_dev, lbaint_t start,
                         lbaint_t blkcnt, const void *buffer);

#endif /* BR_ADAPTER_UBOOT_MMC_SHIM_PART_H */
