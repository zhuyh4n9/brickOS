/* adapter/uboot-mmc shim for `<mmc.h>` -> the vendor original: `struct mmc` is upstream's own
 * protocol-core state (~30 fields, trimmed by 6 config switches) read and written directly by
 * `mmc.c`/`sdhci.c`; copying it would mean syncing it with the config matrix. Its own
 * `<linux/bitops.h> <linux/list.h> <linux/dma-direction.h> <cyclic.h> <part.h>` includes are all
 * satisfied by same-named `compat/` shims.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_MMC_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_MMC_H
#include "../../../third_party/uboot-imx/include/mmc.h"
#endif
