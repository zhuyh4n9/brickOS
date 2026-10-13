/* fs/uboot-fat - shim for <linux/compiler.h>; deliberately does not forward the vendor
 * header. Its chain (linux/compiler_types.h -> compiler_attributes.h -> compiler-gcc.h, plus
 * <linux/types.h>) drags in u-boot's whole type layer (u8/u16/uintN_t/bool/ulong), which
 * collides with fat_env.h. Types and attribute macros stay in this environment; upstream uses
 * only __maybe_unused from here.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_LINUX_COMPILER_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_LINUX_COMPILER_H
#include "../fat_env.h"
#endif
