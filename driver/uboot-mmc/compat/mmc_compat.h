/* driver/uboot-mmc adapter-layer entry header (brickOS side): the single landing point for the
 * u-boot runtime environment. Upstream `drivers/mmc/` is u-boot-style C whose environment
 * (u8/u32/bool, malloc, logging, `flush_dcache_range`, `udelay`/`get_timer`, DM) does not exist in
 * this bare-metal unikernel; the upstream-derived units include only this header, never `<br/...>`
 * directly, so the third-party <-> brickOS boundary is one file.
 *   u-boot side                ->  brickOS side
 *   u8/u16/u32/u64/bool        ->  br_u8/... / br_bool   (br/core/br_types.h)
 *   log_err/log_warning/info   ->  br_log_error/warn/info
 *   udelay()/get_timer()       ->  br_delay_us() / br_clock_now() / br_deadline_from_now()
 *   flush/invalidate_dcache_*  ->  br_mm_cache_flush/invalidate
 *   memalign() (DMA-aligned)   ->  br_dma_alloc(..., BR_DMA_F_ALIGN4K, ...)
 *   irq_register/enable        ->  br_irq_register/enable/lock/unlock/stats_get
 *   semaphore (no u-boot equiv)->  br_sem_init/take/give
 *   errno (-EIO/...)           ->  BR_ERR(EIO/...)
 *
 * Dependency direction is one-way: `src/` gets its environment only through this header.
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
