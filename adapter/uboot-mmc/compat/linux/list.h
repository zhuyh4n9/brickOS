/* adapter/uboot-mmc - shim for `<linux/list.h>` (-> the **vendor original**).
 *
 * Why forward instead of providing our own `struct list_head`: `mmc_legacy.c` (an upstream
 * original) really **walks** that device chain - `mmc_list_init()`/`mmc_list_add()`/
 * `find_mmc_device()` use `INIT_LIST_HEAD`/`list_add_tail`/`list_for_each`/`list_entry`. Those
 * are not fields provided "just so the layout holds" (as in erofs), so the **real
 * implementation** is required, and the upstream one is authoritative. (The opposite of the
 * "types only, no traversal" treatment in `adapter/uboot-erofs/compat/erofs_env.h` - the
 * difference is that **here something really traverses the list**.)
 *
 * Its include closure: `<linux/stddef.h>` and `<linux/poison.h>`. The former comes from
 * `compat/` (`NULL`/`offsetof`/`size_t`), the latter forwards to the vendor original.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_LIST_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_LIST_H
#include "../../../../third_party/uboot-imx/include/linux/list.h"
#endif
