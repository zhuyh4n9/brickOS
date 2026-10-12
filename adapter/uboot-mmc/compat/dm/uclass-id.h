/* adapter/uboot-mmc - shim for `<dm/uclass-id.h>` (-> the **vendor original**).
 *
 * The first field of `struct blk_desc` is `enum uclass_id uclass_id;`, and `mmc_legacy.c`
 * writes `UCLASS_MMC`. This is a **pure enum with zero include closure** from upstream, so
 * per ADR-0026 D-3 it is "an upstream original that should be vendored"; copying 140 enum
 * values into `adapter/` would instead be "copying upstream sources into adapter/"
 * (forbidden by task doc §1). So this only forwards the path.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_DM_UCLASS_ID_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_DM_UCLASS_ID_H
#include "../../../../third_party/uboot-imx/include/dm/uclass-id.h"
#endif
