/* adapter/uboot-mmc - shim for `<memalign.h>` (-> the **vendor original**).
 *
 * Why forward instead of providing those macros ourselves: upstream `mmc.c:970` uses
 * `ALLOC_CACHE_ALIGN_BUFFER(u8, test_csd, MMC_MAX_BLOCK_LEN)` (preparing a **DMA-aligned
 * stack buffer** for CMD6 in mc_set_card_speed). Those macros (stack array + aligned slice)
 * are exactly u-boot's "DMA-safe stack buffer" idiom - copying them would be "copying
 * upstream sources into adapter/". Its include closure (`<linux/kernel.h>`/`<asm/cache.h>`/
 * `<malloc.h>`) is all satisfied by compat/.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_MEMALIGN_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_MEMALIGN_H
#include "../../../third_party/uboot-imx/include/memalign.h"
#endif
