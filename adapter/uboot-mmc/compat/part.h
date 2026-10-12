/* adapter/uboot-mmc — **interface type shell** for `<part.h>` (not a copy of u-boot's)
 *
 * ============================== why a "shell" ==============================
 * Same rationale as `compat/blk.h`: upstream `include/part.h` lines 9-12 include
 * `<blk.h> <u-boot/uuid.h> <linux/errno.h> <linux/list.h>`, and it itself declares a **whole
 * family of partition-table parsers** (dos/gpt/iso/amiga/efi…). This file needs only three:
 *
 *   1. `LOG2()` — ★ inconspicuous but required: upstream `mmc_startup()` writes
 *      `bdesc->log2blksz = LOG2(bdesc->blksz)`, and the **only definition** of `LOG2` is
 *      `include/part.h` (not `linux/log2.h` — that has `ilog2`). Without it upstream fails to
 *      compile immediately. The definition below is **verbatim** from upstream;
 *   2. `struct disk_partition` — the partition parser's output type, part of the block layer's
 *      public interface. This file does not parse partitions, but the type must be present
 *      (`CONFIG_PARTITION_UUIDS`/`CONFIG_PARTITION_TYPE_GUID`/`CONFIG_DOS_PARTITION` are all
 *      undefined ⇒ only four fields remain, as with upstream);
 *   3. `part_init()` — called at the end of `mmc_startup()` when `!DM_MMC` (scans the partition
 *      table). This file does no software partitioning (v1 exposes the raw device), so the glue
 *      provides an **explicit empty implementation** with the reason stated (rather than
 *      quietly deleting that upstream line — that would be "modifying upstream").
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_PART_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_PART_H

#include "blk.h"

/* ★ Verbatim from upstream `include/part.h` (it is where `LOG2` is defined, not `linux/log2.h`). */
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

/*
 * One entry in a partition table. ★ The three `#if CONFIG_IS_ENABLED(...)` fields are **not
 * present** (all off in the matrix), so the layout matches upstream's "minimal config" form.
 */
struct disk_partition {
    lbaint_t start;                  /* # of first block in partition */
    lbaint_t size;                   /* number of blocks in partition */
    ulong    blksz;                  /* block size in bytes */
    uchar    name[PART_NAME_LEN];    /* partition name */
    uchar    type[PART_TYPE_LEN];    /* string type description */
    int      bootable;               /* see upstream's bitmask convention */
    u16      type_flags;             /* top 16 bits of GPT partition attributes */
};

/* Called at the end of `mmc_startup()` (`!DM_MMC`). This file does not parse partition tables ⇒ glue provides an empty implementation. */
void part_init(struct blk_desc *desc);

/* Block layer read/write shims (declared by upstream `include/blk.h`; this file's MMC path does
 * **not** go through them, but the interface must be present: consumers like `adapter/uboot-fat`
 * do use `blk_dread`/`blk_dwrite`). */
unsigned long blk_dread(struct blk_desc *block_dev, lbaint_t start,
                        lbaint_t blkcnt, void *buffer);
unsigned long blk_dwrite(struct blk_desc *block_dev, lbaint_t start,
                         lbaint_t blkcnt, const void *buffer);

#endif /* BR_ADAPTER_UBOOT_MMC_SHIM_PART_H */
