/* adapter/uboot-mmc — an **interface-type shim** for `<blk.h>` (not a copy of u-boot's)
 *
 * ============================== why a "shim" ==============================
 * The originals under upstream `drivers/mmc/` need `struct blk_desc`, `U_BOOT_LEGACY_BLK()`, `blk_dselect_hwpart()`
 * — those **block-layer interface types**. Why not vendor upstream `include/blk.h`:
 *
 *   u-boot's `include/blk.h` lines 10-12 are
 *       #include <bouncebuf.h>
 *       #include <dm/uclass-id.h>
 *       #include <efi.h>
 *   ⇒ vendoring it drags in `<bouncebuf.h>` (the whole bounce buffer mechanism) and `<efi.h>`
 *   (the whole EFI boot services / `efi_guid_t` / `efi_status_t` family). This file needs only
 *   **one struct, one macro, two function prototypes** from it — exactly what ADR-0026 D-3 calls "a faithful but minimal
 *   interface type", handled the same way as `adapter/uboot-erofs/compat/part.h`.
 *
 * ★ What "faithful" means (aligned with upstream item by item, not invented):
 *   - derivation of `lbaint_t` and `LBAF`/`LBAFU` (`CONFIG_SYS_64BIT_LBA` undefined ⇒ `ulong`,
 *     which is 64-bit on LP64 — matching aarch64 u-boot defaults);
 *   - the **non-BLK field order** of `struct blk_desc`: `CONFIG_BLK` is deliberately undefined here, so upstream's
 *     struct takes the `#else` branch (three trailing legacy callbacks + `priv`), replicated field by field;
 *   - `BLK_VEN_SIZE`/`BLK_PRD_SIZE`/`BLK_REV_SIZE`: **values must match upstream** —
 *     `mmc_startup()` does `sprintf(bdesc->vendor, ...)`, and the buffer size is part of the layout;
 *   - `U_BOOT_LEGACY_BLK()`: upstream expands it to "place a `struct blk_driver` in the
 *     `.u_boot_list_2_blk_driver_2_<name>` section". This file has **no** such link-section enumeration (we do not
 *     enumerate block drivers; the glue holds a `struct mmc *` directly), so it is downgraded to a **plain global object**:
 *     the `U_BOOT_LEGACY_BLK(mmc) = { ... };` at the end of `mmc_legacy.c` therefore still holds verbatim,
 *     just nobody iterates it (`--gc-sections` drops it). This is a **known and deliberate behavior deviation**,
 *     registered in the "deviations" section of `ADAPTER-NOTES.md`.
 *
 * ★ `enum uclass_id` is not defined here: it is a zero-dependency upstream original (`third_party/.../dm/uclass-id.h`),
 *   so `compat/dm/uclass-id.h` forwards it directly.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H

#include "mmc_env.h"
#include <dm/uclass-id.h>

/* ---- logical block address type (derived as in upstream blk.h) ---- */
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

struct udevice;                  /* forward declaration: no DM here ⇒ exists as a type only */

#define BLK_VEN_SIZE      40
#define BLK_PRD_SIZE      20
#define BLK_REV_SIZE      8

#define PART_FORMAT_PCAT  0x1
#define PART_FORMAT_GPT   0x2

/* same values as upstream (enum sig_type); no partition table detection here, but the struct field is required */
enum sig_type {
    SIG_TYPE_NONE,
    SIG_TYPE_MBR,
    SIG_TYPE_GUID,
    SIG_TYPE_COUNT
};

/*
 * Block device descriptor. ★ This is the **only substantive interface** between this file and upstream: upstream `mmc.c`
 * fills `blksz`/`log2blksz`/`lba`/`vendor`/`product`/`revision`/`hwpart` into it, and reads
 * `devnum` from it to find the `struct mmc`; our glue also reads `lba` (capacity) and `blksz` from it.
 * Field order matches upstream `include/blk.h`'s `!CONFIG_BLK` shape one by one.
 */
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
        char     guid_sig[16];   /* GPT GUID Signature (upstream is efi_guid_t; not parsed here,
                                  * but **16 bytes are kept** so the layout does not drift) */
    };
    /* ---- `!CONFIG_BLK` branch: legacy block callbacks ---- */
    unsigned long (*block_read)(struct blk_desc *block_dev, lbaint_t start,
                                lbaint_t blkcnt, void *buffer);
    unsigned long (*block_write)(struct blk_desc *block_dev, lbaint_t start,
                                 lbaint_t blkcnt, const void *buffer);
    unsigned long (*block_erase)(struct blk_desc *block_dev, lbaint_t start,
                                 lbaint_t blkcnt);
    void *priv;                  /* driver private struct pointer */
};

/* block driver table shape when upstream `CONFIG_BLK` is disabled; see "U_BOOT_LEGACY_BLK" in the file header. */
struct blk_driver {
    const char *uclass_idname;
    enum uclass_id uclass_id;
    int max_devs;
    struct blk_desc *desc;
    int (*get_dev)(int devnum, struct blk_desc **descp);
    int (*select_hwpart)(struct blk_desc *desc, int hwpart);
};

/*
 * ★ Behavior deviation (see the last part of the file header): upstream = register a `blk_driver` into a link section;
 *   this file = a plain global object. `mmc_legacy.c`'s text therefore still holds verbatim, but there is **no**
 *   "look up a block driver by uclass_id" mechanism — this file does not need it (the glue holds the instance itself).
 */
#define U_BOOT_LEGACY_BLK(__name) \
    struct blk_driver __attribute__((unused)) uboot_mmc_legacy_blk_ ## __name

/* Implemented by the glue (see glue/emmc_bdev.c): these two are hook points on upstream's mmc block-layer path.
 * `mmc_bread()` does `blk_dselect_hwpart(block_dev, block_dev->hwpart)` before every read. */
int blk_dselect_hwpart(struct blk_desc *desc, int hwpart);
int blk_select_hwpart_devnum(enum uclass_id uclass_id, int devnum, int hwpart);

#endif /* BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H */
