/* adapter/uboot-fat — u-boot 头名 `<asm/byteorder.h>` 的适配壳。
 *
 * 上游源码写的是 `#include <asm/byteorder.h>` 这类 **u-boot 头名**。本文件把它接到我们的环境实现
 * `../fat_env.h` 上 —— 于是上游源码**一个字都不用改**。
 * 这是"适配"而不是"复制": 我们只提供名字与环境, 不含任何上游实现。
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_ASM_BYTEORDER_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_ASM_BYTEORDER_H
#include "../fat_env.h"
#endif
