/* adapter/uboot-mmc - shim for `<linux/poison.h>` (-> the **vendor original**).
 *
 * It is part of `include/linux/list.h`'s include closure (`LIST_POISON1/2`).
 * It is pure macros with no dependencies, so this forwards to the upstream original
 * instead of copying two constants.
 */#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_POISON_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_POISON_H
#include "../../../../third_party/uboot-imx/include/linux/poison.h"
#endif
