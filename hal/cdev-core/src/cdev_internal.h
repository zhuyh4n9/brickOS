/*
 * brickOS prototype v0.2.0: cdev-core plugin-private contract (not exported). Audience: this
 * plugin's `src/cdev_selftest.c` only; living in `src/` keeps it off the public interface
 * surface. Exposes only the two accessors the cases need.
 */
#ifndef BR_CDEV_CORE_INTERNAL_H
#define BR_CDEV_CORE_INTERNAL_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

/* Shape of the open_file hook (= signature of `br_dev_class_entry_t.open_file`). A dedicated
 * typedef because the use is function-pointer identity comparison: object/function pointer
 * conversion is undefined in ISO C, so the types must match exactly. */
typedef int (*cdev_open_file_hook_t)(void *dev_priv, const br_file_ops_t **fops, void **fpriv);

/* The open_file hook `br_cdev_register` fills into cdev entries; forwards to the production
 * static `cdev_open_file` (its address, not a copy, so cases judge the same truth).
 * Use: TC-CDEV-004/005 -- recognize "our own entry" by pointer identity. */
cdev_open_file_hook_t cdev_internal_open_file_hook(void);

/* The generic session adapter's `br_file_ops` table (production static `s_cdev_file_ops`),
 * returned by address, not copied.
 * Use: TC-CDEV-006 -- assert forwarding slots complete and lseek/fsync/poll_attach empty. */
const br_file_ops_t *cdev_internal_file_ops(void);

#endif /* BR_CDEV_CORE_INTERNAL_H */
