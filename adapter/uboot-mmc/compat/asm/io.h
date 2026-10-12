/* adapter/uboot-mmc — u-boot 头名 `<asm/io.h>` 的适配壳。
 *
 * 上游 `drivers/mmc/` 下的源码写的是 **u-boot 头名**(例如 `#include <log.h>`)。本文件把它
 * 接到我们的环境实现 `mmc_env.h` 上 —— 于是上游源码**一个字都不用改**。
 * 这是"给上游它依赖的环境", 不是复制上游: 本文件不含任何上游实现。
 *
 * ★ 为什么用相对路径而不是 `-I third_party/uboot-imx/include`:
 *   brickie 给每个编译单元的 `-I` 是**按字典序**排的, 而 `../..` 排在 `compat` 之前
 *   ⇒ vendor 的 include 目录会**盖住** `compat/` 里的同名壳(`<mmc.h>`/`<part.h>` 会被
 *   解析到 u-boot 的真头, 那份还要 `cyclic.h`/`efi.h` 整族)。相对路径没有这个歧义
 *   (与 `adapter/uboot-erofs/compat/u-boot/lz4.h` 同款处置)。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_ASM_IO_H_H_
#define BR_ADAPTER_UBOOT_MMC_ASM_IO_H_H_
#include "../mmc_env.h"
#endif
