/* fs/uboot-fat - forwarding shim for <rtc_def.h>, added as an original (the vendor tree
 * lacked it) and hash-pinned in third_party/upstream.lock. Vendored <fs.h> needs it
 * for the three struct rtc_time fields of struct fs_dirent; with CONFIG_DM_RTC=0 they matter
 * only for layout -- upstream fat_readdir fills them under CONFIG_EFI_LOADER only.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_RTC_DEF_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_RTC_DEF_H
#include "../../../third_party/uboot-imx/include/rtc_def.h"
#endif
