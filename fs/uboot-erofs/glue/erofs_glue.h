/*
 * fs/uboot-erofs - adapter-layer internal contract (no upstream counterpart): what
 * upstream lacks and the glue needs -- block-layer prototypes of erofs_dev_read()/
 * erofs_blk_read() (implemented in glue/, wired to br_bdev_ops_t), mount-time facts, the
 * shared scratch buffer, and fixture constants byte-aligned with tools/mk-fs-images.sh.
 * Lives in glue/, so it may include upstream internal.h; upstream does not know us.
 */
#ifndef BR_ADAPTER_UBOOT_EROFS_GLUE_H
#define BR_ADAPTER_UBOOT_EROFS_GLUE_H

#include "erofs_env.h"
#include "internal.h"          /* upstream fs/erofs/internal.h */

/* Selftest fixture device name; the mount source is given by the APP's [[mount]].source. */
#define BR_EROFS_DEV_NAME    "emmc3"      /* bdev name used by the selftest fixture */
#define BR_EROFS_MOUNT_PATH  "/assets"    /* mount point (two sources of truth: brick.toml [[mount]] and here) */
#define BR_EROFS_TYPE_NAME   "erofs"      /* for diagnostics */

/* Max logical block this plugin supports: upstream allows blkszbits in [9,12], but the
 * compressed path requires 4096 (EROFS_MAX_BLOCK_SIZE, from upstream via PAGE_SIZE). */
#define EROFS_BLOCK_SIZE_BITS 12u

/* Static bound for the relative path in a transient inode (BR_VFS_PATH_MAX is vfs-core only). */
#define UBOOT_EROFS_REL_PATH_MAX  256u

/* ---- block layer ---- */
typedef struct br_erofs_dev {
    const br_bdev_ops_t *ops;
    void                *priv;
    br_u64               n_sectors;
    br_u32               sector_size;
    br_u32               max_rw;        /* sectors */
    br_u32               align;
} br_erofs_dev_t;

extern br_erofs_dev_t uboot_erofs_dev;      /* bound at mount time; selftests only read it */
int  uboot_erofs_bdev_bind(const char *dev_name);  /* look up the bdev by the APP-given source device name and remember its geometry */

/* Shared 4 KiB bounce buffer; owned by the writer (see erofs_vfs.c). */
u8  *uboot_erofs_scratch(void);

/* ---- fixture constants (byte-aligned with tools/mk-fs-images.sh) ----
 * Checks are generative (no large literals):
 *   hello.txt 12 B "erofs hello\n" FLAT_INLINE; comp/lz4.txt 70000 B "ABCDEFGH"[i%8] LZ4;
 *   raw.bin 8192 B incompressible LZ4; dir/a.bin 4096 B byte i = i&0xFF FLAT_PLAIN;
 *   dir/sub/deep.txt 5 B "deep\n" FLAT_INLINE.
 * 70000 is not a multiple of 4096 -> covers the tail extent of a compressed file.
 */
#define EROFS_FX_HELLO_PATH      "hello.txt"
#define EROFS_FX_HELLO_SIZE      12u
#define EROFS_FX_HELLO_TEXT      "erofs hello\n"
#define EROFS_FX_HELLO_LAYOUT    EROFS_INODE_FLAT_INLINE

#define EROFS_FX_LZ4_PATH        "comp/lz4.txt"
#define EROFS_FX_LZ4_SIZE        70000u
#define EROFS_FX_LZ4_PERIOD      8u
#define EROFS_FX_LZ4_PATTERN     "ABCDEFGH"      /* byte i = PATTERN[i % 8] */
#define EROFS_FX_LZ4_LAYOUT      EROFS_INODE_COMPRESSED_COMPACT

#define EROFS_FX_RAW_PATH        "raw.bin"
#define EROFS_FX_RAW_SIZE        8192u
#define EROFS_FX_RAW_LAYOUT      EROFS_INODE_COMPRESSED_COMPACT

#define EROFS_FX_ABIN_PATH       "dir/a.bin"
#define EROFS_FX_ABIN_SIZE       4096u
#define EROFS_FX_ABIN_LAYOUT     EROFS_INODE_FLAT_PLAIN

#define EROFS_FX_DEEP_PATH       "dir/sub/deep.txt"
#define EROFS_FX_DEEP_SIZE       5u
#define EROFS_FX_DEEP_TEXT       "deep\n"

/* TC-EROFS-003 mid-read window: covers extent splitting at a non-zero offset. */
#define EROFS_FX_ABIN_MID_OFF    1000u
#define EROFS_FX_ABIN_MID_LEN    500u

#define EROFS_DIRENT_MAX_NAME    EROFS_NAME_LEN

/* EROFS dirent ftype -> br_dirent type; upstream only has the opposite direction. */
static inline u8 uboot_erofs_ftype_from_mode(umode_t mode)
{
    if (S_ISDIR(mode))  { return 2u; }   /* DT_DIR */
    if (S_ISREG(mode))  { return 1u; }   /* DT_REG */
    if (S_ISLNK(mode))  { return 7u; }   /* DT_LNK */
    if (S_ISCHR(mode))  { return 3u; }   /* DT_CHR */
    if (S_ISBLK(mode))  { return 4u; }   /* DT_BLK */
    if (S_ISFIFO(mode)) { return 5u; }   /* DT_FIFO */
    if (S_ISSOCK(mode)) { return 6u; }   /* DT_SOCK */
    return 0u;                           /* DT_UNKNOWN */
}

#endif /* BR_ADAPTER_UBOOT_EROFS_GLUE_H */
