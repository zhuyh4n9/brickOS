/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * adapter/uboot-fat — plugin-internal contract (**single** declaration site; not exported)
 *
 * ============================== layering and file roles ==============================
 * After ADR-0026 D-2/D-3 this plugin has only three kinds of things:
 *   `brick.toml` sources     upstream `third_party/uboot-imx/fs/fat/fat_write.c`
 *                            (**the original file is compiled**: its line 23 does
 *                              `#include "fat.c"`, so one translation unit holds both
 *                              read+write; do **not** compile fat.c separately, it would
 *                              duplicate symbols)
 *   compat/                  u-boot runtime environment (header-name shim + `fat_env.h`);
 *   glue/                    this project's binding:
 *       fat_blk.c    block-layer shim (`blk_dread/blk_dwrite` -> `br_bdev_ops_t`) + volume
 *                    table + BPB observation (mount criterion) + the few "we implement
 *                    these" environment bits (public-API builds of
 *                    `strdup`/`sprintf`/`rand`/truncate);
 *       fat_vfs.c    VFS four-layer ops + lifecycle hooks;
 *       fat_selftest.c  TC-FAT-001..008.
 * This header is the contract inside glue (layout/constants/prototypes); it exports **no**
 * public API.
 *
 * ============================== differences from the previous version (ADR-0026) ==============================
 *   · The `src/` layer is **gone**: the upstream original is compiled directly, so the
 *     plugin holds no "rewritten upstream";
 *   · The previous version's `fsdata per volume` deviation is **revoked**: each upstream API
 *     carries its own `fsdata` (with its own malloc/free semantics), and glue calls only
 *     **public APIs** => there is no longer a "FAT cache kept across calls", and hence no
 *     "every entry point must remember to refresh" invariant. Volume-level sync is just
 *     `bdev.flush` (each upstream write entry point already commits the FAT window and
 *     directory entry internally, see the two flushes in `file_fat_write_at`);
 *   · The previous version's 160-line `uboot_fat_truncate()` rewrite is **deleted**:
 *     truncation is built only from upstream public APIs (see the function header of
 *     `uboot_fat_truncate`: shrinking/to 0 is upstream `file_fat_write_at`'s own
 *     truncate/extend to size (upstream has no truncate, only one "truncate the rest"
 *     branch on the write path).
 *
 * ============================== ownership rules ==============================
 * Design SD-3: this plugin's inodes are **transient path wrappers** — every lookup/create
 * builds a `uboot_fat_ino_t` with br_malloc (holding {volume id, path relative to the mount
 * point, type, size}), and `free_inode` must br_free it. **Only the mount root inode is an
 * exception**: mount builds it with br_malloc and `uboot_fat_fs_t` holds it long-term
 * (vfs never hands the root to free_inode), but it still must not go into BSS — the
 * "heap allocation / explicit release" line must hold throughout.
 *
 * ============================== volumes and mounts ==============================
 * Three volumes each mount one independent block device (registered by adapter/uboot-mmc
 * in its start phase):
 *   emmc0 = 2 MiB  FAT12 -> /fat12
 *   emmc1 = 16 MiB FAT16 -> /fat16
 *   emmc2 = 32 MiB FAT32 -> /fat32
 * Mounting happens in **this plugin's own start phase** (`[[dep]] kind = "init"` guarantees
 * phase order). The volume-id-to-mount-point mapping is a **compile-time constant table**
 * (see `s_vols` in fat_blk.c), not runtime probing.
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_FAT_GLUE_H
#define BR_ADAPTER_UBOOT_FAT_GLUE_H

/* The upstream .c and glue both take the runtime environment via the **environment
 * header**; glue additionally uses the vendored on-disk structures directly
 * (`<fat.h>` is compat's forwarding shim — not a third-party header; gate criterion ⑤ see
 * tools/check-third-party-pristine.sh). */
#include "fat_env.h"
/* This plugin is a **filesystem** plugin: the common inode/file header (`br_inode_t`) and
 * the four-layer ops types come from the VFS contract. The upstream .c does not need it
 * (upstream only knows path strings), so it lives outside compat — only glue consumes it. */
#include <br/vfs/br_vfs.h>

/* Upstream FAT's **public API surface** (`fat_exists`/`fat_size`/`fat_read_file`/
 * `fat_opendir`/`fat_readdir`/`fat_closedir`/`file_fat_write`/`fat_unlink`/`fat_mkdir`/
 * `fat_rename`/`fat_set_blk_dev`/`clust_to_sect`/...) — declared in the vendor
 * `include/fat.h` (via compat's forwarding shim). Glue goes only through these entry
 * points, so "the upstream original is compiled as-is" also holds on the **call surface**. */
#include <fat.h>

/* ---- entry point exported by upstream but **not** declared in the vendored `<fat.h>` ----
 * `file_fat_write_at()` is a public function of upstream `fs/fat/fat_write.c` (used by
 * u-boot's `fs/fs.c` for the "positioned write" of `fs_write`) but is not written into
 * `include/fat.h`. The vendor header is a **byte-for-byte upstream original** (must not be
 * edited), and glue needs to consume it => the prototype lives in the adapter layer (same
 * signature as upstream; having a prototype also covers -Wmissing-prototypes).
 * Because it exists, this plugin's truncation does **not** need to modify upstream (see
 * uboot_fat_truncate in fat_blk.c). */
int file_fat_write_at(const char *filename, loff_t pos, void *buffer, loff_t size,
                      loff_t *actwrite);

/* ============================================ volume facts (compile-time constants) */

#define UB_FAT_VOL_MAX   3u      /* how many volumes this plugin mounts (matches [[mount]] count in brick.toml) */

/* Block device names (dev-core naming rule `[a-z][a-z0-9]*`) */
/* ADR-0030: the source device name is no longer decided here (the APP's [[mount]].source
 *   gives it); the three below remain only as self-test fixture names, consistent with the
 *   app's mount plan. */
#define UB_FAT_DEV12     "emmc0"
#define UB_FAT_DEV16     "emmc1"
#define UB_FAT_DEV32     "emmc2"

/*
 * Mount point and device name constants — **self-test fixture names only** (consistent with
 * the `[[mount]]` entries in `app/hello/brick.toml`).
 *
 * Since ADR-0030 "which card mounts at which mount point" is given by the **APP's
 *   `[[mount]]`**: this file only registers the driver `"fat"`, and at mount time it
 *   **claims a volume slot** by `(cfg->dev, cfg->path)` (`uboot_fat_vol_claim`).
 *   The previous three-line mount-point<->device table here and the manifest were **two
 *   sources of truth** (debt br-wa-fs-001); after the generation pipeline landed and the
 *   table was removed, that debt is repaid — these constants thus degrade to self-test
 *   fixture names.
 */
#define UB_FAT_MNT12     "/fat12"
#define UB_FAT_MNT16     "/fat16"
#define UB_FAT_MNT32     "/fat32"

/* Volume id (array index; order = mount order) */
#define UB_FAT_VOL12     0u
#define UB_FAT_VOL16     1u
#define UB_FAT_VOL32     2u

/* Volume FAT type (determined from the BPB at power-on, a **runtime fact**; these three
 * are the self-test expectations) */
#define UB_FAT_TYPE12    12
#define UB_FAT_TYPE16    16
#define UB_FAT_TYPE32    32

/* Cluster-count upper bound for FAT12 (upstream `fs/fat/fat.c` has a same-named file-scope
 * macro `MAX_FAT12`; that one is static/translation-unit-internal and glue cannot reference
 * it => declared again here from the **on-disk fact**, with its source noted). */
#define UB_FAT_MAX_FAT12 0xFF4u

/*
 * Volume sector size. The whole plugin hardcodes "512 B sectors" — because **upstream
 * assumes the same**: `boot_sector.sector_size` must equal the block device's `blksz`
 * (upstream `get_fs_info` criterion), and this plugin's three emmc devices are always 512;
 * at mount time `sect_size != 512` is treated as -EINVAL (better to refuse the mount than do
 * 4 KiB sector arithmetic). Per-I/O sector upper bound = block device `geometry.max_rw`
 * (measured 64 sectors); the shim splits chunks internally.
 */
#define UB_FAT_SECTOR_SIZE   512u

/* Name upper bound (this repo's BR_NAME_MAX = 32, br_dirent_t.name[32]).
 * A long name (LFN) whose byte count exceeds it => BR_ERR(ENOSPC) (same rule as fs/tmpfs). */
#define UB_FAT_NAME_MAX      BR_NAME_MAX

/* Joined path buffer upper bound (mount point <= 8 + '/' + name <= 31 + '\0' = 40; 256 left
 * for future deep paths). It is also the `strdup` bound in the environment
 * (Ub_FAT_STRDUP_CAP in compat/fat_env.h). */
#define UB_FAT_PATH_MAX      256u

/* Zero-byte chunk appended per step when growing (truncate's "extend" direction) (on stack, 512 B) */
#define UB_FAT_ZERO_CHUNK    512u

/* ============================================ host-provided content
 *
 * The files in the three volumes are built by the host in a post-build gate (see the
 * "prerequisite artifacts" section of the README). These constants are this plugin's
 * **contract** with that artifact's shape: if the host builds a different shape,
 * TC-FAT-002/003 go red — which is exactly the criterion we want (rather than "the
 * self-test builds its own data and verifies it").
 */
#define UB_FAT_C12_HELLO       "fat12 hello\n"
#define UB_FAT_C16_HELLO       "fat16 hello\n"
#define UB_FAT_C32_HELLO       "fat32 hello\n"

/* Byte count and fill character of `/dir/a.bin` (see the image-building script contract in the README) */
#define UB_FAT_C12_DIR_A_BYTES 16u
#define UB_FAT_C16_DIR_A_BYTES 256u
#define UB_FAT_C32_DIR_A_BYTES 4096u

/* Over-long (> BR_NAME_MAX-1 = 31 bytes) LFN file name: verifies the "long name => -ENOSPC" rule */
#define UB_FAT_C32_LFN_NAME    "long name file.txt"

/* ============================================ volume table and block layer */

/*
 * Volume (bdev + geometry + **BPB observation**). **Entry point for device I/O**: upstream's
 * `blk_dread/blk_dwrite` find it via `s_cur_vol`, so "volume" appears in the plugin only in
 * the shim, in one place.
 *
 * `fatsize`/`sect_size`/`clust_size`/`total_sect` etc. are **observations parsed from the
 * BPB at mount time** (upstream `get_fs_info` is static with no external observation point
 * => glue parses the vendored `boot_sector` layout itself; it is **used only for criteria
 * and logging**, and takes part in no I/O decision — all I/O is in upstream's hands).
 */
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
    br_u32               data_begin;   /* sector of the first cluster (cluster 2) (may be a "negative cluster" for non-FAT32) */
    br_u32               root_cluster; /* FAT32 root directory first cluster */
} uboot_fat_vol_t;

/* GET_INFO-style observation (mount criterion + self-test; the production path goes only
 * through internal volume-table accessors) */
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

/*
 * **Transient inode** (design SD-3): one is built per lookup/create and released by
 * free_inode. `head` must be the first field (CA-2: vfs knows only br_inode_t*, and a node
 * must be usable as soon as it is obtained). Path = path **relative to the mount point**,
 * with the root directory being "" (empty string) — so joining is `mnt + "/" + path` and
 * does not rely on the error-prone "does the path have a leading '/'" convention.
 */
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
    br_inode_t      *root;            /* root inode (br_malloc; held by the mount table, never free_inode'd) */
} uboot_fat_fs_t;

/* ---- volume bind/unbind/query (fat_blk.c) ---- */
int  uboot_fat_vol_bind(uboot_fat_vol_t *vol);
void uboot_fat_vol_unbind(void);
uboot_fat_vol_t *uboot_fat_vol_get(br_u32 id);
int  uboot_fat_vol_claim(const char *dev, const char *mnt, br_u32 *id);
int  uboot_fat_vol_info(br_u32 id, uboot_fat_vol_info_t *out);

/*
 * Truncate/extend to size (upstream has no truncate, only one "truncate the rest" branch on
 * the write path). **Built only from upstream public APIs** (see the implementation and
 * function header in fat_blk.c), no longer the previous version's 160-line rewrite.
 */
int uboot_fat_truncate(const char *path, br_u64 size);

/* ============================================ internal view of the VFS binding
 * These are not a "public API" (this plugin exports none) but are needed between
 * fat_selftest.c / fat_vfs.c. */
int  uboot_fat_mount_vol_info(const char *mnt, uboot_fat_vol_info_t *out);
int  uboot_fat_join_path(const char *dir_path, const char *name, char *out, br_size_t cap);
br_bool uboot_fat_name_ok(const char *name);
int  uboot_fat_test_sync(const char *mnt);

#endif /* BR_ADAPTER_UBOOT_FAT_GLUE_H */
