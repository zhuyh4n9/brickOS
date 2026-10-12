/* adapter/uboot-mmc - shim for `<sdhci.h>` (-> the **vendor original**).
 *
 * `struct sdhci_host` is the upstream SDHCI host driver's own state (register window, the
 * `USE_ADMA` bit in `flags`, `adma_desc_table`, ...), and `sdhci.c`/`sdhci-adma.c` read and
 * write it directly. Our glue **also writes** its `flags` bits (runtime SET_PATH switching
 * between ADMA2 and PIO) plus `ioaddr`/`cfg`/`quirks` - that struct **is** the interface
 * (task doc §2.6). Copying it would drift immediately.
 *   => use the vendor copy byte for byte.
 *
 * It includes `<asm/io.h>`/`<asm/gpio.h>`/`<linux/kernel.h>` etc., all satisfied by `compat/`.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_SDHCI_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_SDHCI_H
#include "../../../third_party/uboot-imx/include/sdhci.h"
#endif
