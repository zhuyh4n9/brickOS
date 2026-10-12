/* adapter/uboot-mmc - shim for `<linux/stddef.h>` (-> `mmc_env.h`).
 *
 * The first line of `include/linux/list.h` is `#include <linux/stddef.h>`; it needs `NULL`
 * and `offsetof`. This file does not vendor the upstream header - that one also pulls in
 * `<linux/compiler_types.h>` -> the whole `<linux/compiler_attributes.h>`/
 * `<linux/compiler-gcc.h>` family (which in turn wants `COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW`
 * and friends), while we only need the two things the compiler's own `<stddef.h>` already
 * provides, plus `size_t`.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_STDDEF_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_STDDEF_H
#include "../mmc_env.h"
#endif
