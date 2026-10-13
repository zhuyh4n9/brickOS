/* driver/uboot-mmc interface-type shim for `<blk.h>` (not a copy of u-boot's).
 *
 * Upstream needs `struct blk_desc`, `U_BOOT_LEGACY_BLK()` and `blk_dselect_hwpart()` from it.
 * u-boot's `include/blk.h` includes `<bouncebuf.h>` and `<efi.h>`, dragging in the whole bounce
 * buffer and EFI families, so this file restates the minimal interface instead.
 *
 * Faithful to upstream item by item:
 *   - `lbaint_t` and `LBAF`/`LBAFU`: `CONFIG_SYS_64BIT_LBA` undefined => `ulong` (64-bit on LP64);
 *   - `struct blk_desc` field order follows upstream's `!CONFIG_BLK` branch (three trailing legacy
 *     callbacks + `priv`);
 *   - `BLK_VEN_SIZE`/`BLK_PRD_SIZE`/`BLK_REV_SIZE`: values must match upstream, they are part of
 *     the layout (`mmc_startup()` sprintf's into `vendor`);
 *   - `U_BOOT_LEGACY_BLK()`: upstream places a `struct blk_driver` in a link section, here it is a
 *     plain global object (no block-driver enumeration; glue holds the `struct mmc *`), so
 *     `mmc_legacy.c`'s text still holds verbatim but nobody iterates it (`--gc-sections` drops
 *     it). Known, deliberate deviation (see `ADAPTER-NOTES.md`).
 *
 * `enum uclass_id` is forwarded from the zero-dependency vendor original.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H

#include "mmc_env.h"
#include <dm/uclass-id.h>

#ifdef CONFIG_SYS_64BIT_LBA
typedef uint64_t lbaint_t;
#define LBAFlength "ll"
#else
typedef ulong lbaint_t;          /* aarch64 LP64: ulong is 64-bit */
#define LBAFlength "l"
#endif
#define LBAF  "%" LBAFlength "x"
#define LBAFU "%" LBAFlength "u"

#define DEFAULT_BLKSZ     512

struct udevice;                  /* forward declaration: no DM here */

#define BLK_VEN_SIZE      40
#define BLK_PRD_SIZE      20
#define BLK_REV_SIZE      8

#define PART_FORMAT_PCAT  0x1
#define PART_FORMAT_GPT   0x2

/* same values as upstream (enum sig_type); the struct field is required even without detection */
enum sig_type {
    SIG_TYPE_NONE,
    SIG_TYPE_MBR,
    SIG_TYPE_GUID,
    SIG_TYPE_COUNT
};

/* Block device descriptor, the substantive interface with upstream: `mmc.c` fills `blksz`/
 * `log2blksz`/`lba`/`vendor`/`product`/`revision`/`hwpart` and reads `devnum`; glue reads `lba`
 * (capacity) and `blksz`. Field order matches upstream's `!CONFIG_BLK` shape. */
struct blk_desc {
    enum uclass_id  uclass_id;   /* type of the interface */
    int             devnum;      /* device number */
    unsigned char   part_type;   /* partition type */
    unsigned char   target;      /* target SCSI ID */
    unsigned char   lun;         /* target LUN */
    unsigned char   hwpart;      /* HW partition, e.g. for eMMC */
    unsigned char   type;        /* device type */
    unsigned char   removable;   /* removable device */
    bool            lba48;       /* device can use 48bit addr (ATA/ATAPI v7) */
    unsigned char   atapi;       /* Use ATAPI protocol */
    unsigned char   bb;          /* Use bounce buffer */
    lbaint_t        lba;         /* number of blocks */
    unsigned long   blksz;       /* block size */
    int             log2blksz;   /* for convenience: log2(blksz) */
    char            vendor[BLK_VEN_SIZE + 1];   /* device vendor string */
    char            product[BLK_PRD_SIZE + 1];  /* device product number */
    char            revision[BLK_REV_SIZE + 1]; /* firmware revision */
    enum sig_type   sig_type;    /* Partition table signature type */
    union {
        uint32_t mbr_sig;        /* MBR integer signature */
        char     guid_sig[16];   /* GPT GUID sig (upstream efi_guid_t); 16 B keep the layout */
    };
    unsigned long (*block_read)(struct blk_desc *block_dev, lbaint_t start,
                                lbaint_t blkcnt, void *buffer);
    unsigned long (*block_write)(struct blk_desc *block_dev, lbaint_t start,
                                 lbaint_t blkcnt, const void *buffer);
    unsigned long (*block_erase)(struct blk_desc *block_dev, lbaint_t start,
                                 lbaint_t blkcnt);
    void *priv;                  /* driver private struct pointer */
};

/* block driver table shape for upstream `!CONFIG_BLK` (see U_BOOT_LEGACY_BLK below) */
struct blk_driver {
    const char *uclass_idname;
    enum uclass_id uclass_id;
    int max_devs;
    struct blk_desc *desc;
    int (*get_dev)(int devnum, struct blk_desc **descp);
    int (*select_hwpart)(struct blk_desc *desc, int hwpart);
};

#define U_BOOT_LEGACY_BLK(__name) \
    struct blk_driver __attribute__((unused)) uboot_mmc_legacy_blk_ ## __name

/* Implemented by glue; `mmc_bread()` calls `blk_dselect_hwpart()` before every read. */
int blk_dselect_hwpart(struct blk_desc *desc, int hwpart);
int blk_select_hwpart_devnum(enum uclass_id uclass_id, int devnum, int hwpart);

#endif /* BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H */
