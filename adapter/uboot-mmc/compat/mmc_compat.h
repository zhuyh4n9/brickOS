/*
 * adapter/uboot-mmc — **适配层入口头**(brickOS 侧)
 *
 * ============================== 本文件是什么 ==============================
 * 上游 `drivers/mmc/` 是 u-boot 风格的 C: 它的"运行环境"由 u-boot 自己提供 ——
 * `linux/types.h` 的 `u8/u32/bool`、`malloc.h`、`log.h` 的 `log_err/pr_debug`、
 * `cpu_func.h` 的 `flush_dcache_range`、`udelay()`/`get_timer()`、`WARN_ON`、
 * 以及 DM(`udevice`)与中断注册。本插件是**裸机 unikernel**, 这些都不存在。
 *
 * 本文件就是那份环境在 brickOS 上的**唯一落点**。`src/` 下的上游派生件
 * (`mmc_core.c` / `sdhci_host.c` / `mmc_regs.h` / `emmc_internal.h`)**只** include
 * 本头, 不直接 include 任何 `<br/...>` —— 于是"三方代码↔brickOS 代码"的边界是
 * **一个文件**, 而不是散在四个文件里的十来条 include。
 *
 *   u-boot 侧                  →  brickOS 侧(本头兑现)
 *   ---------------------------------------------------------------------------
 *   u8/u16/u32/u64/bool        →  br_u8/… / br_bool   (br/core/br_types.h)
 *   log_err/log_warning/info   →  br_log_error/warn/info
 *   udelay()/get_timer()       →  br_delay_us() / br_clock_now() / br_deadline_from_now()
 *   flush/invalidate_dcache_*  →  br_mm_cache_flush/invalidate
 *   memalign()(DMA 对齐)       →  br_dma_alloc(…, BR_DMA_F_ALIGN4K, …)
 *   irq_register/enable        →  br_irq_register/enable/lock/unlock/stats_get
 *   信号量(u-boot 无对应物)     →  br_sem_init/take/give
 *   errno(-EIO/…)              →  BR_ERR(BR_EIO/…)
 *
 * ★ 依赖方向: `src/` → `adapter/`(**单向**)。`adapter/` 里的东西反过来 include
 *   `src/` 的头是允许的(那是"适配层认识被适配的代码"); 但 `src/` 只许经本头拿环境。
 *
 * ★ 本头**不**声明 `mmc_regs.h` / `emmc_internal.h` 的任何东西 —— 那些是插件内部
 *   契约, 由各自的 .c 显式 include。本头只管"运行环境"。
 * ==========================================================================
 */
#ifndef BR_THIRD_PARTY_UBOOT_MMC_COMPAT_H
#define BR_THIRD_PARTY_UBOOT_MMC_COMPAT_H

#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#endif /* BR_THIRD_PARTY_UBOOT_MMC_COMPAT_H */
