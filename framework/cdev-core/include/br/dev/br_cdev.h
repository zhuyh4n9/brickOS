/*
 * brickOS prototype v0.2.0 — character device subclass contract (exported surface of the framework/cdev-core plugin)
 *
 * Design basis:
 *   - `8-01-device.md` §3 (`br_cdev_ops` session-style shape + `br_cdev_register`;
 *     **flash subtype** `br_flash_ops`/`br_flash_register`/`br_flash_get` — spi-nor/nand attach here)
 *   - `8-01-device.md` §3's **unified reserved slots (D22/SD-14)**: all device-class ops reserve
 *     `ioctl`/`suspend`/`resume` (NULL ⇒ -ENOTSUP); the file-side `poll`/`close` are provided by
 *     this unit's **generic br_file_ops adapter** (devfs obtains that adapter via the open_file hook)
 *   - `7-03-concrete-fs.md` §3 (cdev-core provides the generic session adapter: open creates session /
 *     read/write / ioctl / poll / close forwarding; lseek/fsync slots NULL ⇒ -ENOTSUP)
 *   - `8-01-device.md` §4 (SD-6 blocking semantics / ISR prohibition / SD-10 negative errno)
 * Per-item rulings and deviations: `docs/decisions/0009-vfs-storage-stack.md`.
 *
 * ## What this unit does **not** do
 *   - Knows no concrete device (uart/can/adc…): those are the `io/` plugins attaching to this unit;
 *   - Does no device lifecycle policy (register = online, no unregister — v1 static composition);
 *   - Does not leak cdev's "session" concept into dev-core (D20: generic and shape separated).
 */
#ifndef BR_DEV_BR_CDEV_H
#define BR_DEV_BR_CDEV_H

#include <br/core/br_types.h>
#include <br/dev/br_dev.h>
#include <br/vfs/br_vfs.h>

/* ==================================================================== cdev shape */

/*
 * Character device ops (design `8-01` §3 verbatim).
 *
 * Session model: `open(dev_priv, flags, &sess)` creates a session and yields an opaque `sess`;
 * afterwards read/write/ioctl/poll all carry `sess`; `close(sess)` destroys it. **Exclusivity policy
 * belongs to the driver** (uart-like ones return -EBUSY themselves in open; design `7-01` §1 last paragraph).
 * `suspend`/`resume` are **device-level** (take `dev_priv`, not session-level) PM hooks (D22 reserved;
 * v1 has no unified caller — O-S6). NULL ⇒ -ENOTSUP.
 */
typedef struct br_cdev_ops {
    int     (*open) (void *dev_priv, br_u32 flags, void **sess);
    br_s64  (*read) (void *sess, void *buf, br_size_t n);
    br_s64  (*write)(void *sess, const void *buf, br_size_t n);
    int     (*ioctl)(void *sess, br_u32 cmd, void *arg);
    int     (*poll) (void *sess, br_u32 *events);
    int     (*close)(void *sess);
    int     (*suspend)(void *dev_priv);
    int     (*resume) (void *dev_priv);
} br_cdev_ops_t;

/*
 * Register a character device (device name = `/dev/<name>` in devfs).
 * Semantics: validate the name (`br_dev_name_valid`) → enter the dev-core registry with
 *        `class_id = BR_CLASS_CDEV`, and **auto-fill this unit's open_file hook** (cdev's generic
 *        session adapter) ⇒ the driver only implements ops.
 * Errors: `-EINVAL` (bad name/ops) / `-EEXIST` (duplicate name) / `-ENOSPC` (registry full).
 * Required ops slots: open/close (missing ⇒ -EINVAL); read/write/ioctl/poll may be null (⇒ call gets -ENOTSUP).
 */
int br_cdev_register(const char *name, const br_cdev_ops_t *ops, void *dev_priv);

/* Form B (not linked to vfs-core) direct accessor (design `8-01` §1.2): fetch ops and driver private by name.
 * Not registered ⇒ ops returns BR_NULL and `*dev_priv` is unchanged. */
const br_cdev_ops_t *br_cdev_get(const char *name, void **dev_priv);

/* ==================================================================== flash subtype */
/*
 * flash is a **character-type medium** (program/erase by address, no disk-style sector abstraction,
 * design SD-2) ⇒ belongs to the cdev subclass as a **subtype**; its shape is 1:1 with littlefs's
 * `lfs_config` (zero-glue binding, `7-03` §4).
 */
typedef struct br_flash_geom {
    br_u32 read_size;     /* minimum read granularity */
    br_u32 prog_size;     /* program page size */
    br_u32 block_size;    /* erase block */
    br_u32 block_count;
} br_flash_geom_t;

typedef struct br_flash_ops {
    int (*read)   (void *priv, br_u32 addr, void *buf, br_size_t n);
    int (*program)(void *priv, br_u32 addr, const void *buf, br_size_t n);
    int (*erase)  (void *priv, br_u32 block_idx);
    int (*sync)   (void *priv);
    /* D22 reserved: ioctl (NOR deep power-down/feature control) + suspend/resume (power management) */
    int (*ioctl)  (void *priv, br_u32 cmd, void *arg);
    int (*suspend)(void *priv);
    int (*resume) (void *priv);
} br_flash_ops_t;

/*
 * Register a flash device.
 * ★ **v1 fills no open_file hook** (BR_NULL): the file surface of raw flash is not a v1 goal
 *   — design `7-03` §3 says outright "bdev's raw block access (/dev/blk0) hook = v2 (v1 sets NULL,
 *   FS binds via `br_bdev_get`-class API)". So `/dev/nor0` will **appear but not open** (-ENOTSUP):
 *   visible-but-unopenable is more honest than "invisible" (ADR-0009 §3 ruling 6).
 */
int br_flash_register(const char *name, const br_flash_ops_t *ops,
                      const br_flash_geom_t *geom, void *priv);

/* Fetch flash ops and geometry by name (form B / littlefs binding point). Not registered ⇒ BR_NULL. */
const br_flash_ops_t *br_flash_get(const char *name, void **priv);
int br_flash_geom_get(const char *name, br_flash_geom_t *out);

/* ==================================================================== conformance cases */

/* This unit's conformance cases (own numbering `TC-CDEV-*`; registered in `br-wa-test-001`) have moved to
 * `src/cdev_selftest.c` (implementing `cdev_core_selftest()`), driven uniformly by core's
 * `br_plugin_manager_selftest()` via the descriptor's `.selftest` hook (ADR-0010).
 * Why removed from the public header: a test entry is **not** a plugin's golden interface surface — it is
 * this unit's self-verification mechanism; consumers should not depend on it, nor should it take part in interface
 * hash/version governance (adding a case should not be an interface change). */

#endif /* BR_DEV_BR_CDEV_H */
