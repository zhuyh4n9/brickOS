/* adapter/uboot-fat - **forwarding shim** for `<fs.h>` (not a copy).
 *
 * Upstream `fs/fat/fat.c` writes `#include <fs.h>` and takes `struct fs_dirent` (directory
 * entry), `struct fs_dir_stream` (directory stream) and `FS_DT_DIR/FS_DT_REG`. The previous
 * version **copied** these structs into the plugin and **trimmed** rtc_time and the timestamp
 * fields (that was "modifying upstream") => now it forwards to the vendor original: `struct
 * fs_dirent` is u-boot's shape (name[256] + three rtc_time), and enforcing the name bound
 * remains glue's job (see `uboot_fat_fill_dirent` in `glue/fat_vfs.c`).
 *
 * `include/fs.h` itself includes `<rtc_def.h>`, so this directory provides a same-named
 * forwarding shim (pointing at the vendor original).
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_FS_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_FS_H
#include "fat_env.h"
#include "../../../third_party/uboot-imx/include/fs.h"
#endif
