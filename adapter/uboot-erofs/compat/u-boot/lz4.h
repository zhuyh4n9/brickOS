/* adapter/uboot-erofs - shim for `<u-boot/lz4.h>`.
 *
 * Upstream `fs/erofs/decompress.c` writes `#include <u-boot/lz4.h>`. This file **routes it to
 * the upstream original in the third_party tree - so we neither copy its content nor modify it.
 *
 * Why relative paths instead of -I into third_party/uboot-imx/include: brickie orders each
 * unit's `-I` flags **lexicographically** (see unit_includes in build.rs), and the `..` in
 * `adapter/uboot-erofs/../../third_party/...` sorts before `compat` => third_party/include
 * would **shadow** our compat shims (`<fs.h>`/`<part.h>` would resolve to the real u-boot
 * headers, which in turn need rtc_def.h etc. and fail to compile). Explicit relative paths
 * avoid the ambiguity.
 */
#include "../../../../third_party/uboot-imx/include/u-boot/lz4.h"
