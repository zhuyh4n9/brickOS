/*
 * brickOS prototype v0.2.0: generic device contract (framework/dev-core public surface). One
 * flat device namespace: naming rule, registry, lookup/enumeration, Linux-compatible ioctl
 * encoding. No ops shape, no lifecycle policy (register = online, no unregister in v1).
 */
#ifndef BR_DEV_BR_DEV_H
#define BR_DEV_BR_DEV_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

/* ---- naming and limits ---- */

/* Device name rule: [a-z][a-z0-9]*, no slash (the devfs node name is a path component).
 * BR_DEV_NAME_MAX reuses vfs's BR_NAME_MAX -- one namespace, one length truth. */
#define BR_DEV_NAME_MAX   BR_NAME_MAX
#define BR_DEV_MAX        16u

/* ---- subclass ids ---- */
/* class_id: shape interpretation belongs to the subclass framework (append-only); dev-core
 * passes it through. BR_CLASS_CDEV = framework/cdev-core (br_cdev_ops / br_flash_ops);
 * BR_CLASS_BDEV = framework/bdev-core (not landed, id reserved to avoid drift). */
#define BR_CLASS_NONE     0u
#define BR_CLASS_CDEV     1u
#define BR_CLASS_BDEV     2u

/*
 * One device. `name` is the registry key, passed separately to br_dev_add(name, entry).
 * open_file: devfs calls it on node lookup; on 0 it stores {*fops, *fpriv} in the node inode.
 * BR_NULL = no file surface (raw flash/bdev are v2) => the node is listed but br_open yields
 * -ENOTSUP.
 */
typedef struct br_dev_class_entry {
    br_u16 class_id;      /* BR_CLASS_* */
    void  *class_priv;    /* subclass framework's ops table pointer (e.g. `const br_cdev_ops_t *`) */
    int  (*open_file)(void *dev_priv, const br_file_ops_t **fops, void **fpriv);
    void  *dev_priv;      /* driver private */
} br_dev_class_entry_t;

/* ---- register and lookup ---- */

/*
 * Register one device (called by the subclass framework's `*_register`, not by drivers).
 * Errors: -EINVAL (bad name/entry) / -EEXIST (duplicate name, uniqueness enforced here) /
 * -ENOSPC (table full). Device names are runtime strings invisible at composition time, hence
 * a runtime -EEXIST.
 */
int br_dev_add(const char *name, const br_dev_class_entry_t *entry);

/* Look up by name; not registered => BR_NULL. Returns the in-table copy, lifetime = image. */
const br_dev_class_entry_t *br_dev_lookup(const char *name);

/* Enumeration for devfs readdir, in registration order. Out-of-range index => BR_NULL for both
 * br_dev_name_at and br_dev_entry_at. */
br_u32 br_dev_count(void);
const char *br_dev_name_at(br_u32 index);
const br_dev_class_entry_t *br_dev_entry_at(br_u32 index);

/* Name conformance (`[a-z][a-z0-9]*`, length 1..BR_DEV_NAME_MAX-1) => BR_TRUE/BR_FALSE.
 * Exported so a subclass framework can reject a bad name before calling br_dev_add. */
br_bool br_dev_name_valid(const char *name);

/* ---- ioctl encoding ---- */
/*
 * Same bit layout as Linux _IOC (statically checkable):
 *   dir[31:30] size[29:16] type[15:8] nr[7:0]
 * `type` = magic letter per device family (globally unique): 'u' uart / 'c' can / 'b' bdev /
 * 'f' flash / 'd' display ...
 */
#define BR_IOC_NRBITS      8u
#define BR_IOC_TYPEBITS   8u
#define BR_IOC_SIZEBITS   14u
#define BR_IOC_DIRBITS    2u

#define BR_IOC_NRSHIFT    0u
#define BR_IOC_TYPESHIFT  (BR_IOC_NRSHIFT + BR_IOC_NRBITS)
#define BR_IOC_SIZESHIFT  (BR_IOC_TYPESHIFT + BR_IOC_TYPEBITS)
#define BR_IOC_DIRSHIFT   (BR_IOC_SIZESHIFT + BR_IOC_SIZEBITS)

#define BR_IOC_NONE       0u
#define BR_IOC_WRITE      1u
#define BR_IOC_READ       2u

#define BR_IOC(dir, type, nr, size) \
    (((br_u32)(dir)  << BR_IOC_DIRSHIFT)  | \
     ((br_u32)(type) << BR_IOC_TYPESHIFT) | \
     ((br_u32)(nr)   << BR_IOC_NRSHIFT)   | \
     ((br_u32)(size) << BR_IOC_SIZESHIFT))

#define BR_IOR(type, nr, size)   BR_IOC(BR_IOC_READ,  (type), (nr), (br_u32)sizeof(size))
#define BR_IOW(type, nr, size)   BR_IOC(BR_IOC_WRITE, (type), (nr), (br_u32)sizeof(size))
#define BR_IOWR(type, nr, size)  BR_IOC(BR_IOC_READ | BR_IOC_WRITE, (type), (nr), (br_u32)sizeof(size))
#define BR_IO(type, nr)          BR_IOC(BR_IOC_NONE,  (type), (nr), 0u)

#define BR_IOC_DIR(cmd)   (((cmd) >> BR_IOC_DIRSHIFT)  & ((1u << BR_IOC_DIRBITS)  - 1u))
#define BR_IOC_TYPE(cmd)  (((cmd) >> BR_IOC_TYPESHIFT) & ((1u << BR_IOC_TYPEBITS) - 1u))
#define BR_IOC_NR(cmd)    (((cmd) >> BR_IOC_NRSHIFT)   & ((1u << BR_IOC_NRBITS)   - 1u))
#define BR_IOC_SIZE(cmd)  (((cmd) >> BR_IOC_SIZESHIFT) & ((1u << BR_IOC_SIZEBITS) - 1u))

/* TC-DEV-* conformance cases live in src/dev_selftest.c, driven by core's
 * br_plugin_manager_selftest() via the descriptor's .selftest hook. */

#endif /* BR_DEV_BR_DEV_H */
