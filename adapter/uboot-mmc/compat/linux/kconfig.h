/* adapter/uboot-mmc - shim for `<linux/kconfig.h>` (-> the **vendor original**).
 *
 * This is one of the few shims in this plugin that **forward to the upstream original** (the
 * others are `mmc.h`/`sdhci.h`). Reason: the `CONFIG_IS_ENABLED()`/`IS_ENABLED()`/
 * `CONFIG_VAL()` paste machinery that treats undefined configs as 0 is u-boot's
 * **authoritative implementation** (the `__ARG_PLACEHOLDER_1` family), and it is exactly the
 * referee for the "configuration matrix trap" section here. Copying it would create a second
 * source of truth that silently takes the wrong branch when copied wrong. => use
 * `third_party/uboot-imx/include/linux/kconfig.h` byte for byte; this only forwards the path.
 *
 * Its first line is `#include <generated/autoconf.h>` - so this plugin's configuration matrix
 * lives in `compat/generated/autoconf.h` (the role that file plays in a real u-boot build).
 *
 * Why relative paths instead of -I into third_party/uboot-imx/include: see `compat/config.h`.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_KCONFIG_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_KCONFIG_H
#include "../../../../third_party/uboot-imx/include/linux/kconfig.h"
#endif
