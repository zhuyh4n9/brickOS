/* adapter/uboot-fat — shim for `<linux/compiler.h>` (not a forward).
 *
 * ★ This **deliberately does not** forward the vendor `include/linux/compiler.h`, because of
 *   a compiler-forced fact (measured with probes): that header includes
 *   `<linux/compiler_types.h>` → `<linux/compiler_attributes.h>`
 *   → `<linux/compiler-gcc.h>`/`<asm/compiler.h>` → …, plus `<linux/types.h>` →
 *   `<linux/posix_types.h>`/`<asm/types.h>`/`<stdbool.h>`. The latter drags in the whole
 *   u-boot **type layer** (u8/u16/uintN_t/bool/ulong), which collides head-on with this
 *   environment (`fat_env.h`, the u-boot runtime environment the adapter provides) —
 *   whereas ADR-0026 D-3 assigns the "types/macros" layer explicitly to `compat/`.
 *
 *   ⇒ The boundary is drawn here: **on-disk formats and structures**
 *     (`include/fat.h`/`include/fs.h`/`include/rtc_def.h`) forward to the vendor originals
 *     (that is where "content has a single source of truth"); **types and compile-time
 *     attribute macros** come from this environment (upstream actually uses only
 *     `__maybe_unused` from `<linux/compiler.h>`). This is the direct consequence of
 *     "adaptation = giving upstream the environment it depends on", registered in NOTICE's
 *     deviation list.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_LINUX_COMPILER_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_LINUX_COMPILER_H
#include "../fat_env.h"
#endif
