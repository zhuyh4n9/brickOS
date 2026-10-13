/* driver/uboot-mmc shim for `<sdhci.h>` -> the vendor original: `struct sdhci_host` is
 * upstream's own host state, and glue also writes `flags` (runtime ADMA2/PIO switching),
 * `ioaddr`/`cfg`/`quirks` -- copying it would drift immediately. Its `<asm/io.h>`/`<asm/gpio.h>`/
 * `<linux/kernel.h>` includes are satisfied by `compat/`.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_SDHCI_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_SDHCI_H
#include "../../../third_party/uboot-imx/include/sdhci.h"
#endif
