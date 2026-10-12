/*
 * adapter/uboot-mmc — **plugin-internal contract** (glue/; project code, no upstream counterpart)
 *
 * ============================== Layering (form after ADR-0026) ==============================
 *   third_party/uboot-imx/drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c
 *        upstream originals, **compiled as-is** (no copies; locks and criteria in
 *        tools/check-third-party-pristine.sh)
 *   compat/   u-boot runtime environment: header shims + mmc_env.h + generated/autoconf.h (config matrix)
 *   glue/     this project's wiring: PCIe binding + static instance table + interrupts (ISR/bh)
 *        + bdev ops + ioctl + selftest
 *
 * ⇒ This header is the contract on the **glue side**: what upstream needs but "the environment
 *   layer must not know" (device count, scratch area, ioctl payload layout, IRQ count observation
 *   surface). Upstream **protocol/host state** is not here — that is `struct mmc` (vendor
 *   `include/mmc.h`) and `struct sdhci_host` (vendor `include/sdhci.h`), used as-is through the
 *   compat forwarding shims.
 *
 * ★ Why this header may include upstream headers and compat/ may too: the dependency direction is
 *   `glue → compat → third_party` (one-way). The criterion = the `src/` layer is **deleted**, and
 *   no direct include writes an upstream path in angle brackets or quotes anywhere in the tree
 *   (gate criterion ⑤).
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H
#define BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H

#include "mmc_env.h"    /* adapter layer: u-boot runtime environment → brickOS (single entry) */
#include "mmc.h"        /* upstream original (forwarded by compat/mmc.h): struct mmc / struct mmc_config */
#include "sdhci.h"      /* upstream original (forwarded by compat/sdhci.h): struct sdhci_host / registers/IRQ bits */
/* Upstream's **private** contract (`drivers/mmc/mmc_private.h`): it declares internal APIs that
 * upstream keeps out of public headers — this glue is exactly a caller of those APIs
 * (`mmc_bread`/`mmc_bwrite`/`mmc_poll_for_busy`/`mmc_list_init`/`mmc_switch_part`). Same pattern
 * as `adapter/uboot-erofs/glue/erofs_glue.h` including upstream `fs/erofs/internal.h`: glue
 * knowing the code it adapts is an allowed direction. */
#include "mmc_private.h"

/* ==================================================================== data path */
/* Switchable at runtime (ADR-0021 D-4). ★ How: directly flip the `USE_ADMA` bit of upstream
 * `struct sdhci_host::flags` — that field is upstream's "data path" interface (task spec §2.6),
 * so no extra abstraction is introduced. */
#define BR_MMC_PATH_DMA 0u
#define BR_MMC_PATH_PIO 1u

/* ==================================================================== transfer limit
 *
 * ★ **Single source**: the per-transfer limit is upstream's `CONFIG_SYS_MMC_MAX_BLK_COUNT` (a value
 *   config, see the matrix in `compat/generated/autoconf.h`): upstream uses it for both
 *   `cfg->b_max` (granularity at which the protocol core splits large requests) and
 *   `ADMA_TABLE_SZ` (ADMA2 descriptor table size). This file invents no separate number — a
 *   mismatch between the two would cause an out-of-bounds write when "the descriptor table does
 *   not fit one transfer".
 *
 *   The two `_Static_assert`s below turn this into a **compile-time criterion** (not a comment
 *   convention):
 *     ① the limit must be exactly 64 (same value as the old rewritten copy's
 *        `BR_EMMC_MAX_RW_SECTORS`, contract unchanged);
 *     ② one 64-sector transfer must fit inside a **single** ADMA2 descriptor's len limit (65532 B). */
#define BR_EMMC_MAX_RW_SECTORS  CONFIG_SYS_MMC_MAX_BLK_COUNT
#define BR_EMMC_SECTOR_SIZE     512u

_Static_assert(BR_EMMC_MAX_RW_SECTORS == 64u,
               "BR_EMMC_MAX_RW_SECTORS must be 64, same as CONFIG_SYS_MMC_MAX_BLK_COUNT");
_Static_assert((BR_EMMC_MAX_RW_SECTORS * BR_EMMC_SECTOR_SIZE) <= ADMA_MAX_LEN,
               "a full-size transfer must fit in one ADMA2 descriptor");

/* Upper bound for `flush` waiting: eMMC CMD6 (GEN_CMD) with SET_BIT can be slow (u-boot's
 * `DEFAULT_CMD6_TIMEOUT_MS` is 500 ms). */
#define BR_EMMC_CMD6_TIMEOUT_MS 500u

/* The division of labor between `udelay()` (busy-wait) and "yield-and-wait" is in the
 * `compat/mmc_env.h` file header: the former waits for "register stable/reset complete" (no device
 * event to wait on ⇒ busy-wait), the latter for "device events" (⇒ yield + interrupt wake). The
 * criterion is expressed on the `compat` side, so no threshold constant is needed here. */

/* ==================================================================== environment wait window
 *
 * ★ This is the **key constant** of the "polling → interrupt-driven" switch (task spec §2.4:
 *   "yield the CPU until **the next ms** or a device interrupt"). It is 1 ms, **not** the clock
 *   tick (`BR_CFG_TICK_PERIOD_US` = 1/HZ; this product HZ=200 ⇒ 5 ms).
 *
 * ★★ Measured lesson (hit and fixed on this pass, see ADR-0029 §4): it must be only the **upper
 *   bound of a single yield window** and must **not** be read as "every `udelay()` sleeps this
 *   long". Upstream uses `udelay(100)` as a polling metronome (inhibit loops / clock-stable loops /
 *   the `sdhci_reset` spin); if every call slept a full 1 ms, four-card bring-up grew from
 *   **0.037 s to 0.376 s** (measured) and the whole selftest was pushed past `irq-test`'s 6 s gate
 *   limit (measured: irq-test was 2/5 red before the change, 5/5 red after).
 *   ⇒ `udelay(us)` now yields for **the duration the caller asked for** (sliced as
 *   `min(remaining, 1 ms)`), so `udelay(100)` ≈ 100 µs (matching u-boot semantics), while
 *   `get_timer()`'s 1 ms yield window can still be woken early by a device interrupt (that is the
 *   command-completion wait path).
 *
 * The constant itself is defined in `compat/mmc_env.h` (`UBOOT_MMC_WAIT_MAX_US`) — it is an
 * **environment layer** parameter (compat needs it to declare the wait primitives), so glue must
 * not define a second copy. */


/* ==================================================================== device contract (private) */

/* Max number of cards this plugin drives at once (= upper bound on `sdhci-pci` endpoints). The
 * value comes from **platform fact**, not guesswork: QEMU virt's GPEX wires only 4 INTx lines to
 * the GIC (SPI 3..6), and core's IRQ framework requires "exactly one owner per physical line"
 * (`irq_core.c` INV-A) ⇒ a 5th card's INTx would collide with the 1st and fail registration.
 * So 4 is the number of cards this platform **can take**. */
#define BR_EMMC_MAX_DEVS     4u

/* Device name looks like `emmc<d>` (dev-core naming rule [a-z][a-z0-9]*, see 7-02 §1). */
#define BR_EMMC_DEV_NAME     "emmc0"
#define BR_EMMC_NAME_MAX     8u

/*
 * **Scratch area** for device-level cases (ADR-0022): the four cards each have a role
 * (0/1/2 = FAT12/16/32 volumes, 3 = EROFS); the FAT volumes are filled by their filesystems and
 * EROFS is read-only — its image occupies only the start of the card (measured ≤ 1 MiB), leaving
 * the tail for the "write a pattern then read it back" device-level cases. The property
 * **does not overlap any filesystem's data area** is asserted by `tools/mk-fs-images.sh`
 * (it fails outright if the EROFS image > 1 MiB), not left to convention.
 */
#define BR_EMMC_ASSETS_DEV        "emmc3"
#define BR_EMMC_SCRATCH_LBA       4096u    /* starts at 2 MiB */
#define BR_EMMC_SCRATCH_SECTORS   1024u    /* 512 KiB scratch area (2 MiB..2.5 MiB) */

#define BR_EMMC_IOC_MAGIC    'e'
#define BR_EMMC_IOC_GET_INFO 0u
#define BR_EMMC_IOC_SET_PATH 1u

/* GET_INFO reply (ioctl payload). Production (emmc_bdev.c) and selftest (emmc_selftest.c) share one layout. */
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

/* ==================================================================== observation surface (selftest) */
/*
 * Per-card **wait/interrupt counts** (ADR-0021 D-4's "interrupt-driven" must be observable, not
 * merely asserted in comments).
 *
 * `wake` is the **new criterion surface** of this pass: it turns "is it really interrupt-driven"
 * from prose into numbers — counting only `isr > 0` is not enough: if waits mostly return via the
 * 1 ms fallback timeout, `isr` still grows slowly and looks like "there are interrupts". So the
 * wait primitives **account on every return**:
 *
 *   irq     number of entries into that card's INTx ISR (the device really delivered a level IRQ);
 *   bh      number of handoffs from the ISR via `br_work_submit()` to the bottom half with a
 *           `br_sem_give()` in bh;
 *   drop    number of `br_work_submit()` failures due to a full queue (IR-10 rollback path: wake
 *           directly in the ISR);
 *   wake_irq    number of times a wait primitive, **after sleeping**, was woken by a device
 *           interrupt (`isr` grew within this window);
 *   wake_sem    entering `br_sem_take` with the **token already there** (ISR/bh gave it in the
 *           previous round) ⇒ immediate return. ★ This too is evidence that "the event was caught
 *           by the interrupt", merely consumed by the following call — it must **not** be read as a
 *           timeout;
 *   wake_to     number of times it really slept the full 1 ms window with not a single ISR — ★ this
 *           is the **fallback**;
 *   wake_spin   number of **busy-waits** (`udelay()` takes this path — waiting for register
 *           stable/reset complete, with no device event to wait on, so the CPU is not yielded; see
 *           the division of labor in the `compat/mmc_env.h` header).
 *
 * ★ Criterion: only `wake_irq + wake_sem ≫ wake_to` counts as "interrupt-driven"; looking at
 *   `wake_irq` alone gives a wrong conclusion in the "token consumed by the next round" shape
 *   (hit in measurement, see the accounting comment in emmc_bdev.c).
 *
 * ★ There is also one **faithfully recorded redundancy**: the ISR can only mask `SIGNAL_ENABLE` to
 *   deassert the level line (it cannot clear `INT_STATUS` — upstream's wait loops read it), and the
 *   wait primitive must re-enable `SIGNAL_ENABLE` before sleeping, so when upstream has "read the
 *   status but not yet cleared it", re-enabling the signal makes the level line **assert once more
 *   immediately** ⇒ `irq` exceeds the command count (factor = number of times per command upstream
 *   read status without clearing). These are still **real INTx deliveries**, just redundant ones;
 *   see the comparison line in emmc_selftest.c and the README's "deviations from upstream" for the
 *   numeric relation. */
void uboot_mmc_irq_counts(br_u32 idx, br_u32 *irq, br_u32 *bh, br_u32 *drop,
                          br_u32 *wake_irq, br_u32 *wake_sem, br_u32 *wake_to);

/* Count of the `udelay()` (busy-wait) path — it does **not sleep**, so it is not among the three kinds above. */
br_u32 uboot_mmc_wake_spin(br_u32 idx);

/*
 * **Turning "no thread waiting ⇒ no delivery" into a criterion** (see the single place in
 * emmc_bdev.c that enables `SIGNAL_ENABLE`).
 *
 * The assert condition of a level interrupt = `INT_STATUS & SIGNAL_ENABLE`, and upstream leaves
 * bits "read but not yet cleared" ⇒ enabling the signal while nobody waits makes this INTx deliver
 * continuously for the entire non-transfer period. Measured consequence: it intersected with core's
 * IRQCONF selftest (the thread-only API enforcement case run in bh context) and blew up with
 * `[sched] br_task_exit(): must not exit a thread in the interrupt bottom half (bh)`.
 *
 * Criterion: after a transfer ends (nobody waiting) `SIGNAL_ENABLE == 0`. The selftest prints this
 * for every card.
 */
br_bool uboot_mmc_irq_idle(br_u32 idx);

#endif /* BR_ADAPTER_UBOOT_MMC_GLUE_INTERNAL_H */
