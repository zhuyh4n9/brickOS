/*
 * brickOS prototype v0.2.0: character device subclass contract (hal/cdev-core public
 * surface). cdev ops are session-shaped; the open_file hook is this unit's generic file adapter
 * (lseek/fsync empty => -ENOTSUP); flash is a subtype. No concrete-device knowledge; register =
 * online, no unregister in v1.
 */
#ifndef BR_DEV_BR_CDEV_H
#define BR_DEV_BR_CDEV_H

#include <br/core/br_types.h>
#include <br/dev/br_dev.h>
#include <br/vfs/br_vfs.h>

/* ---- cdev shape ---- */

/*
 * Character device ops. open(dev_priv, flags, &sess) creates the opaque `sess` used by
 * read/write/ioctl/poll; close(sess) destroys it. Exclusivity belongs to the driver (open
 * returns -EBUSY); suspend/resume are device-level PM hooks taking dev_priv. NULL => -ENOTSUP.
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

/* Register a character device (`/dev/<name>`): validates the name, adds a BR_CLASS_CDEV entry to
 * dev-core, installs this unit's session adapter as the open_file hook. Errors: -EINVAL (bad
 * name/ops or missing open/close) / -EEXIST / -ENOSPC; NULL read/write/ioctl/poll => -ENOTSUP. */
int br_cdev_register(const char *name, const br_cdev_ops_t *ops, void *dev_priv);

/* Form B direct accessor: fetch ops and driver private by name. Not registered => BR_NULL,
 * `*dev_priv` unchanged. */
const br_cdev_ops_t *br_cdev_get(const char *name, void **dev_priv);

/* Flash is a cdev subtype, a character-type medium (program/erase by address, no sector
 * abstraction); the geometry shape is 1:1 with littlefs's lfs_config. */
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
    /* Reserved: ioctl = NOR feature control; suspend/resume = power management */
    int (*ioctl)  (void *priv, br_u32 cmd, void *arg);
    int (*suspend)(void *priv);
    int (*resume) (void *priv);
} br_flash_ops_t;

/*
 * Register a flash device. v1 installs no open_file hook (BR_NULL): `/dev/<name>` appears but
 * br_open on it gets -ENOTSUP (raw flash has no file face in v1).
 */
int br_flash_register(const char *name, const br_flash_ops_t *ops,
                      const br_flash_geom_t *geom, void *priv);

/* Fetch flash ops/geometry by name (littlefs binding). Not registered => BR_NULL / -ENODEV. */
const br_flash_ops_t *br_flash_get(const char *name, void **priv);
int br_flash_geom_get(const char *name, br_flash_geom_t *out);

/* TC-CDEV-* conformance cases live in src/cdev_selftest.c, driven by core's
 * br_plugin_manager_selftest() via the descriptor's .selftest hook. */

#endif /* BR_DEV_BR_CDEV_H */
