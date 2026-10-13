/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * adapter/uboot-fat - plugin-internal contract (single declaration site; no public API exported).
 *
 * Layering: brick.toml compiles upstream third_party/uboot-imx/fs/fat/fat_write.c only -- its
 * line 23 does `#include "fat.c"`, so one translation unit holds read+write; compiling fat.c
 * separately would duplicate symbols. compat/ is the u-boot runtime environment (header-name
 * shims + fat_env.h). glue/ is this project's binding: fat_blk.c (block-layer shim + volume
 * table + BPB observation + the environment bits we implement: strdup/sprintf/rand/truncate),
 * fat_vfs.c (VFS four-layer ops + lifecycle hooks), fat_selftest.c (TC-FAT-001..008).
 *
 * Ownership: inodes are transient path wrappers -- each lookup/create br_mallocs a
 * uboot_fat_ino_t and free_inode must br_free it. Only the mount root inode is an exception:
 * mount br_mallocs it and uboot_fat_fs_t holds it long-term (vfs never passes the root to
 * free_inode), but it still never lives in BSS.
 *
 * Volumes: emmc0 2 MiB FAT12 -> /fat12, emmc1 16 MiB FAT16 -> /fat16, emmc2 32 MiB FAT32 ->
 * /fat32, each on its own block device registered by adapter/uboot-mmc. Mounting runs in this
 * plugin's own start phase ([[dep]] kind = "init" fixes the order); the id-to-mount-point
 * mapping is the compile-time table s_vols in fat_blk.c, not runtime probing.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_GLUE_H
#define BR_ADAPTER_UBOOT_FAT_GLUE_H

/* Runtime environment for upstream .c and glue alike; glue also uses the vendored on-disk
 * structures via compat's forwarding shim <fat.h> (not a third-party header). */
#include "fat_env.h"
/* Filesystem plugin: br_inode_t and the four-layer ops types come from the VFS contract; only
 * glue consumes them (upstream knows path strings only), so this include stays outside compat. */
#include <br/vfs/br_vfs.h>

/* Upstream FAT's public API surface (fat_exists/fat_size/fat_read_file/fat_opendir/fat_readdir/
 * fat_closedir/file_fat_write/fat_unlink/fat_mkdir/fat_rename/fat_set_blk_dev/clust_to_sect/...);
 * glue calls only these entry points, so upstream is used as-is on the call surface too. */
#include <fat.h>

/* ---- entry point exported by upstream but absent from the vendored <fat.h> ----
 * file_fat_write_at() is public in fs/fat/fat_write.c (u-boot's fs/fs.c does the positioned write
 * with it) but was never added to include/fat.h. That header is a byte-for-byte upstream original,
 * so the prototype lives here with upstream's signature (also covers -Wmissing-prototypes). */
int file_fat_write_at(const char *filename, loff_t pos, void *buffer, loff_t size,
                      loff_t *actwrite);

/* ---- volume facts (compile-time constants) ---- */

#define UB_FAT_VOL_MAX   3u      /* mounted volumes; matches brick.toml's [[mount]] count */

/* Block device names (dev-core rule [a-z][a-z0-9]*). The source device is the APP's
 * [[mount]].source; these three are self-test fixture names matching the app's mount plan. */
#define UB_FAT_DEV12     "emmc0"
#define UB_FAT_DEV16     "emmc1"
#define UB_FAT_DEV32     "emmc2"

/* Mount points: self-test fixture names only, matching the [[mount]] entries in
 * app/hello/brick.toml. Which card mounts where is the APP's [[mount]]; this plugin registers
 * the "fat" driver and claims a volume slot by (cfg->dev, cfg->path) at mount time. */
#define UB_FAT_MNT12     "/fat12"
#define UB_FAT_MNT16     "/fat16"
#define UB_FAT_MNT32     "/fat32"

/* Volume id (array index; order = mount order) */
#define UB_FAT_VOL12     0u
#define UB_FAT_VOL16     1u
#define UB_FAT_VOL32     2u

/* FAT type from the BPB at power-on (a runtime fact); these are the self-test expectations. */
#define UB_FAT_TYPE12    12
#define UB_FAT_TYPE16    16
#define UB_FAT_TYPE32    32

/* FAT12 cluster-count upper bound (upstream fs/fat/fat.c has a file-scope MAX_FAT12, but it is
 * translation-unit-internal, so re-declared here from the on-disk fact). */
#define UB_FAT_MAX_FAT12 0xFF4u

/* Volume sector size: upstream assumes the same (boot_sector.sector_size must equal the device's
 * blksz, the get_fs_info criterion) and these emmc devices are always 512 B. sect_size != 512 at
 * mount time is -EINVAL; per-I/O chunks are bounded by geometry.max_rw (measured 64). */
#define UB_FAT_SECTOR_SIZE   512u

/* Name upper bound (BR_NAME_MAX = 32, br_dirent_t.name[32]); an LFN longer than this gives
 * BR_ERR(ENOSPC), as in fs/tmpfs. */
#define UB_FAT_NAME_MAX      BR_NAME_MAX

/* Joined path buffer bound: mount point <= 8 + '/' + name <= 31 + '\0' = 40, leaving 256 for
 * deeper paths. Also the environment's strdup bound (UB_FAT_STRDUP_CAP in compat/fat_env.h). */
#define UB_FAT_PATH_MAX      256u

/* Zero-byte chunk per grow step (truncate extend direction); 512 B on stack */
#define UB_FAT_ZERO_CHUNK    512u

/* ---- host-provided content ---- */
/* The host builds the volumes' files in a post-build gate (README). A different shape turns
 * TC-FAT-002/003 red, which is the intended criterion. */
#define UB_FAT_C12_HELLO       "fat12 hello\n"
#define UB_FAT_C16_HELLO       "fat16 hello\n"
#define UB_FAT_C32_HELLO       "fat32 hello\n"

/* Byte count of /dir/a.bin (README documents the image-building script contract). */
#define UB_FAT_C12_DIR_A_BYTES 16u
#define UB_FAT_C16_DIR_A_BYTES 256u
#define UB_FAT_C32_DIR_A_BYTES 4096u

/* Over-long (> BR_NAME_MAX-1 = 31 bytes) LFN file name: verifies the "long name => -ENOSPC" rule */
#define UB_FAT_C32_LFN_NAME    "long name file.txt"

/* ---- volume table and block layer ---- */

/* Volume: bdev + geometry + BPB observation; the device-I/O entry point, found by upstream's
 * blk_dread/blk_dwrite via s_cur_vol. The BPB fields are parsed at mount time (upstream get_fs_info
 * is static) and feed criteria and logging only -- no I/O decision depends on them. */
typedef struct uboot_fat_vol {
    const char          *dev;          /* block device name (constant) */
    const char          *mnt;          /* mount point (constant) */
    const br_bdev_ops_t *ops;          /* ops table of the bdev class API */
    void                *priv;         /* bdev private handle */
    br_bdev_geom_t       geom;         /* geometry at registration (sector size/count/max_rw) */
    br_bool              bound;        /* whether the volume is bound (mount succeeded) */
    /* ---- the following are BPB observations (filled by uboot_fat_vol_probe in fat_blk.c) ---- */
    int                  fatsize;      /* 12 / 16 / 32 */
    br_u32               sect_size;    /* sector bytes (must == geom.sector_size) */
    br_u32               clust_size;   /* cluster = how many sectors */
    br_u32               total_sect;   /* total sectors in the volume */
    br_u32               fats;         /* number of FAT copies */
    br_u32               fat_sect;     /* FAT start sector */
    br_u32               fatlength;    /* sectors per FAT copy */
    br_u32               rootdir_sect; /* root directory start sector */
    br_u32               rootdir_size; /* root directory sector count for non-FAT32 */
    br_u32               data_begin;   /* cluster 2 sector; may be "negative" for non-FAT32 */
    br_u32               root_cluster; /* FAT32 root directory first cluster */
} uboot_fat_vol_t;

/* GET_INFO-style observation for the mount criterion and the self-test. */
typedef struct uboot_fat_vol_info {
    const char *dev;
    const char *mnt;
    br_u32      sector_size;
    br_u64      n_sectors;
    br_u32      max_rw;
    br_u32      clust_size_sectors;
    br_u32      max_clust;             /* data-area cluster count (computed from the BPB) */
    int         fatsize;               /* 12 / 16 / 32 */
    br_bool     mounted;
} uboot_fat_vol_info_t;

/* Transient inode: built per lookup/create, released by free_inode. `head` must be first -- vfs
 * knows only br_inode_t* and must use the node immediately. `path` is relative to the mount point
 * (root = ""), so joining is mnt + "/" + path, never depending on a leading '/'. */
typedef struct uboot_fat_ino {
    br_inode_t head;                  /* common header (type / iops / fops / fpriv) */
    br_u8      vol;                   /* volume id (UB_FAT_VOL*) */
    br_u8      type;                  /* BR_INODE_DIR / BR_INODE_FILE */
    br_u32     size;                  /* file byte count (directory = 0); snapshotted at lookup */
    char       path[UB_FAT_PATH_MAX]; /* relative to the mount point; root = "" */
} uboot_fat_ino_t;

/* Single-volume mount body (mount's fs_priv; held long-term by the mount table) */
typedef struct uboot_fat_fs {
    uboot_fat_vol_t *vol;
    br_inode_t      *root;            /* root inode: br_malloc'd, never free_inode'd */
} uboot_fat_fs_t;

/* ---- volume bind/unbind/query (fat_blk.c) ---- */
int  uboot_fat_vol_bind(uboot_fat_vol_t *vol);
void uboot_fat_vol_unbind(void);
uboot_fat_vol_t *uboot_fat_vol_get(br_u32 id);
int  uboot_fat_vol_claim(const char *dev, const char *mnt, br_u32 *id);
int  uboot_fat_vol_info(br_u32 id, uboot_fat_vol_info_t *out);

/* Truncate/extend to `size`, built only from upstream public APIs (upstream has no truncate,
 * just one "truncate the rest" branch on the write path); see fat_blk.c. */
int uboot_fat_truncate(const char *path, br_u64 size);

/* Internal view of the VFS binding: not a public API (this plugin exports none), but shared. */
int  uboot_fat_mount_vol_info(const char *mnt, uboot_fat_vol_info_t *out);
int  uboot_fat_join_path(const char *dir_path, const char *name, char *out, br_size_t cap);
br_bool uboot_fat_name_ok(const char *name);
int  uboot_fat_test_sync(const char *mnt);

#endif /* BR_ADAPTER_UBOOT_FAT_GLUE_H */
