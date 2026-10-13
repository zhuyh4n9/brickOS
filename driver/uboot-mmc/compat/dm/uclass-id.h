/* driver/uboot-mmc shim for `<dm/uclass-id.h>` -> the vendor original: `struct blk_desc`'s
 * first field is `enum uclass_id` and `mmc_legacy.c` writes `UCLASS_MMC`. It is a pure enum with
 * zero include closure, so the vendor original is used rather than 140 copied values.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_DM_UCLASS_ID_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_DM_UCLASS_ID_H
#include "../../../../third_party/uboot-imx/include/dm/uclass-id.h"
#endif
