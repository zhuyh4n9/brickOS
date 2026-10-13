/*
 * Block device subclass contract (public surface of framework/bdev-core). A bdev registers into the
 * dev-core namespace as BR_CLASS_BDEV with open_file = BR_NULL (raw /dev access is v2); read/write
 * return sectors transferred or a negative errno, and flush must reach the medium. Ops are
 * self-locked by the driver; the surface is thread-only (ISR-unsafe).
 */
#ifndef BR_DEV_BR_BDEV_H
#define BR_DEV_BR_BDEV_H

#include <br/core/br_types.h>

/*
 * flags bits: READONLY = medium not writable (does not relax the required ops slots; the write path
 * returns -EROFS, access-mode enforcement belongs to v1 svc-posix); STACKED = provider is itself a
 * consumer (partition mapper / page cache). DMA/PIO report the actual data-path mode: at most one
 * may be set (both => -EINVAL at registration; neither = path not declared, allowed).
 */
#define BR_BDEV_F_READONLY  0x0001u
#define BR_BDEV_F_STACKED   0x0002u
#define BR_BDEV_F_DMA       0x0004u
#define BR_BDEV_F_PIO       0x0008u

typedef struct br_bdev_geom {
    br_u32 sector_size;   /* logical sector, usually 512 */
    br_u64 n_sectors;     /* total sectors on the medium */
    br_u32 max_rw;        /* max transfer (sectors; > 0 and <= n_sectors) */
    br_u32 align;         /* buffer alignment requirement, 0 = any (DMA-insensitive) */
    br_u32 flags;         /* BR_BDEV_F_* */
} br_bdev_geom_t;

/*
 * Block device ops. read/write(priv, lba, buf, n_lba): lba is in sectors (scaled by
 * geometry.sector_size); success returns the number of sectors transferred (0..n_lba, short reads
 * allowed), failure returns a negative errno. flush(priv) must reach the medium (page cache relies
 * on it); returning 0 is supported. geometry(priv, out) reports the current geometry on every call.
 * ioctl/suspend/resume are reserved slots and may be BR_NULL => callers get -ENOTSUP.
 */
typedef struct br_bdev_ops {
    int (*read)    (void *priv, br_u64 lba, void *buf, br_size_t n_lba);
    int (*write)   (void *priv, br_u64 lba, const void *buf, br_size_t n_lba);
    int (*flush)   (void *priv);
    int (*geometry)(void *priv, br_bdev_geom_t *g);
    int (*ioctl)   (void *priv, br_u32 cmd, void *arg);
    int (*suspend) (void *priv);
    int (*resume)  (void *priv);
} br_bdev_ops_t;

/* Register a block device (name = /dev/<name> in devfs, e.g. emmc0). read/write/flush/geometry are
 * all required; geometry is called once and checked for self-consistency. Errors: -EINVAL (bad
 * name/ops/geometry or DMA|PIO both set) / -EEXIST (duplicate name) / -ENOSPC (registry full). */
int br_bdev_register(const char *name, const br_bdev_ops_t *ops, void *priv);

/* Look up ops by name (binding point for FS/stacking plugins). Not registered => BR_NULL and *priv
 * is unchanged. */
const br_bdev_ops_t *br_bdev_get(const char *name, void **priv);

/* This file's conformance cases (TC-BDEV-*, registered in br-wa-test-001) live in
 * src/bdev_selftest.c, driven by core's br_plugin_manager_selftest() via .selftest. */

#endif /* BR_DEV_BR_BDEV_H */
