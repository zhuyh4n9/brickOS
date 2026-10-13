/* adapter/uboot-mmc adaptation shell for `<cyclic.h>`, restating upstream's `!CONFIG_CYCLIC`
 * shape (that switch is deliberately off).
 *
 * `struct cyclic_info` is empty, but `struct mmc` has a field of it, so the type must exist (a
 * zero-size member leaves struct offsets unchanged in GCC). `cyclic_register()`/
 * `cyclic_unregister()` are no-ops: upstream's call site
 * `if (CONFIG_IS_ENABLED(CYCLIC, ...))` is always false here, so card-detect polling does not
 * participate. They must be `static inline`, else the always-false call site still demands the
 * symbols at link time.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_CYCLIC_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_CYCLIC_H

#include "mmc_env.h"

struct cyclic_info {
};

/** Function type for cyclic functions */
typedef void (*cyclic_func_t)(struct cyclic_info *c);

static inline void cyclic_register(struct cyclic_info *cyclic, cyclic_func_t func,
                                   uint64_t delay_us, const char *name)
{
    (void)cyclic;
    (void)func;
    (void)delay_us;
    (void)name;
}

static inline void cyclic_unregister(struct cyclic_info *cyclic)
{
    (void)cyclic;
}

static inline int cyclic_unregister_all(void)
{
    return 0;
}

#endif /* BR_ADAPTER_UBOOT_MMC_SHIM_CYCLIC_H */
