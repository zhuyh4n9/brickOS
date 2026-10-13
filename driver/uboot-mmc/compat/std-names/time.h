/* driver/uboot-mmc unit-level environment patch for the standard header name `<time.h>`
 * (upstream `mmc.c`/`sdhci.c` include it literally). Same `std-names/` rationale as the sibling
 * `errno.h`: a name-level include-surface collision with `runtime/posix/include/time.h`.
 * `get_timer()`/`timer_get_us()`/`time_after()`/`udelay()`/`mdelay()` all live in `mmc_env.h`.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_STDNAME_TIME_H
#define BR_ADAPTER_UBOOT_MMC_STDNAME_TIME_H
#include "../mmc_env.h"
#endif
