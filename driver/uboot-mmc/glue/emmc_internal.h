/*
 * Plugin-internal contract on the glue side: what upstream needs but the environment layer must not
 * know (device count, scratch area, ioctl payload layout, IRQ observation surface). Upstream
 * protocol/host state is `struct mmc` and `struct sdhci_host`, used as-is via the compat shims.
 * Dependency direction is one-way: `glue -> compat -> third_party`.
 */
#ifndef BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H
#define BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H

#include "mmc_env.h"    /* adapter layer: u-boot runtime environment -> brickOS (single entry) */
#include "mmc.h"        /* upstream original (forwarded by compat/mmc.h): struct mmc / struct mmc_config */
#include "sdhci.h"      /* upstream original (forwarded by compat/sdhci.h): struct sdhci_host / registers/IRQ bits */
/* Upstream's private contract (`drivers/mmc/mmc_private.h`): glue is a caller of those internal
 * APIs (`mmc_bread`/`mmc_bwrite`/`mmc_poll_for_busy`/`mmc_list_init`/`mmc_switch_part`). */
#include "mmc_private.h"

/* Runtime-switchable data path: flip the `USE_ADMA` bit of upstream `struct sdhci_host::flags`,
 * which is upstream's data-path interface, so no extra abstraction is introduced. */
#define BR_MMC_PATH_DMA 0u
#define BR_MMC_PATH_PIO 1u

/* Per-transfer limit: upstream's `CONFIG_SYS_MMC_MAX_BLK_COUNT` (a value config) is used both for
 * `cfg->b_max` and for `ADMA_TABLE_SZ`, so this file invents no second number. The asserts below
 * pin it to 64 and to one ADMA2 descriptor's 65532 B length limit. */
#define BR_EMMC_MAX_RW_SECTORS  CONFIG_SYS_MMC_MAX_BLK_COUNT
#define BR_EMMC_SECTOR_SIZE     512u

_Static_assert(BR_EMMC_MAX_RW_SECTORS == 64u,
               "BR_EMMC_MAX_RW_SECTORS must be 64, same as CONFIG_SYS_MMC_MAX_BLK_COUNT");
_Static_assert((BR_EMMC_MAX_RW_SECTORS * BR_EMMC_SECTOR_SIZE) <= ADMA_MAX_LEN,
               "a full-size transfer must fit in one ADMA2 descriptor");

/* `flush` wait bound: eMMC CMD6 (GEN_CMD) with SET_BIT can be slow (u-boot's 500 ms default). */
#define BR_EMMC_CMD6_TIMEOUT_MS 500u

/* `udelay()` busy-waits for register-stable/reset waits, the yield path waits for device events. */

/* Upper bound of a single yield window, 1 ms -- not the clock tick (HZ=200 => 5 ms). It must not
 * be read as "every udelay sleeps this long": udelay(100) yields ~100 us, while get_timer()'s 1 ms
 * window can be woken early by a device IRQ. */


/* ---- device contract (private) ---- */

/* Max cards driven at once: QEMU virt's GPEX wires only 4 INTx lines (SPI 3..6) to the GIC, and
 * core's IRQ framework requires one owner per physical line, so a 5th card would collide. */
#define BR_EMMC_MAX_DEVS     4u

/* Device name is `emmc<d>` (dev-core naming rule [a-z][a-z0-9]*). */
#define BR_EMMC_DEV_NAME     "emmc0"
#define BR_EMMC_NAME_MAX     8u

/* Scratch area for device-level cases: cards 0/1/2 carry live FAT volumes and emmc3 is read-only
 * EROFS, whose image occupies only the start of the card, leaving this tail for write/read-back
 * tests. `tools/mk-fs-images.sh` asserts the image stays under 1 MiB. */
#define BR_EMMC_ASSETS_DEV        "emmc3"
#define BR_EMMC_SCRATCH_LBA       4096u    /* starts at 2 MiB */
#define BR_EMMC_SCRATCH_SECTORS   1024u    /* 512 KiB scratch area (2 MiB..2.5 MiB) */

#define BR_EMMC_IOC_MAGIC    'e'
#define BR_EMMC_IOC_GET_INFO 0u
#define BR_EMMC_IOC_SET_PATH 1u

/* GET_INFO reply, shared by emmc_bdev.c and emmc_selftest.c. */
typedef struct br_emmc_info {
    br_uintptr_t base;              /* SDHCI register window (BAR0) */
    br_u64       capacity_sectors;
    br_u32       sector_size;
    br_u32       max_rw;
    br_u32       bus_width;
    br_u32       clock_hz;
    br_u32       path;              /* BR_MMC_PATH_DMA / _PIO */
    br_u32       card_type;
} br_emmc_info_t;

/* ---- observation surface (selftest) ---- */
/* Per-card wait/interrupt counts, making "interrupt-driven" observable: `irq` ISR entries, `bh`
 * handoffs, `drop` failed `br_work_submit()` (wake directly in the ISR), `wake_irq` waits woken by
 * a device interrupt, `wake_sem` token already present at take (also interrupt evidence, not a
 * timeout), `wake_to` full 1 ms window with no ISR (the fallback), `wake_spin` busy-waits. Only
 * `wake_irq + wake_sem >> wake_to` counts; `irq` exceeds the command count because re-enabling
 * `SIGNAL_ENABLE` can re-assert a status bit upstream has read but not cleared. */
void uboot_mmc_irq_counts(br_u32 idx, br_u32 *irq, br_u32 *bh, br_u32 *drop,
                          br_u32 *wake_irq, br_u32 *wake_sem, br_u32 *wake_to);

/* `udelay()` (busy-wait) calls, which never sleep. */
br_u32 uboot_mmc_wake_spin(br_u32 idx);

/* Level interrupt assertion is `INT_STATUS & SIGNAL_ENABLE`; upstream leaves bits read but not
 * cleared, so enabling the signal with nobody waiting delivers continuously and intersects core's
 * IRQCONF selftest. Criterion: after a transfer ends, `SIGNAL_ENABLE == 0`. */
br_bool uboot_mmc_irq_idle(br_u32 idx);

#endif /* BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H */
