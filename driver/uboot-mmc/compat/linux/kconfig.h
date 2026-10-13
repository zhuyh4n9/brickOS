/* driver/uboot-mmc shim for `<linux/kconfig.h>` -> the vendor original: the
 * `CONFIG_IS_ENABLED()`/`IS_ENABLED()`/`CONFIG_VAL()` paste machinery that treats undefined
 * configs as 0 is u-boot's authoritative implementation, and copying it would create a second
 * source of truth that silently takes the wrong branch. Its first line includes our
 * `generated/autoconf.h` config matrix.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_KCONFIG_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_KCONFIG_H
#include "../../../../third_party/uboot-imx/include/linux/kconfig.h"
#endif
