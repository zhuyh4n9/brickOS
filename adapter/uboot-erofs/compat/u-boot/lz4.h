/* adapter/uboot-erofs - shim for `<u-boot/lz4.h>`: routes to the upstream original in the
 * third_party tree (content neither copied nor modified). Relative, not -I into
 * third_party/uboot-imx/include: brickie orders a unit's -I flags lexicographically, so that
 * -I would sort before compat and shadow our shims (`<fs.h>`/`<part.h>` would then resolve to
 * the real u-boot headers, which need rtc_def.h etc. and fail to compile).
 */
#include "../../../../third_party/uboot-imx/include/u-boot/lz4.h"
