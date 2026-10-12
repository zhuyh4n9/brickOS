/* adapter/uboot-mmc — `<linux/poison.h>` 的适配壳(→ **vendor 原件**)。
 *
 * `include/linux/list.h` 的 include 闭合集里就有它(`LIST_POISON1/2`)。
 * 它是纯宏、无依赖, 所以直接转发上游原件, 不自己抄两个常量。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_POISON_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_POISON_H
#include "../../../../third_party/uboot-imx/include/linux/poison.h"
#endif
