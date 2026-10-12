/*
 * brickOS prototype v0.2.0 — generic device contract (public surface of the
 * framework/dev-core plugin)
 *
 * Design basis:
 *   - `8-01-device.md` §1.1/§1.3 (layer split and framework ownership: **dev-core only cares
 *     that something "is a device"** — the single flat namespace / naming rule / call
 *     semantics and error model / ioctl encoding / **subclass registration protocol**; shape
 *     belongs to the subclass framework)
 *   - `8-01-device.md` §2 (registry entry `br_dev_class_entry_t` + **open_file hook**
 *     (D21/D23): the "device → file" adaptation entry returning {fops, fpriv}; the hook
 *     signature references `br_file_ops` ⇒ dev-core has a **type dependency** on vfs-core
 *     (header only, no init/call dependency, O-S7))
 *   - `8-01-device.md` §3 (dev-core **defines no concrete ops shape** — adding a subclass
 *     does not touch this file)
 *   - `8-01-device.md` §4 (error model = negative errno) / §5 (ioctl uses Linux-compatible
 *     32-bit encoding)
 * Per-item rulings and deviations: `docs/decisions/0009-vfs-storage-stack.md`.
 *
 * ## What this file does **not** do (the R-S4 line of defense)
 *   - does not define cdev/bdev/flash ops shapes (that is cdev-core/bdev-core's job);
 *   - does not own device lifecycle policy (devices appear when the driver calls
 *     `*_register`, with no explicit unregister — v1 static composition);
 *   - does no "policy": naming uniqueness/limits/error codes are **mechanism**, that is all.
 */
#ifndef BR_DEV_BR_DEV_H
#define BR_DEV_BR_DEV_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>      /* br_file_ops_t (return type of the open_file hook; type dependency, see O-S7) */

/* ==================================================================== naming and limits */

/* Device name rule (design `8-01` §2): `[a-z][a-z0-9]*`, no slash — the devfs node name is a
 * path component. The static bound reuses vfs's `BR_NAME_MAX` (one namespace should not have
 * two length truths). */
#define BR_DEV_NAME_MAX   BR_NAME_MAX
#define BR_DEV_MAX        16u   /* registry static bound (static-bound idiom of design 3-02 §14.6) */

/* ==================================================================== subclass ids */
/* `class_id`: shape interpretation belongs to the **subclass framework** (append-only, D14).
 * dev-core only passes it through. */
#define BR_CLASS_NONE     0u
#define BR_CLASS_CDEV     1u   /* framework/cdev-core (br_cdev_ops / br_flash_ops) */
#define BR_CLASS_BDEV     2u   /* framework/bdev-core (not landed; id reserved to avoid drift) */

/* ==================================================================== registry entry */

/*
 * One device (design `8-01` §2 as-is, plus why `name` is not in the struct):
 * the name is the registry **key** and is passed separately by `br_dev_add(name, entry)` (by
 * design).
 *
 * `open_file` (the D21/D23 hook): devfs calls it when looking up a device node; it returns
 * the file-surface ops and that surface's private data. **When it returns 0** devfs puts
 * {*fops, *fpriv} into the node inode.
 *   - the cdev subclass fills it from cdev-core (generic session adapter);
 *   - `BR_NULL` = this subclass **provides no** file surface (raw flash and bdev raw block
 *     access are v2) ⇒ devfs still lists the node, but `br_open` on it yields `-ENOTSUP`
 *     (honest: visible, not openable).
 */
typedef struct br_dev_class_entry {
    br_u16 class_id;      /* BR_CLASS_* */
    void  *class_priv;    /* subclass framework's ops table pointer (e.g. `const br_cdev_ops_t *`) */
    int  (*open_file)(void *dev_priv, const br_file_ops_t **fops, void **fpriv);
    void  *dev_priv;      /* driver private */
} br_dev_class_entry_t;

/* ==================================================================== register and lookup */

/*
 * Register one device (called internally by the subclass framework's `*_register`; drivers do
 * not call it directly).
 * Errors: `-EINVAL` (bad name/entry, or non-conforming name) / `-EEXIST` (duplicate name —
 * uniqueness is enforced here) / `-ENOSPC` (table full).
 * Why duplicate names are a **runtime** error rather than only a composition-time one: the
 * `[[res]]` domain catches resource conflicts, but device names are runtime strings (built by
 * the driver) and invisible at composition time — same shape as `br_service_publish`'s
 * `-EEXIST`.
 */
int br_dev_add(const char *name, const br_dev_class_entry_t *entry);

/* Look up by name (not registered ⇒ BR_NULL). Returns the **in-table copy**, lifetime = image. */
const br_dev_class_entry_t *br_dev_lookup(const char *name);

/* Enumeration (used by devfs readdir; order = registration order).
 * `br_dev_count()` count; `br_dev_name_at(i)` name (out of range ⇒ BR_NULL);
 * `br_dev_entry_at(i)` entry (out of range ⇒ BR_NULL). */
br_u32 br_dev_count(void);
const char *br_dev_name_at(br_u32 index);
const br_dev_class_entry_t *br_dev_entry_at(br_u32 index);

/* Name conformance (`[a-z][a-z0-9]*`, length 1..BR_DEV_NAME_MAX-1). Returns BR_TRUE/BR_FALSE.
 * Exported so a **subclass framework** can give the driver a clear error before calling
 * br_dev_add, instead of each writing its own regex. */
br_bool br_dev_name_valid(const char *name);

/* ==================================================================== ioctl encoding */
/*
 * Design `8-01` §5: same bit layout as Linux `_IOC` (zero learning cost + statically
 * checkable):
 *   dir[31:30] size[29:16] type[15:8] nr[7:0]
 * `type` = magic letter per device/driver family (one each, globally unique): 'u' uart /
 * 'c' can / 'b' bdev / 'f' flash / 'd' display …
 */
#define BR_IOC_NRBITS      8u
#define BR_IOC_TYPEBITS   8u
#define BR_IOC_SIZEBITS   14u
#define BR_IOC_DIRBITS    2u

#define BR_IOC_NRSHIFT    0u
#define BR_IOC_TYPESHIFT  (BR_IOC_NRSHIFT + BR_IOC_NRBITS)      /* 8 */
#define BR_IOC_SIZESHIFT  (BR_IOC_TYPESHIFT + BR_IOC_TYPEBITS)  /* 16 */
#define BR_IOC_DIRSHIFT   (BR_IOC_SIZESHIFT + BR_IOC_SIZEBITS)  /* 30 */

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

/* ==================================================================== conformance cases */

/* This file's conformance cases (self-assigned `TC-DEV-*`; registered in `br-wa-test-001`)
 * moved to `src/dev_selftest.c` (implementing `dev_core_selftest()`), driven uniformly by
 * core's `br_plugin_manager_selftest()` through the descriptor's `.selftest` hook (ADR-0010).
 * Why it was dropped from the public header: the test entry is **not** the plugin's golden
 * interface surface — it is the "self-proof" mechanism; consumers should not depend on it, and
 * it should not take part in interface hash/version governance (adding a case is not an
 * interface change). */

#endif /* BR_DEV_BR_DEV_H */
