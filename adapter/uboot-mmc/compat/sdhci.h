/* adapter/uboot-mmc — `<sdhci.h>` 的适配壳(→ **vendor 原件**)。
 *
 * ★ `struct sdhci_host` 是上游 SDHCI 主机驱动自己的状态(寄存器窗口、`flags` 的
 *   `USE_ADMA` 位、`adma_desc_table`…), 而 `sdhci.c`/`sdhci-adma.c` 直接读写它。
 *   ★ 我们的 glue **也要写它**的 `flags` 位(运行期 SET_PATH 切 ADMA2/PIO)与 `ioaddr`/
 *   `cfg`/`quirks` —— 那个字段**就是接口**(任务书 §2.6)。抄一份会立刻漂移。
 *   ⇒ 逐字节用 vendor 那份。
 *
 * ★ 它 include `<asm/io.h>`/`<asm/gpio.h>`/`<linux/kernel.h>` 等, 全由 `compat/` 兑现。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_SDHCI_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_SDHCI_H
#include "../../../third_party/uboot-imx/include/sdhci.h"
#endif
