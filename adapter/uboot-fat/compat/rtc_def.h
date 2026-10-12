/* adapter/uboot-fat - **forwarding shim** for `<rtc_def.h>` (not a copy).
 *
 * The vendored `include/fs.h` itself does `#include <rtc_def.h>` (it uses `struct rtc_time`
 * for the three timestamp fields of `struct fs_dirent`). The vendor tree did not have this
 * header, so under ADR-0026 it was **added as an original**: taken byte for byte from
 * `build/uboot-ref/uboot.tar.gz` with a sha256 entry added to
 * `adapter/third-party-upstream.lock`.
 * This build has no RTC (`CONFIG_DM_RTC=0`), so those three fields are used only by the
 * **layout** of `struct fs_dirent` - upstream `fat_readdir` fills them only under
 * `CONFIG_EFI_LOADER`.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_RTC_DEF_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_RTC_DEF_H
#include "../../../third_party/uboot-imx/include/rtc_def.h"
#endif
