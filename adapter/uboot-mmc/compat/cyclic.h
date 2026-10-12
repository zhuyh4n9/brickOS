/* adapter/uboot-mmc — adaptation shell for `<cyclic.h>` (a **faithful minimal** form of
 * the u-boot header name).
 *
 * ★ Why **not** vendor upstream `include/cyclic.h`: it `#include <u-boot/schedule.h>`,
 *   which in turn `#include <uthread.h>` — a whole family of thread scheduling
 *   facilities. This file does not enable `CONFIG_CYCLIC` (deliberately off in the
 *   matrix); with it off, the upstream header **already** degrades to "empty struct +
 *   three empty static inline functions"; this file is a line-by-line restatement of
 *   that shape (not an invention).
 *
 * Semantics (matching the upstream `#else` branch word for word):
 *   - `struct cyclic_info` is an **empty struct** — but `struct mmc` **has a field of
 *     it** (`cyclic`), so the type must exist (a zero-size member leaves struct offsets
 *     unchanged in GCC);
 *   - `cyclic_register()`/`cyclic_unregister()` are no-ops: the call site in upstream
 *     `mmc.c` sits inside
 *     `if (CONFIG_IS_ENABLED(CYCLIC, (!mmc->cyclic.func), (NULL)))`, and that condition
 *     is always false (expands to `if (NULL)`), so the upstream "card-detect polling"
 *     feature does **not participate** here. They must be `static inline`, otherwise the
 *     "always-false call site" would still demand the symbols at link time.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_CYCLIC_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_CYCLIC_H

#include "mmc_env.h"

/* Consistent with the `!CONFIG_CYCLIC` shape of upstream `include/cyclic.h` */
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
