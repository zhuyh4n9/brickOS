/* adapter/uboot-mmc - **standard header name** `<time.h>` unit-level environment patch (see the deviation notes in brick.toml)
 *
 * Upstream `mmc.c` line 18 / `sdhci.c` line 17 both literally `#include <time.h>` (u-boot gets
 * `get_timer()`/`timer_get_us()` from it). This is a bare-metal unikernel with no libc `<time.h>`.
 *
 * Why not put it in `compat/` and list it in `includes`: the reason is identical to the sibling
 * `errno.h` (a standard header name collides with `runtime/posix/include/time.h`, which would be
 * misreported by brickie's name-level include-surface check). The full explanation and regression
 * conditions are in the `compat/std-names/errno.h` header, the `[build]` comment in `brick.toml`
 * and the "deviations" section of `ADAPTER-NOTES.md`.
 *
 * Semantics: `get_timer()`/`timer_get_us()`/`time_after()`/`udelay()`/`mdelay()` all live in
 * `mmc_env.h` - they are this file's **core technique** (turning upstream polling into
 * interrupt-driven sleep waits).
 */
#ifndef BR_ADAPTER_UBOOT_MMC_STDNAME_TIME_H
#define BR_ADAPTER_UBOOT_MMC_STDNAME_TIME_H
#include "../mmc_env.h"
#endif
