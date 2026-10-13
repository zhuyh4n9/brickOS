/* adapter/uboot-mmc shim for `<linux/list.h>` -> the vendor original: `mmc_legacy.c` really
 * walks the device chain (`INIT_LIST_HEAD`/`list_add_tail`/`list_for_each`/`list_entry`), so the
 * real implementation is required, not just layout-holding fields. Its closure takes
 * `<linux/stddef.h>` from `compat/` and `<linux/poison.h>` from the vendor.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_LIST_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_LIST_H
#include "../../../../third_party/uboot-imx/include/linux/list.h"
#endif
