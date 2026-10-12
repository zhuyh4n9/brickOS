/*
 * adapter/uboot-mmc — **adapter-layer entry header** (brickOS side)
 *
 * ============================== What this file is ==============================
 * Upstream `drivers/mmc/` is u-boot-style C: its "runtime environment" is provided by u-boot itself —
 * `linux/types.h`'s `u8/u32/bool`, `malloc.h`, `log.h`'s `log_err/pr_debug`, `cpu_func.h`'s
 * `flush_dcache_range`, `udelay()`/`get_timer()`, `WARN_ON`, plus DM (`udevice`) and interrupt
 * registration. This plugin is a **bare-metal unikernel**, where none of these exist.
 *
 * This file is the **single landing point** for that environment on brickOS. The upstream-derived
 * units under `src/` (`mmc_core.c` / `sdhci_host.c` / `mmc_regs.h` / `emmc_internal.h`) include
 * **only** this header, never any `<br/...>` directly — so the "third-party code ↔ brickOS code"
 * boundary is **one file**, not a dozen includes scattered across four files.
 *
 *   u-boot side                →  brickOS side (fulfilled by this header)
 *   ---------------------------------------------------------------------------
 *   u8/u16/u32/u64/bool        →  br_u8/… / br_bool   (br/core/br_types.h)
 *   log_err/log_warning/info   →  br_log_error/warn/info
 *   udelay()/get_timer()       →  br_delay_us() / br_clock_now() / br_deadline_from_now()
 *   flush/invalidate_dcache_*  →  br_mm_cache_flush/invalidate
 *   memalign() (DMA-aligned)   →  br_dma_alloc(…, BR_DMA_F_ALIGN4K, …)
 *   irq_register/enable        →  br_irq_register/enable/lock/unlock/stats_get
 *   semaphore (no u-boot equiv)→  br_sem_init/take/give
 *   errno (-EIO/…)             →  BR_ERR(EIO/…)
 *
 * ★ Dependency direction: `src/` → `adapter/` (**one-way**). It is allowed for things in `adapter/`
 *   to include headers from `src/` (that is "the adapter layer knowing the adapted code"); but `src/`
 *   may only get its environment through this header.
 *
 * ★ This header declares **nothing** from `mmc_regs.h` / `emmc_internal.h` — those are plugin-internal
 *   contracts, included explicitly by their respective .c files. This header only covers the "runtime
 *   environment".
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
