/* adapter/uboot-mmc - shim for the u-boot header name `<linux/compiler.h>`.
 *
 * Upstream `drivers/mmc/` sources use **u-boot header names** (e.g. `#include <log.h>`).
 * This file routes them to our environment implementation `mmc_env.h`, so upstream sources
 * stay byte-identical. It gives upstream the environment it expects, and copies nothing.
 *
 * Why relative paths instead of `-I third_party/uboot-imx/include`:
 *   brickie orders each unit's `-I` flags **lexicographically**, and `../..` precedes
 *   `compat`, so the vendor include dir would **shadow** same-named shims under `compat/`
 *   (`<mmc.h>`/`<part.h>` would resolve to the real u-boot headers, which pull in the whole
 *   `cyclic.h`/`efi.h` family). Relative paths have no such ambiguity (same treatment as
 *   `adapter/uboot-erofs/compat/u-boot/lz4.h`).
 */
#ifndef BR_ADAPTER_UBOOT_MMC_LINUX_COMPILER_H_H_
#define BR_ADAPTER_UBOOT_MMC_LINUX_COMPILER_H_H_
#include "../mmc_env.h"
#endif
