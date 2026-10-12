/* adapter/uboot-mmc — `<memalign.h>` 的适配壳(→ **vendor 原件**)。
 *
 * ★ 为什么转发而不是自己给那几条宏: 上游 `mmc.c:970` 用
 *   `ALLOC_CACHE_ALIGN_BUFFER(u8, test_csd, MMC_MAX_BLOCK_LEN)`(mc_set_card_speed 里给
 *   CMD6 准备一张**DMA 对齐的栈上缓冲**)。这几条宏(栈数组 + 对齐切片的写法)正是 u-boot
 *   的"栈上 DMA 安全缓冲"惯用法 —— 抄一份就是"把上游源码抄进 adapter/"。
 *   它的 include 闭合集(`<linux/kernel.h>`/`<asm/cache.h>`/`<malloc.h>`)全由 compat/ 兑现。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_MEMALIGN_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_MEMALIGN_H
#include "../../../third_party/uboot-imx/include/memalign.h"
#endif
