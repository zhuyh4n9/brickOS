/* driver/uboot-mmc unit-level environment patch for the standard header name `<errno.h>`
 * (upstream `mmc.c:15` includes it literally; this unikernel has no libc one).
 *
 * It lives in `compat/std-names/` and is deliberately NOT listed in `includes`: brickie's
 * include-surface criterion is name-level and cannot see per-unit `-I`, so a standard-name header
 * in `includes` would be misjudged as included by `runtime/posix` (false positive). It ships via
 * one `-Idriver/uboot-mmc/compat/std-names` in `[build].cflags`, which only affects this
 * plugin's units. Move it back to `compat/` once that criterion resolves per-unit `-I`.
 *
 * Semantics: only forwards, no second source of truth (`br_error.h` already carries the names).
 */
#ifndef BR_ADAPTER_UBOOT_MMC_STDNAME_ERRNO_H
#define BR_ADAPTER_UBOOT_MMC_STDNAME_ERRNO_H
#include "../mmc_env.h"
#endif
