/* adapter/uboot-fat — a **forwarding shell** for `<fat.h>` (not a copy).
 *
 * The .c files under upstream `fs/fat` write `#include <fat.h>` to get the FAT **on-disk
 * structures** (boot_sector/volume_info/dir_entry/dir_slot/nameext/fsdata plus attribute bits and
 * predicate macros). An earlier revision **copied** those structures into the plugin (exactly what
 * ADR-0026 §5 calls out) ⇒ now it **forwards to the vendor original**: content has a single truth
 * (byte-for-byte upstream, guarded by the hashes in `adapter/third-party-upstream.lock`), and we
 * copy not one field.
 *
 * ★ Why a relative path instead of `-I third_party/uboot-imx/include`:
 *   brickie orders each compile unit's `-I` flags **lexicographically** (`build.rs`'s
 *   unit_includes), and the `.` in `../…/third_party/…` sorts before `compat` ⇒ the `-I` pointing
 *   at the vendor include would **shadow** the same-named shell in this directory (`<fs.h>`/
 *   `<part.h>` would resolve to u-boot's real headers, which in turn need `<asm/byteorder.h>`/
 *   `<rtc_def.h>` and the like). An explicit relative path has no such ambiguity (same technique
 *   and same reason as `adapter/uboot-erofs/compat/u-boot/lz4.h`).
 *
 * ★ Environment first: upstream `include/fat.h` itself includes `<fs.h>` and `<asm/byteorder.h>`,
 *   so we wire up `fat_env.h` (the environment implementation) before forwarding — the reverse
 *   order would leave `FAT2CPU16/32`/`__u16` and the like in the vendor header undefined.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_FAT_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_FAT_H
#include "fat_env.h"
#include "../../../third_party/uboot-imx/include/fat.h"
#endif
