/* adapter/uboot-fat — `<rtc_def.h>` 的**转发壳**(不是副本)。
 *
 * vendored `include/fs.h` 自己 `#include <rtc_def.h>`(它要用 `struct rtc_time` 作为
 * `struct fs_dirent` 的三个时间戳字段)。vendor 树里本来没有这个头, 于是按 ADR-0026
 * 的纪律**新增原件**: 逐字节取自 `build/uboot-ref/uboot.tar.gz`, 并在
 * `adapter/third-party-upstream.lock` 里补了 sha256。
 * 本件 v1 没有 RTC(`CONFIG_DM_RTC=0`), 于是那三个字段只被 `struct fs_dirent` 的**布局**
 * 用到 —— 上游 `fat_readdir` 只在 `CONFIG_EFI_LOADER` 分支里填它们。
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_RTC_DEF_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_RTC_DEF_H
#include "../../../third_party/uboot-imx/include/rtc_def.h"
#endif
