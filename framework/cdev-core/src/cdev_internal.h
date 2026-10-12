/*
 * brickOS prototype v0.2.0 — cdev-core **plugin-private** internal contract (not exported, not in include/)
 *
 * Audience: this plugin's own `src/cdev_selftest.c`. Living in `src/` (not `include/`) is
 * deliberate: everything under `include/` is the **plugin's public surface** (governed by
 * `brickie check` interface domains, and visible to `descriptor_include`), while this header is
 * needed **only by the self-test**.
 *
 * ## Why the self-test reads internals
 *   Two criteria test **the internal mechanism itself**, which the public API cannot express:
 *     - TC-CDEV-004/005 judge by **pointer identity** whether a dev entry is the cdev
 *       registered by this component: the public surface only offers `br_cdev_get` (fetch ops
 *       by name), which cannot answer "is the open_file hook on the entry the one this
 *       component filled in" — and that is exactly the criterion for "cdev and flash subtypes
 *       share class_id but differ in file surface";
 *     - TC-CDEV-006 inspects the adapter layer's `br_file_ops` table **slot by slot**
 *       (lseek/fsync/poll_attach must be empty ⇒ calls get -ENOTSUP), but `s_cdev_file_ops` is
 *       static in `cdev_core.c`.
 *   Exposing these as **public API** would be worse: the adapter table's internal view would
 *   enter the golden interface surface, so later slot changes become interface changes.
 *
 * ## Boundary discipline (same origin as core's `*_internal.h` and vfs-core's `src/vfs_internal.h`)
 *   Only the two things the self-test really uses are exposed, each documented below; the whole
 *   slot pool (`s_cdevs`/`s_flash` and their counts) is **not** exposed: the self-test always
 *   observes the registry through the public API.
 */
#ifndef BR_CDEV_CORE_INTERNAL_H
#define BR_CDEV_CORE_INTERNAL_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>      /* br_file_ops_t (type of the adapter table pointer) */

/* Shape of the open_file hook (= signature of `br_dev_class_entry_t.open_file`).
 * Why a dedicated typedef instead of an accessor returning `void *`: converting between object
 * and function pointers is undefined in ISO C, and what is done here is exactly **function
 * pointer identity comparison** — the types must match exactly. */
typedef int (*cdev_open_file_hook_t)(void *dev_priv, const br_file_ops_t **fops, void **fpriv);

/* The open_file hook this component fills into the cdev entry in `br_cdev_register`
 * (production implementation is static `cdev_open_file`).
 * Use: TC-CDEV-004/005 — recognize "our own entry" by pointer identity, then run fetch/duplicate
 * name round trips on it.
 * ★ Forwards directly to the production implementation (returns its address) instead of copying
 *   a new function: otherwise the case compares a copy and "the hook filled at registration is
 *   the adapter entry" is never verified (see the boundary discipline in that header). */
cdev_open_file_hook_t cdev_internal_open_file_hook(void);

/* The generic session adapter's `br_file_ops` table (production implementation is static
 * `s_cdev_file_ops`).
 * Use: TC-CDEV-006 — assert slot by slot "forwarding slots complete, lseek/fsync/poll_attach empty".
 * ★ Also forwarded: returns `&s_cdev_file_ops`, so the case sees the very table `cdev_open_file`
 *   hands to devfs (not a copy). */
const br_file_ops_t *cdev_internal_file_ops(void);

#endif /* BR_CDEV_CORE_INTERNAL_H */
