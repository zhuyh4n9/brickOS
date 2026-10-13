/* driver/uboot-mmc shim for `<linux/stddef.h>` -> `mmc_env.h`: `include/linux/list.h` needs
 * `NULL`/`offsetof`. The upstream header is not vendored (it pulls in `<linux/compiler_types.h>`
 * and the `<linux/compiler-gcc.h>` family); the compiler's own `<stddef.h>` provides them.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_STDDEF_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_STDDEF_H
#include "../mmc_env.h"
#endif
