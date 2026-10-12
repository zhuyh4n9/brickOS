/*
 * adapter/uboot-erofs — **adapter-layer internal contract** (our project's code; upstream has no counterpart)
 *
 * Upstream `fs/erofs/` now **compiles as-is** (see ADR-0026): its own `internal.h` provides
 * `sbi` / `erofs_blksiz()` / `iloc()` / the function prototypes; `erofs_fs.h` provides the on-disk format.
 * This header holds only what **upstream lacks and the adapter layer needs**:
 *
 *   - block layer: the prototypes of upstream `erofs_dev_read()` / `erofs_blk_read()` are in upstream
 *     `internal.h`, **the implementations are ours** (`glue/`), wired onto `br_bdev_ops_t`;
 *   - mount-time facts (`br_erofs_dev_t` / device name / mount point);
 *   - the shared scratch buffer;
 *   - fixture constants (byte-aligned with `tools/mk-fs-images.sh`, used by selftests).
 *
 * Dependency direction: this header lives in `glue/`, so it **may** include upstream `internal.h`; upstream does not know us.
 */
#ifndef BR_ADAPTER_UBOOT_EROFS_GLUE_H
#define BR_ADAPTER_UBOOT_EROFS_GLUE_H

#include "erofs_env.h"
#include "internal.h"          /* upstream fs/erofs/internal.h */

/* ---- mount-time facts ---- */
/* ★ ADR-0030: this artifact **no longer carries a device name** — the source device comes from the
 *   APP's [[mount]].source. The constant below is kept only as the selftest fixture name (matching
 *   the app's mount plan). */
#define BR_EROFS_DEV_NAME    "emmc3"      /* bdev name used by the selftest fixture */
#define BR_EROFS_MOUNT_PATH  "/assets"    /* mount point (two sources of truth: brick.toml [[mount]] and here) */
#define BR_EROFS_TYPE_NAME   "erofs"      /* for diagnostics */

/* Largest logical block this plugin supports. Upstream allows blkszbits ∈ [9,12], but the compressed
 * path only supports 4096 (`EROFS_MAX_BLOCK_SIZE` is given by upstream internal.h from PAGE_SIZE —
 * here we only supply the "bit count"). */
#define EROFS_BLOCK_SIZE_BITS 12u

/* Static upper bound for the "path relative to the mount point" in a transient inode (we do not use
 * BR_VFS_PATH_MAX: that one is private to vfs-core). */
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

/* Shared 4 KiB bounce buffer (the writer owns it, see the notes in erofs_vfs.c). */
u8  *uboot_erofs_scratch(void);

/* ---- fixture constants (byte-aligned with tools/mk-fs-images.sh) ----
 *
 * Selftests use these constants only for **generative** checks (byte i = some formula) and do not
 * embed large literals:
 *
 *   path               size    content (generative)               mkfs-observed datalayout
 *   ------------------------------------------------------------------------------
 *   /hello.txt          12     "erofs hello\n"                   2 = FLAT_INLINE
 *   /comp/lz4.txt    70000     "ABCDEFGH" repeated (i%8)         3 = compressed (LZ4)
 *   /raw.bin          8192     ((i*1103515245+12345)>>16)&0xFF   3 = compressed (LZ4, incompressible)
 *   /dir/a.bin        4096     byte i = i & 0xFF                 0 = FLAT_PLAIN
 *   /dir/sub/deep.txt    5     "deep\n"                          2 = FLAT_INLINE
 *
 * 70000 is not a multiple of 4096 ⇒ deliberately covers the "tail extent of a compressed file"
 * (partial decoding); raw.bin is incompressible (~50% ratio) ⇒ covers the other LZ4 path.
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

/* TC-EROFS-003's "mid-file read" window (deliberately not starting at 0: covers extent splitting at a non-zero offset) */
#define EROFS_FX_ABIN_MID_OFF    1000u
#define EROFS_FX_ABIN_MID_LEN    500u

#define EROFS_DIRENT_MAX_NAME    EROFS_NAME_LEN

/* EROFS dirent type code (ftype) → our br_dirent type. Upstream `namei.c` only has the opposite
 * direction (`erofs_ftype_from_mode` is not upstream); this branch is **our** integration need. */
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
