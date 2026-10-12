/* adapter/uboot-erofs — u-boot 头名的适配壳。
 *
 * 上游源码写的是 `#include <linux/compat.h>` 这类 **u-boot 头名**。本文件把它接到
 * 我们的环境实现 `erofs_env.h` 上 —— 于是上游源码**一个字都不用改**。
 * 这是"适配"而不是"复制": 我们只提供名字与环境, 不含任何上游实现。
 */
#ifndef BR_ADAPTER_UBOOT_EROFS_SHIM_COMPILER_H
#define BR_ADAPTER_UBOOT_EROFS_SHIM_COMPILER_H
#include "erofs_env.h"
#endif
