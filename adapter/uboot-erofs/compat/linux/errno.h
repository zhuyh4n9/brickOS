/* adapter/uboot-erofs - shim for u-boot header names.
 *
 * Upstream sources include **u-boot header names** such as `#include <linux/compat.h>`.
 * This file routes them to our environment implementation `erofs_env.h`, so upstream
 * sources stay byte-identical. Adaptation, not copying: names and environment only,
 * no upstream implementation.
 */
#ifndef BR_ADAPTER_UBOOT_EROFS_SHIM_LINUX_ERRNO_H
#define BR_ADAPTER_UBOOT_EROFS_SHIM_LINUX_ERRNO_H
#include "../erofs_env.h"
#endif
