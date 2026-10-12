/* adapter/uboot-mmc — `<linux/kconfig.h>` 的适配壳(→ **vendor 原件**)。
 *
 * ★ 这是本插件里少数几个**转发到上游原件**的壳之一(另一个是 `mmc.h`/`sdhci.h`)。
 *   理由: `CONFIG_IS_ENABLED()`/`IS_ENABLED()`/`CONFIG_VAL()` 这套"把未定义的配置当 0"
 *   的粘贴机制是 u-boot 的**权威实现**(`__ARG_PLACEHOLDER_1` 那套), 而它正是本件
 *   "配置矩阵的陷阱"那一节的裁判。自己抄一份 = 第二处真值 + 抄错就静默走错分支。
 *   ⇒ `third_party/uboot-imx/include/linux/kconfig.h` 逐字节原样, 这里只做路径转发。
 *
 * ★ 它第一行就是 `#include <generated/autoconf.h>` —— 于是本插件的配置矩阵落在
 *   `compat/generated/autoconf.h`(u-boot 真实构建里那个文件的角色)。
 *
 * ★ 为什么用相对路径而不是 -I 到 third_party/uboot-imx/include: 见 `compat/config.h`。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_KCONFIG_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_KCONFIG_H
#include "../../../../third_party/uboot-imx/include/linux/kconfig.h"
#endif
