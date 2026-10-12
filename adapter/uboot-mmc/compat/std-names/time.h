/* adapter/uboot-mmc — **标准头名** `<time.h>` 的单元级环境补丁(见 plugin.toml 的偏离说明)
 *
 * 上游 `mmc.c` 第 18 行 / `sdhci.c` 第 17 行都有字面 `#include <time.h>`(u-boot 由它拿
 * `get_timer()`/`timer_get_us()`)。本件是裸机 unikernel, 没有 libc 的 `<time.h>`。
 *
 * ★ 为什么不放在 `compat/` 并列入 `includes`: 与同目录 `errno.h` 完全同源的理由(标准头名
 *   与 `runtime/posix/include/time.h` 撞名 ⇒ 会在 brickie 的名字级包含面判据里误报)。
 *   完整说明与回归条件见 `compat/std-names/errno.h` 的文件头、`plugin.toml` 的 `[build]`
 *   注释与 `ADAPTER-NOTES.md` 的"偏离"一节。
 *
 * ★ 语义: `get_timer()`/`timer_get_us()`/`time_after()`/`udelay()`/`mdelay()` 都在
 *   `mmc_env.h` 里 —— 它们是本件的**核心手法**(把上游的轮询变成中断驱动的睡眠等待)。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_STDNAME_TIME_H
#define BR_ADAPTER_UBOOT_MMC_STDNAME_TIME_H
#include "../mmc_env.h"
#endif
