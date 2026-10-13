/* fs/uboot-fat - forwarding shim for <fs.h>: struct fs_dirent, struct fs_dir_stream and
 * FS_DT_DIR/FS_DT_REG are u-boot's shapes (fs_dirent name[256] + three rtc_time), untrimmed.
 * Vendor fs.h pulls <rtc_def.h>, provided by the shim in this directory. Enforcing the name
 * bound stays glue's job (uboot_fat_fill_dirent, glue/fat_vfs.c).
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_FS_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_FS_H
#include "fat_env.h"
#include "../../../third_party/uboot-imx/include/fs.h"
#endif
