/* adapter/uboot-fat - shim for the u-boot header name `<rand.h>`.
 *
 * Upstream sources include **u-boot header names** such as `#include <rand.h>`.
 * This file routes them to our environment implementation `fat_env.h`, so upstream sources
 * stay byte-identical. Adaptation, not copying: names and environment only, no upstream
 * implementation.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_RAND_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_RAND_H
#include "fat_env.h"
#endif
