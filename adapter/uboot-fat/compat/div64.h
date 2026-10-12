/* adapter/uboot-fat - shim for the u-boot header name `<div64.h>`.
 *
 * Upstream sources include **u-boot header names** such as `#include <div64.h>`.
 * This file routes them to our environment implementation `fat_env.h`, so upstream sources
 * stay byte-identical. Adaptation, not copying: names and environment only, no upstream
 * implementation.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_DIV64_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_DIV64_H
#include "fat_env.h"
#endif
