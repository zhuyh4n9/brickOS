/* adapter/uboot-fat — `<fat.h>` 的**转发壳**(不是副本)。
 *
 * 上游 `fs/fat` 下的 .c 写 `#include <fat.h>`, 拿的是 FAT 的**盘上结构**(boot_sector/
 * volume_info/dir_entry/dir_slot/nameext/fsdata 与属性位/判据宏)。上一版把这些结构
 * **照抄**进了插件(ADR-0026 §5 点名要消灭的东西)⇒ 现在改成**转发到 vendor 原件**:
 * 内容只有一处真值(逐字节上游, 由 `adapter/third-party-upstream.lock` 的哈希锁守),
 * 我们这边一个字段都不抄。
 *
 * ★ 为什么用相对路径而不是 `-I third_party/uboot-imx/include`:
 *   brickie 对每个编译单元的 `-I` 是**按字典序**排的(`build.rs` 的 unit_includes), 而
 *   `../…/third_party/…` 里的 `.` 排在 `compat` 前面 ⇒ 指向 vendor include 的那条 `-I`
 *   会**盖住**本目录的同名壳(`<fs.h>`/`<part.h>` 会被解析到 u-boot 的真头, 而那份还要
 *   `<asm/byteorder.h>`/`<rtc_def.h>` 之类)。显式相对路径没有这个歧义
 *   (与 `adapter/uboot-erofs/compat/u-boot/lz4.h` 同一手法与同一理由)。
 *
 * ★ 环境先就位: 上游 `include/fat.h` 自己 include `<fs.h>` 与 `<asm/byteorder.h>`,
 *   于是这里先把 `fat_env.h`(环境实现)接上, 再转发 —— 顺序反了会把 vendor 头里的
 *   `FAT2CPU16/32`/`__u16` 之类落在"未定义"上。
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_FAT_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_FAT_H
#include "fat_env.h"
#include "../../../third_party/uboot-imx/include/fat.h"
#endif
