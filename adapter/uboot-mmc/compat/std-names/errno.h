/* adapter/uboot-mmc — unit-level environment patch for the **standard header name** `<errno.h>` (see the deviation note in brick.toml)
 *
 * ============================== Why it lives in `std-names/` and not `compat/` ==============================
 * The upstream original in `drivers/mmc/` has the literal `#include <errno.h>` (u-boot `mmc.c` line 15).
 * This is a bare-metal unikernel with no libc `<errno.h>` ⇒ we must provide one.
 *
 * But `errno.h` is a **standard header name**, and `runtime/posix/include/errno.h` uses it too. brickie's
 * include-surface criterion (ADR-0018) works at the **name level**: it builds a "name → owning plugin" map
 * from the file names under each plugin's `[build].includes` directories, then checks whose literal include
 * hits — it **cannot see per-unit `-I`**. So putting this file in `compat/` (declared in `includes`) would
 * make `runtime/posix/src/posix.c` and `iface/posix`'s `<errno.h>` be judged as "included a header from
 * adapter/uboot-mmc" (7 BRV-MF-0001 entries) — yet that directory is not in their `-I` at all, so it is a
 * **false positive**.
 *
 * ⇒ Handling (same source of truth as the `[build]` comment in `brick.toml` and the "deviations" section of
 *   `ADAPTER-NOTES.md`): put the shells for the two **standard header names** in `compat/std-names/`, **not
 *   listed in `includes`**, and ship them via one `-Iadapter/uboot-mmc/compat/std-names` in
 *   `[build].cflags` — that `-I` only affects this plugin's translation units, so "the name stays out of the
 *   declaration surface while units still find the header".
 *   Regression condition: when brickie's include-surface criterion resolves by **per-unit `-I`** (or
 *   explicitly excludes standard C header names), move these two shells back to `compat/` and list them in
 *   `includes`.
 *
 * ★ Semantics: upstream uses exactly those bare names, and ADR-0027 already renamed errno to the POSIX
 *   names, colocated with the values in `br/core/br_error.h` ⇒ this file **only forwards** (no second
 *   source of truth).
 */
#ifndef BR_ADAPTER_UBOOT_MMC_STDNAME_ERRNO_H
#define BR_ADAPTER_UBOOT_MMC_STDNAME_ERRNO_H
#include "../mmc_env.h"
#endif
