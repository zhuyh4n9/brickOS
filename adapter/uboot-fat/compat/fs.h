/* adapter/uboot-fat — `<fs.h>` 的**转发壳**(不是副本)。
 *
 * 上游 `fs/fat/fat.c` 写 `#include <fs.h>`, 拿的是 `struct fs_dirent`(目录项)与
 * `struct fs_dir_stream`(目录流), 以及 `FS_DT_DIR/FS_DT_REG`。上一版把这几个结构**抄**
 * 进了插件并**裁掉**了 rtc_time 与时戳字段(那是"改上游") ⇒ 现在转发到 vendor 原件:
 * `struct fs_dirent` 就是 u-boot 的形状(name[256] + 三个 rtc_time), 名字上界的执法仍由
 * glue 负责(见 `glue/fat_vfs.c` 的 `uboot_fat_fill_dirent`)。
 *
 * `include/fs.h` 自己 include `<rtc_def.h>` ⇒ 本目录提供同名的转发壳(指向 vendor 原件)。
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_FS_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_FS_H
#include "fat_env.h"
#include "../../../third_party/uboot-imx/include/fs.h"
#endif
