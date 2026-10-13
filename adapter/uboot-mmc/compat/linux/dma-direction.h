/* adapter/uboot-mmc shim for `<linux/dma-direction.h>`: routes it to `mmc_env.h`, so upstream
 * `drivers/mmc/` sources stay byte-identical. Relative paths instead of
 * `-I third_party/uboot-imx/include`: brickie orders `-I` lexicographically and `../..` precedes
 * `compat`, so the vendor dir would shadow these shims.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_LINUX_DMA_DIRECTION_H_H_
#define BR_ADAPTER_UBOOT_MMC_LINUX_DMA_DIRECTION_H_H_
#include "../mmc_env.h"
#endif
