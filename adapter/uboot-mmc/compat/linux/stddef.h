/* adapter/uboot-mmc — `<linux/stddef.h>` 的适配壳(→ `mmc_env.h`)。
 *
 * `include/linux/list.h` 的第一行就是 `#include <linux/stddef.h>`, 它需要 `NULL` 与
 * `offsetof`。本件不 vendor 上游那份 —— 它还会拉 `<linux/compiler_types.h>` →
 * `<linux/compiler_attributes.h>`/`<linux/compiler-gcc.h>` 一整族(那族又要求
 * `COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW` 之类), 而我们只需要编译器自带的
 * `<stddef.h>` 已经给的那两个东西 + `size_t`。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_STDDEF_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_STDDEF_H
#include "../mmc_env.h"
#endif
