/* driver/uboot-mmc shim for `<memalign.h>` -> the vendor original: upstream `mmc.c:970` uses
 * `ALLOC_CACHE_ALIGN_BUFFER(u8, test_csd, MMC_MAX_BLOCK_LEN)` (DMA-aligned stack buffer for CMD6),
 * u-boot's own idiom. Its closure (`<linux/kernel.h>`/`<asm/cache.h>`/`<malloc.h>`) is in compat/.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_MEMALIGN_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_MEMALIGN_H
#include "../../../third_party/uboot-imx/include/memalign.h"
#endif
