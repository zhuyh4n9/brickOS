/* adapter/uboot-fat — u-boot 头名 `<part.h>` 的适配壳。
 *
 * 上游 `fs/fat/fat.c` 写 `#include <part.h>`, 拿的是 `struct blk_desc` +
 * `struct disk_partition`(u-boot 的分区表抽象)。本件没有分区表 —— "整设备一个分区"
 * (起始扇区 0, 扇区数 = 设备几何的 n_sectors) —— 那两个结构是 `fat_env.h` 里的**极小
 * shim**(字段名与上游一致, 只保留上游 fat.c 真正碰到的字段), 定义在环境头里。
 * 这是"给上游它依赖的环境", 不是"把上游的 part.h 抄进来"。
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_PART_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_PART_H
#include "fat_env.h"
#endif
