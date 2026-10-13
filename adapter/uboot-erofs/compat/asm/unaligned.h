/* adapter/uboot-erofs - shim for u-boot header names: routes them to erofs_env.h so upstream
 * sources stay byte-identical. Adaptation only: names and environment, no upstream code. */
#ifndef BR_ADAPTER_UBOOT_EROFS_SHIM_ASM_UNALIGNED_H
#define BR_ADAPTER_UBOOT_EROFS_SHIM_ASM_UNALIGNED_H
#include "../erofs_env.h"
#endif
