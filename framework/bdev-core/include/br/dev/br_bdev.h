/*
 * brickOS prototype v0.2.0 — block device subclass contract (public surface of the
 * framework/bdev-core plugin)
 *
 * Design basis:
 *   - `7-storage/7-02-bdev.md` §1 (`br_bdev_geom_t` / `br_bdev_ops` / `br_bdev_register` /
 *     `br_bdev_get` aligned field by field; success = **number of sectors transferred**;
 *     failure = SD-10 negative errno)
 *   - `8-device/8-01-device.md` §1.3 (bdev-core depends on dev-core: block devices register
 *     into the dev-core namespace), §3 (bdev subclass = sector read/write/flush + geometry;
 *     the D20 shape split)
 *   - `7-storage/7-02-bdev.md` §2 (SD-9 partition mapper = v1.x, designed here but not
 *     implemented), §3 (page cache is a stacking plugin; this file only supplies the
 *     same-shape groundwork so stacking is invisible to the layer above)
 * Per-item rulings and deviations: `docs/decisions/0021-bdev-pci-emmc.md`.
 *
 * ## What this file does **not** do (the R-S4 line of defense)
 *   - knows no concrete medium (eMMC/NOR/virtio-blk): those are `io/` or third-party plugins
 *     that attach to this file;
 *   - does no partition parsing (MBR/GPT and `br_bdev_partition` are v1.x, SD-9);
 *   - does no cache policy (page cache is a **stacking plugin**, SD-8; this file only
 *     guarantees the "same `br_bdev_ops`" shape);
 *   - does no device lifecycle policy (register = online, no unregister — v1 static
 *     composition, same as dev-core/cdev-core).
 *
 * ## raw block access (D21 / `7-02` §1 last paragraph)
 *   In v1 block devices are bound to FSes through the **class API** (`br_bdev_get`);
 *   `/dev/<name>` raw file access is **v2**. So the bdev entry's `open_file` hook is
 *   **always `BR_NULL`** (visible, not openable, same as flash). This has a mechanical
 *   criterion (TC-BDEV-006: walk the registry and assert bdev entries' open_file is always
 *   NULL).
 */
#ifndef BR_DEV_BR_BDEV_H
#define BR_DEV_BR_BDEV_H

#include <br/core/br_types.h>

/* ==================================================================== geometry */

/*
 * `flags`: the design only said "stackable/read-only/...", so the bits are fixed here.
 * **This change adds the DMA/PIO bits** (the controller requires data transfers to state
 * DMA/PIO; see ADR-0021 D-1/D-4).
 *
 *   READONLY declares "medium not writable": a semantic bit that does not relax the required
 *            slots at registration (the ops shape must still be complete; the write path
 *            should return -EROFS — access-mode enforcement belongs to v1 svc-posix, see
 *            ADR-0009 §5).
 *   STACKED  the provider is itself a consumer (marker for partition mapper / page cache,
 *            SD-8/SD-9).
 *   DMA/PIO  the **actual** mode of the data path: `geometry` reports the current value each
 *            time, and **at most one** may be set (both = contradiction ⇒ -EINVAL at
 *            registration; neither = "path not declared", allowed but not observable).
 */
#define BR_BDEV_F_READONLY  0x0001u
#define BR_BDEV_F_STACKED   0x0002u
#define BR_BDEV_F_DMA       0x0004u
#define BR_BDEV_F_PIO       0x0008u

typedef struct br_bdev_geom {
    br_u32 sector_size;   /* logical sector, usually 512 (design text) */
    br_u64 n_sectors;     /* total sectors on the medium */
    br_u32 max_rw;        /* max transfer (sectors; > 0 and <= n_sectors) */
    br_u32 align;         /* buffer alignment requirement, 0 = any (DMA-insensitive) */
    br_u32 flags;         /* BR_BDEV_F_* */
} br_bdev_geom_t;

/* ==================================================================== bdev shape */

/*
 * Block device ops (design `7-02` §1 as-is; types respelled with this repo's `br_*`).
 *
 * Call semantics:
 *   - `read/write(priv, lba, buf, n_lba)`: success returns the **number of sectors
 *     transferred** (0..n_lba; short reads allowed), failure returns `BR_ERR(BR_E*)`.
 *     **lba is in sectors** and is converted to bytes via `geometry.sector_size`.
 *   - `flush(priv)`: **must reach the medium** (page cache depends on it, SD-8); swallowing
 *     it is not allowed. Returning 0 is supported.
 *   - `geometry(priv, out)`: reports the current geometry on every call (may change with
 *     runtime mode switches).
 *   - `ioctl/suspend/resume`: D22 reserved slots; `BR_NULL` ⇒ callers get `-ENOTSUP`.
 *     The slots **exist** because the ops layout is in the golden interface (D14/D22):
 *     adding fields later = binary incompatibility.
 *
 * Concurrency (D22/`8-01` §4): device ops are **self-locked by the driver**; bdev-core takes
 * no locks (it is a registry, not a request queue). Context: device/storage-domain APIs are
 * **thread-only** (ISR prohibition, `8-01` §4); interrupt-driven drivers must complete ops in
 * the bottom half/thread, not in the ISR.
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

/*
 * Register a block device (device name = `/dev/<name>` in devfs, e.g. `emmc0`).
 * Semantics: validate the name (`br_dev_name_valid`) → call `geometry` once and check the
 * geometry is self-consistent → insert into the dev-core registry with
 * `class_id = BR_CLASS_BDEV` (`open_file = BR_NULL`, D21).
 * Errors: `-EINVAL` (bad name/ops/geometry, or DMA|PIO both set) / `-EEXIST` (duplicate name)
 *         / `-ENOSPC` (registry full).
 * Required `ops` slots: `read` / `write` / `flush` / `geometry` are all required (a block
 * medium has no "not flushed" notion — a read-only device declares `BR_BDEV_F_READONLY` and
 * must still provide the write slot).
 */
int br_bdev_register(const char *name, const br_bdev_ops_t *ops, void *priv);

/* Direct accessor for form B (no vfs-core link) (design `7-02` §1: "for FS plugin binding").
 * Not registered ⇒ ops returns BR_NULL and `*priv` is unchanged. */
const br_bdev_ops_t *br_bdev_get(const char *name, void **priv);

/* ==================================================================== conformance cases */

/* This file's conformance cases (self-assigned `TC-BDEV-*`; registered in `br-wa-test-001`)
 * live in `src/bdev_selftest.c` (implementing `bdev_core_selftest()`), driven uniformly by
 * core's `br_plugin_manager_selftest()` through the descriptor's `.selftest` hook (ADR-0010).
 * Why the test entry was removed from the public header: it is not the plugin's golden
 * interface surface (adding a case is not an interface change). */

#endif /* BR_DEV_BR_BDEV_H */
