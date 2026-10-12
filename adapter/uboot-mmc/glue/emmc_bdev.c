/*
 * adapter/uboot-mmc — bdev glue + PCIe bind + instance table + interrupts (ISR/bh) + runtime hooks
 *
 * Design refs: `docs/7-storage/7-02-bdev.md` §1/§4 (shape, "DMA/PIO must be observable")/§5 (ISR→bh→wake),
 * `docs/decisions/0021-bdev-pci-emmc.md` D-3 (three-way isolation)/D-4 (interrupts + ADMA2/PIO)/D-5 (flush semantics),
 * `docs/decisions/0022-erofs-fat.md` (multi-card: emmc0/1/2 = FAT12/16/32 volumes, emmc3 = EROFS),
 * `docs/decisions/0026-third-party-vendor-and-adapter.md` (architecture: upstream compiled verbatim).
 *
 * ============================== where this file fits ==============================
 * Upstream `drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c` are byte-for-byte
 * originals, named directly in `brick.toml`'s `sources`. They need two things:
 *   ① **runtime** (types/macros/MMIO/time/log/errno…) — provided by `compat/`;
 *   ② **behaviour glue** (which card, IRQ line, bdev, instance table) — this file.
 *
 * Upstream protocol core and host-layer **state** are no longer ours to define: `struct mmc`
 * (vendor `mmc.h`) and `struct sdhci_host` (vendor `sdhci.h`) are it. The old rewritten copy's
 * `br_mmc_t`/`br_sdhci_t` abstractions are **deleted entirely** — exactly the "upstream
 * derivative" ADR-0026 eliminates.
 *
 * ============ ★ how interrupt-driven holds with "upstream compiled verbatim" (ADR-0021 D-4) ============
 * Upstream `sdhci.c` completes by **polling**: it only yields the CPU via `get_timer()` /
 * `udelay()` (the two loops in task spec §2.4). `compat/mmc_env.h` implements those two as
 * "yield until next ms or **device interrupt**", so the upstream loop becomes an interrupt-driven
 * wait. The three-part scheme (this file + environment layer):
 *
 *   { ISR }  read `INT_STATUS` (**read-only, never clear** — the upstream wait loop detects
 *            completion by reading it), write `SIGNAL_ENABLE` = 0 to **deassert the level IRQ
 *            line**, hand to the bottom half via `br_work_submit()`;
 *   { bh }   `br_sem_give()` wakes the waiter;
 *   { env wait primitive }  **before** sleeping, re-open this card's `SIGNAL_ENABLE`
 *            (see `emmc_env_wait`), so each round can still be woken by the next interrupt.
 *
 * ★ Why the ISR must not clear `INT_STATUS`: QEMU's SDHCI is a **level** interrupt, asserted when
 *   `INT_STATUS & SIGNAL_ENABLE` (`INT_ENABLE` is the source enable upstream `sdhci_init()` already
 *   set). Upstream must read those bits in its own loop and clear them **itself**; if we clear them
 *   it can never see them. The proper way to deassert a level line is to mask the signal source —
 *   which is what `SIGNAL_ENABLE` is for. The wait primitive re-opens it before sleeping, so
 *   "one interrupt → one wake" is self-consistent (if the level is still valid when re-opened the
 *   line asserts again immediately, and the waiter has already read `INT_STATUS`, so no event is lost).
 *
 * ★ How the env layer knows which card is being waited on: the upstream wait loop has no device
 *   argument. The MMIO accessors in `compat/mmc_env.h` report the **address just accessed** to
 *   `uboot_mmc_env_reg_touched()`; this file maps it back to "which registered SDHCI window".
 *   ⇒ Each of the four cards has its own semaphore and IRQ channel (**multi-instance**, ADR-0022),
 *   not one global singleton.
 * ==========================================================================
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>
#include <br/dev/br_bdev.h>
#include <br/dev/br_dev.h>
#include <br/pci/br_pci.h>

#include "emmc_internal.h"

/* Lifecycle hook prototypes (name = symbol_prefix + phase; referenced by generated `plugin_desc.c`) */
int uboot_mmc_early_init(void);
int uboot_mmc_init(void);
int uboot_mmc_start(void);

/* QEMU `-device sdhci-pci` identity (same source as framework/pci-core self-test; measured) */
#define BR_EMMC_PCI_VENDOR  0x1b36u
#define BR_EMMC_PCI_DEVICE  0x0007u

/* Which interrupts SDHCI should **signal**: same set upstream `sdhci_init()` writes to
 * `INT_ENABLE` (`SDHCI_INT_DATA_MASK | SDHCI_INT_CMD_MASK`) - command response/data
 * available/data end/various errors. `INT_STATUS & SIGNAL_ENABLE` asserts the level line (see header). */
#define BR_EMMC_INT_OURS  (SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK)

/* Upper bound on the SDHCI register window (minimum legal BAR0 size; we only map back addresses in it). */
#define BR_EMMC_BAR_WINDOW  0x100u

/* ==================================================================== instance table */

/*
 * Runtime state of one card. **No global singleton** (ADR-0022): each of the four cards has its
 * own upstream host instance (`struct sdhci_host`), protocol core (`struct mmc *`, allocated by
 * upstream `mmc_create`), geometry, IRQ channel and device name; `bdev ops` `priv` points here.
 */
typedef struct emmc_dev {
    struct sdhci_host  host;        /* ★ upstream host state (register window/flags/adma table) */
    struct mmc        *mmc;         /* ★ upstream protocol core (made by upstream mmc_create) */
    br_bdev_geom_t     geom;        /* n_sectors/max_rw filled in start; flags computed per call */
    br_uintptr_t       base;        /* SDHCI register window (BAR0), answered to GET_INFO */
    br_u32             path;        /* BR_MMC_PATH_* (lands in host.flags USE_ADMA bit) */
    br_u32             intx_virq;
    br_bool            chan_ready;  /* IRQ channel registered and enabled (else only tick yields) */
    br_sem_t           sem;         /* ISR → bh → waiter wake channel */
    volatile br_u32    pending;     /* INT_STATUS snapshot accumulated by ISR (diagnostics) */
    volatile br_u32    isr_count;   /* ISR entries (observability criterion, see emmc_internal.h) */
    volatile br_u32    bh_count;    /* gives done inside bh */
    volatile br_u32    bh_drop;     /* br_work_submit queue full ⇒ wake directly in ISR */
    volatile br_u32    wake_irq;    /* waits woken by a device interrupt */
    volatile br_u32    wake_sem;    /* token already present at take (ISR/bh gave) ⇒ returned, no sleep */
    volatile br_u32    wake_to;     /* slept the full window with no ISR (the real "fallback") */
    volatile br_u32    wake_spin;   /* `udelay()` (busy-wait) calls - no sleep, not counted above */
    char               name[BR_EMMC_NAME_MAX];
} emmc_dev_t;

static emmc_dev_t s_dev[BR_EMMC_MAX_DEVS];
static br_u32     s_dev_n;          /* **registered instance count** (probe only grows; see emmc_bring_up) */

/* ==================================================================== IRQ channel */

static void emmc_signal_enable(emmc_dev_t *d)
{
    sdhci_writel(&d->host, (u32)BR_EMMC_INT_OURS, SDHCI_SIGNAL_ENABLE);
}

/* Bottom half: wake only (bh contract: IRQs off, non-blocking, see br_work.h). */
static void emmc_irq_bh(void *arg)
{
    emmc_dev_t *d = (emmc_dev_t *)arg;

    d->bh_count++;
    (void)br_sem_give(&d->sem);
}

static void emmc_isr(void *arg)
{
    emmc_dev_t *d = (emmc_dev_t *)arg;

    if (d == BR_NULL) {
        return;
    }

    const u32 st = sdhci_readl(&d->host, SDHCI_INT_STATUS);

    /*
     * ★ Read `INT_STATUS` only, **never clear** - the upstream wait loop detects completion by
     *   reading it; deassert the level line by writing `SIGNAL_ENABLE` = 0 (re-opened by
     *   `emmc_env_wait` before the next sleep).
     */
    sdhci_writel(&d->host, 0u, SDHCI_SIGNAL_ENABLE);

    d->pending |= st;
    d->isr_count++;

    if (br_work_submit(emmc_irq_bh, d) != 0) {
        /* IR-10: this submit is the only carrier of the interrupt event ⇒ if the bottom half
         * cannot be queued, hand it to the waiter here, else the event goes silent (the waiter
         * could only fall back on tick timeout). */
        d->bh_drop++;
        (void)br_sem_give(&d->sem);
    }
}

/* ==================================================================== runtime hooks
 * These three are **declared in `compat/mmc_env.h`, implemented here** (no u-boot counterpart):
 *   - memalign: upstream `sdhci_adma_init()` needs a DMA-safe ADMA2 descriptor table;
 *   - reg_touched: records the MMIO address just accessed (for the card lookup below);
 *   - wait: yield the CPU until "next ms or this device's interrupt" (polling → interrupt-driven).
 * Dependency direction is thus `compat` (declare) ← `glue` (implement), as with `malloc` → core.
 */

/* ---- memalign: upstream DMA-aligned allocation ------------------------------------
 * u-boot's `memalign.h` contract is "return a buffer at least align-aligned, usable for DMA".
 * ★ Why `br_dma_alloc` (DMA pool) and not `br_malloc`: the ADMA2 descriptor table is **read by
 *   the device**, so it must be physically contiguous with correct attributes (see ADR-0003
 *   region table: the DMA pool sits under CACHED, so `flush_cache()` at the end of
 *   `sdhci_prepare_adma_table()` is real cache maintenance).
 * Upstream calls it in one place only (`sdhci_adma_init`, once per card, 8 bytes), so a small
 * static registry is used; there is no "free" semantics (lives as long as the card - as upstream). */
#define BR_EMMC_ALIGN_SLOTS  8u

static br_dma_buf_t s_align_slot[BR_EMMC_ALIGN_SLOTS];
static br_u32       s_align_n;

void *uboot_mmc_env_memalign(br_size_t align, br_size_t size)
{
    if (size == 0u || s_align_n >= BR_EMMC_ALIGN_SLOTS) {
        return BR_NULL;
    }

    /* At least one cache line: upstream flushes `ROUND(table_len, ARCH_DMA_MINALIGN)`,
     * so the block must cover that range (a flush must not touch a neighbouring allocation). */
    br_size_t need = (size < (br_size_t)ARCH_DMA_MINALIGN) ? (br_size_t)ARCH_DMA_MINALIGN : size;

    br_dma_buf_t b;
    if (br_dma_alloc(need, BR_DMA_F_ALIGN4K, &b) != 0) {
        return BR_NULL;
    }

    /* `br_dma_alloc` gives 4 KiB alignment ≫ ARCH_DMA_MINALIGN(64) that upstream requests. */
    const br_uintptr_t want = (align == 0u) ? 1u : (br_uintptr_t)align;
    if (((br_uintptr_t)b.vaddr % want) != 0u) {
        (void)br_dma_free(&b);
        return BR_NULL;
    }

    s_align_slot[s_align_n] = b;
    s_align_n++;
    return b.vaddr;
}

/* ---- MMIO address → instance lookup -------------------------------------------- */
static br_uintptr_t s_last_reg;

void uboot_mmc_env_reg_touched(const volatile void *reg)
{
    s_last_reg = (br_uintptr_t)reg;
}

static emmc_dev_t *emmc_dev_for_reg(br_uintptr_t addr)
{
    for (br_u32 i = 0u; i < s_dev_n; i++) {
        const br_uintptr_t b = s_dev[i].base;
        if ((b != 0u) && (addr >= b) && (addr < (b + BR_EMMC_BAR_WINDOW))) {
            return &s_dev[i];
        }
    }
    return BR_NULL;
}

/* ---- wait: "yield CPU until next ms or device interrupt" ------------------------
 *
 * ★ This is the core trick (task spec §2.4/§2.5): upstream `get_timer()`/`udelay()` all land
 *   here, so its polling loop becomes "sleep → device interrupt → woken by bh → read INT_STATUS
 *   again".
 *
 * Three branches:
 *   ① address in no SDHCI window (e.g. platform uses time facilities before this) or scheduler
 *      not up ⇒ fall back to "yield one tick / busy-wait one tick"; the env layer is therefore
 *      **always usable**.
 *   ② instance registered but IRQ channel not ready yet (early probe) ⇒ as above, tick fallback.
 *   ③ normal path ⇒ **re-open this card's `SIGNAL_ENABLE` first** (the ISR uses it to deassert
 *      the level line), then sleep; the sleep bound is one tick (`BR_CFG_TICK_PERIOD_US`), so it
 *      **never sleeps forever after a lost interrupt** - worst case it re-reads INT_STATUS every
 *      tick; correctness does not depend on interrupts, but the fast path is interrupt-driven.
 */
/* ---- spin: **busy-wait** (u-boot `udelay` semantics) ----------------------------
 *
 * Upstream uses `udelay()` only to await "register stable/reset done" (10/100/1000/2000 µs,
 * upstream values exhaustively measured); those loops have no device event to wait for, and the
 * scheduler sleep granularity is one tick (5 ms) ⇒ routing them to sleep inflates a µs spin to
 * 5 ms (bring-up 0.037 s → 0.35 s, pushing the whole self-test past the `irq-test` 6 s limit).
 * So do **not** yield here; count into `wake_spin` so logs show how often this path is taken.
 */
void uboot_mmc_env_spin(br_u32 us)
{
    emmc_dev_t *d = emmc_dev_for_reg(s_last_reg);

    br_delay_us((br_time_t)((us != 0u) ? us : 1u));
    if (d != BR_NULL) {
        d->wake_spin++;
    }
}

void uboot_mmc_env_wait(br_u32 max_us)
{
    emmc_dev_t *d = emmc_dev_for_reg(s_last_reg);
    const br_u32 win = (max_us != 0u) ? max_us : UBOOT_MMC_WAIT_MAX_US;

    /* Branches ①②: address in no SDHCI window / IRQ channel not registered / scheduler not up
     * ⇒ fall back to "yield once for the caller's requested duration" (env layer always usable;
     * see the window constants note in emmc_internal.h). */
    if ((d == BR_NULL) || (d->chan_ready != BR_TRUE) || (br_task_self() == BR_NULL)) {
        if (br_task_self() != BR_NULL) {
            (void)br_task_sleep((br_time_t)win);
        } else {
            br_delay_us((br_time_t)win);
        }
        return;
    }

    const br_u32 isr_before = d->isr_count;
    const br_time_t t0 = br_clock_now();

    /*
     * ★★ Open the signal source before sleeping - this line is the source of "interrupt-driven";
     *    it cannot be omitted.
     *
     * Three facts dictate this form (all measured):
     *
     *   ① a level interrupt asserts when `INT_STATUS & SIGNAL_ENABLE` ⇒ to be woken by the next
     *      interrupt, the signal source must be open before sleeping;
     *   ② we **must not** clear `INT_STATUS` (the upstream wait loop detects completion by reading it);
     *   ③ the bit we wait for (DATA_END / DATA_AVAIL / SPACE_AVAIL…) is often **already** set in
     *      `INT_STATUS` - i.e. the line asserts immediately once the source is open and we are
     *      woken right away.
     *
     * ★ Two forms tried and **rejected** (measurements kept, to avoid repeating the exercise):
     *   (a) mask `SIGNAL_ENABLE = 0` again after waking: events the device produces while upstream
     *       is running code have no listener ⇒ the next wait can only be found by the 1 ms
     *       fallback ⇒ whole self-test 2.6 s → 26~29 s, `wake irq/timeout = 0/N` (**no log
     *       evidence of interrupt-driven**), hugging the 30 s gate limit.
     *   (b) open only sources not already in `INT_STATUS` (`OURS & ~pending`): looks clever but
     *       masks exactly the bit we wait for (see ③) ⇒ same degradation to 0/N and 20 s+.
     *   ⇒ The only correct form is **all open** (`SIGNAL_ENABLE = BR_EMMC_INT_OURS`), no mask after wake.
     *
     * Then "won't the line stay asserted when no thread is waiting?" - assertion is still decided by
     * hardware, and upstream `sdhci_send_command()` clears it at **every command end** with
     * `sdhci_writel(host, SDHCI_INT_ALL_MASK, SDHCI_INT_STATUS)` ⇒ the line is flat at command
     * boundaries. And **the only place that opens the signal is here** (bring-up end does **not**
     * open it) ⇒ outside transfer windows the signal source stays 0 as upstream `sdhci_init()` left
     * it. The criterion is the **line**, not the source; see `uboot_mmc_irq_idle()` (the self-test
     * prints `idle-signal=flat|ASSERTED` per card).
     */
    emmc_signal_enable(d);

    (void)br_sem_take(&d->sem, (br_time_t)win);

    /*
     * Accounting (see emmc_internal.h). ★ Three outcomes, counted separately - the first version
     * split only on "was there an ISR during this call", so it mis-recorded case ② below as
     * "fallback timeout", making log readers think interrupt-driven was not working:
     *
     *   ① `wake_irq`  - woken by a device interrupt after sleeping (`isr_count` grew in this
     *                   window). **Strongest evidence**.
     *   ② `wake_sem`  - the token was **already present** entering `br_sem_take` (ISR/bh gave it in
     *                   the previous round) ⇒ returned at once, never slept. This is **also evidence
     *                   of interrupt delivery** (the event was captured by ISR/bh and given), merely
     *                   consumed by the next call. Criterion = elapsed time far below the window
     *                   (half the window).
     *   ③ `wake_to`   - really slept the whole window with not one ISR ⇒ this is the **fallback**.
     *
     * Why ② occurs in bulk: after **one** `uboot_mmc_env_wait()` the upstream wait loop often reads
     * several more rounds of registers before deciding to wait again; the ISR has already placed the
     * token ⇒ the next take hits immediately. Reading ② as timeout leads to the wrong conclusion
     * that waits rely entirely on the 1 ms fallback (hit in practice, see delivery report §5).
     */
    if (d->isr_count != isr_before) {
        d->wake_irq++;
    } else if ((br_clock_now() - t0) < ((br_time_t)win / 2u)) {
        d->wake_sem++;
    } else {
        d->wake_to++;
    }
}

/* ---- "board" weak hooks upstream wants ------------------------------------------
 * u-boot's `mmc_initialize()` calls `board_mmc_init()` → on failure `cpu_mmc_init()`.
 * Probing here is done by glue (PCIe enumeration) and does **not** go through
 * `mmc_initialize()` (only `mmc_list_init()`); these two symbols are a fallback in case
 * upstream `mmc_probe()` gets linked in; semantics match u-boot's default
 * (return -1 = no static mmc list on this board). */
__weak int board_mmc_init(struct bd_info *bis)
{
    (void)bis;
    return -1;
}

__weak int cpu_mmc_init(struct bd_info *bis)
{
    (void)bis;
    return -1;
}

/* ==================================================================== upstream block-layer hooks
 * Upstream `mmc.c`/`mmc_write.c` call these on the block path (interface surface of
 * `blk.h`/`part.h`). Semantics **match upstream** `drivers/mmc/mmc_legacy.c`'s
 * `mmc_select_hwpartp()` item by item: upstream registers it in the `U_BOOT_LEGACY_BLK(mmc)`
 * driver table, which this change does not have (see `compat/blk.h`) ⇒ glue implements the
 * same-named behaviour directly.
 */
int blk_dselect_hwpart(struct blk_desc *desc, int hwpart)
{
    if (desc == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    struct mmc *mmc = find_mmc_device(desc->devnum);
    if (mmc == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    if ((int)mmc->block_dev.hwpart == hwpart) {
        return 0;                       /* already that hw partition (normal here: always 0) */
    }
    if (mmc->part_config == MMCPART_NOAVAILABLE) {
        return BR_ERR(EMEDIUMTYPE);
    }
    return mmc_switch_part(mmc, (unsigned int)hwpart);
}

int blk_select_hwpart_devnum(enum uclass_id uclass_id, int devnum, int hwpart)
{
    if (uclass_id != UCLASS_MMC) {
        return BR_ERR(ENODEV);
    }

    struct mmc *mmc = find_mmc_device(devnum);
    if (mmc == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    return blk_dselect_hwpart(&mmc->block_dev, hwpart);
}

/*
 * `part_init()`: upstream `mmc_startup()` calls it at the end when `!DM_MMC` (scan MBR/GPT and
 * build the software partition table).
 * ★ This change **deliberately does no software partitioning** (v1 exposes the raw device: bdev
 *   geometry is the whole card; partition mapping is SD-9/v1.x). An **explicit empty
 *   implementation** is provided rather than deleting that upstream line - deleting it would be
 *   "modifying upstream"; a clear environment implementation is "adaptation".
 */
void part_init(struct blk_desc *desc)
{
    (void)desc;
}

/*
 * ★ **This change does not define** `blk_dread` / `blk_dwrite` (a measured problem when merging
 *   the two third-party plugins: duplicate symbols - see below).
 *
 *   They are u-boot's **block-layer interface**; in this composition the **only caller** is
 *   upstream `fs/fat/fat.c`/`fat_write.c` compiled in by `adapter/uboot-fat`
 *   (`disk_read/disk_write` → `blk_dread/blk_dwrite`), so its glue defines them
 *   (`adapter/uboot-fat/glue/fat_blk.c`). This plugin's upstream (`mmc.c`) only installs
 *   `mmc_bread/mmc_bwrite` into `blk_desc` function pointers and has **not one `blk_dread` call
 *   site** (non-BLK upstream is designed that way) - defining them here is dead code that also
 *   clashes with fat's copy.
 *
 *   Measured symptom (first full-image link after merging prototype-mmc):
 *     ld: emmc_bdev.o: multiple definition of `blk_dread'; fat_blk.o: first defined here
 *   ⇒ Action = drop these two definitions here (not weaken fat's): "whoever's upstream calls it
 *   defines it". `compat/part.h` still **declares** them (that is the upstream header surface;
 *   removing declarations would modify upstream); this plugin's translation unit does not
 *   reference them ⇒ no undefined symbols.
 */

/* ==================================================================== bdev ops */

static int emmc_read(void *priv, br_u64 lba, void *buf, br_size_t n_lba)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;

    if ((d == BR_NULL) || (buf == BR_NULL) || (n_lba == 0u)) {
        return BR_ERR(EINVAL);
    }

    struct blk_desc *bdesc = mmc_get_blk_desc(d->mmc);
    if (bdesc == BR_NULL) {
        return BR_ERR(ENODEV);
    }

    /*
     * ★ Bounds guarding lives here: upstream `mmc_bread()` **returns 0** out of range (u-boot
     * block-layer convention "0 = error"), while this project's bdev contract is "success =
     * sectors transferred / failure = negative errno" (SD-10) ⇒ 0 must be translated to `-EIO`.
     * This is exactly the TC-EMMC-004 criterion ("out of range does not reach the card": not one
     * command is sent).
     */
    if ((lba >= (br_u64)bdesc->lba) || ((lba + (br_u64)n_lba) > (br_u64)bdesc->lba)) {
        return BR_ERR(EIO);
    }

    const ulong got = mmc_bread(bdesc, (lbaint_t)lba, (lbaint_t)n_lba, buf);
    if (got != (ulong)n_lba) {
        return BR_ERR(EIO);
    }
    return (int)n_lba;
}

static int emmc_write(void *priv, br_u64 lba, const void *buf, br_size_t n_lba)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;

    if ((d == BR_NULL) || (buf == BR_NULL) || (n_lba == 0u)) {
        return BR_ERR(EINVAL);
    }

    struct blk_desc *bdesc = mmc_get_blk_desc(d->mmc);
    if (bdesc == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    if ((lba >= (br_u64)bdesc->lba) || ((lba + (br_u64)n_lba) > (br_u64)bdesc->lba)) {
        return BR_ERR(EIO);
    }

    const ulong got = mmc_bwrite(bdesc, (lbaint_t)lba, (lbaint_t)n_lba, buf);
    if (got != (ulong)n_lba) {
        return BR_ERR(EIO);
    }
    return (int)n_lba;
}

static int emmc_flush(void *priv)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /*
     * WORKAROUND(br-wa-emmc-001): v1 flush only waits for the card to be not busy.
     * The write command polls status before returning, so semantically the blocks written inside
     * the card have reached the medium; but **no** EXT_CSD CACHE_FLUSH (CMD6 SET_BIT[32]) is
     * issued and CACHE_CTRL enable is not checked. Design and repayment in `WORKAROUNDS.md`
     * `br-wa-emmc-001` (ADR-0021 D-5).
     *
     * ★ After ADR-0026 this still lives here: upstream has no "flush" entry point, so "wait for
     *   not busy" uses upstream's exported `mmc_poll_for_busy()` (its `mmc_wait_dat0` is -ENOSYS
     *   here ⇒ takes the SEND_STATUS polling branch, same semantics as the old rewritten copy).
     */
    const int rc = mmc_poll_for_busy(d->mmc, (int)BR_EMMC_CMD6_TIMEOUT_MS);
    return (rc == 0) ? 0 : rc;
}

static int emmc_geometry(void *priv, br_bdev_geom_t *g)
{
    const emmc_dev_t *d = (const emmc_dev_t *)priv;
    if ((d == BR_NULL) || (g == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    br_bdev_geom_t x = d->geom;
    x.flags = (d->path == BR_MMC_PATH_PIO) ? BR_BDEV_F_PIO : BR_BDEV_F_DMA;
    *g = x;
    return 0;
}

/* Runtime SET_PATH: ★ it just changes the `USE_ADMA` bit in upstream `host.flags` (task spec §2.6).
 *   In `sdhci_send_command()` `if (host->flags & USE_DMA)` decides whether to take
 *   `sdhci_prepare_dma()` + `SDHCI_TRNS_DMA`; clearing `USE_ADMA` naturally falls to PIO
 *   (the PIO branch of `sdhci_transfer_data()`). There is **no** extra "threshold/alignment
 *   fallback" logic - the path is decided solely by the runtime flag (hence the old rewritten
 *   copy's `BR_SDHCI_PIO_THRESHOLD_SECTORS` is gone; see README deviations section). */
static void emmc_set_path(emmc_dev_t *d, br_u32 path)
{
    if (path == BR_MMC_PATH_PIO) {
        d->host.flags &= ~(int)USE_ADMA;
    } else {
        d->host.flags |= (int)USE_ADMA;
    }
    d->path = path;
}

static int emmc_ioctl(void *priv, br_u32 cmd, void *arg)
{
    emmc_dev_t *d = (emmc_dev_t *)priv;
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    if (BR_IOC_TYPE(cmd) != (br_u32)BR_EMMC_IOC_MAGIC) {
        return BR_ERR(ENOTSUP);
    }

    switch (BR_IOC_NR(cmd)) {
    case BR_EMMC_IOC_GET_INFO: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }
        br_emmc_info_t *info = (br_emmc_info_t *)arg;
        info->base             = d->base;
        info->capacity_sectors = (br_u64)d->geom.n_sectors;
        info->sector_size      = d->geom.sector_size;
        info->max_rw           = d->geom.max_rw;
        info->bus_width        = d->mmc->bus_width;
        info->clock_hz         = d->mmc->clock;
        info->path             = d->path;
        info->card_type        = d->mmc->cardtype;
        return 0;
    }
    case BR_EMMC_IOC_SET_PATH: {
        if (arg == BR_NULL) {
            return BR_ERR(EINVAL);
        }
        const br_u32 path = *(const br_u32 *)arg;
        if ((path != BR_MMC_PATH_DMA) && (path != BR_MMC_PATH_PIO)) {
            return BR_ERR(EINVAL);
        }
        emmc_set_path(d, path);
        br_log_info("uboot-mmc: %s data path switched to %s", d->name,
                    (path == BR_MMC_PATH_PIO) ? "PIO" : "ADMA2");
        return 0;
    }
    default:
        return BR_ERR(ENOTSUP);
    }
}

static int emmc_suspend(void *priv) { (void)priv; return BR_ERR(ENOTSUP); }   /* D22 reserved */
static int emmc_resume (void *priv) { (void)priv; return BR_ERR(ENOTSUP); }

static const br_bdev_ops_t s_emmc_ops = {
    .read     = emmc_read,
    .write    = emmc_write,
    .flush    = emmc_flush,
    .geometry = emmc_geometry,
    .ioctl    = emmc_ioctl,
    .suspend  = emmc_suspend,
    .resume   = emmc_resume,
};

/* ==================================================================== observability */

void uboot_mmc_irq_counts(br_u32 idx, br_u32 *irq, br_u32 *bh, br_u32 *drop,
                          br_u32 *wake_irq, br_u32 *wake_sem, br_u32 *wake_to)
{
    const br_bool bad = (idx >= BR_EMMC_MAX_DEVS) ? BR_TRUE : BR_FALSE;

    if (irq      != BR_NULL) { *irq      = bad ? 0u : s_dev[idx].isr_count; }
    if (bh       != BR_NULL) { *bh       = bad ? 0u : s_dev[idx].bh_count; }
    if (drop     != BR_NULL) { *drop     = bad ? 0u : s_dev[idx].bh_drop; }
    if (wake_irq != BR_NULL) { *wake_irq = bad ? 0u : s_dev[idx].wake_irq; }
    if (wake_sem != BR_NULL) { *wake_sem = bad ? 0u : s_dev[idx].wake_sem; }
    if (wake_to  != BR_NULL) { *wake_to  = bad ? 0u : s_dev[idx].wake_to; }
}

br_u32 uboot_mmc_wake_spin(br_u32 idx)
{
    return (idx >= BR_EMMC_MAX_DEVS) ? 0u : s_dev[idx].wake_spin;
}

br_bool uboot_mmc_irq_idle(br_u32 idx)
{
    if (idx >= BR_EMMC_MAX_DEVS) {
        return BR_FALSE;
    }
    /*
     * Invariant: **no thread waiting ⇒ INTx line not asserted**. Assertion is SDHCI's hardware
     * definition (`INT_STATUS & SIGNAL_ENABLE != 0`, level type) ⇒ measure that directly, not
     * "is the signal source 0" (even if the source is still open after upstream clears
     * `INT_STATUS` at command end, the line is flat; see the env wait primitive notes).
     */
    const u32 st = sdhci_readl(&s_dev[idx].host, SDHCI_INT_STATUS);
    const u32 se = sdhci_readl(&s_dev[idx].host, SDHCI_SIGNAL_ENABLE);

    return ((st & se) == 0u) ? BR_TRUE : BR_FALSE;
}

/* ==================================================================== lifecycle */

int uboot_mmc_early_init(void)
{
    return 0;   /* no IRQs/no heap yet; the register window waits for pci-core CORE enumeration */
}

int uboot_mmc_init(void)
{
    return 0;   /* see header and README: real init happens in start (global IRQs enabled) */
}

/* Build `emmc<d>` from the device index (no snprintf, fill char by char; only 0..9 possible). */
static void emmc_make_name(char *out, br_u32 idx)
{
    out[0] = 'e';
    out[1] = 'm';
    out[2] = 'm';
    out[3] = 'c';
    out[4] = (char)('0' + (char)(idx % 10u));
    out[5] = '\0';
}

/* Probe and register **one** card. Any failing step returns a negative errno (first failure stops, deliberate). */
static int emmc_bring_up(const br_pci_dev_t *pci, emmc_dev_t *d)
{
    if ((pci->bar[0] == 0u) || (pci->bar_size[0] < BR_EMMC_BAR_WINDOW)) {
        br_log_error("uboot-mmc: %s sdhci BAR0 unassigned or too small (bar0=0x%lx size=%u)",
                     d->name, (unsigned long)pci->bar[0], (unsigned)pci->bar_size[0]);
        return BR_ERR(ENODEV);
    }
    if (pci->intx_virq == 0u) {
        br_log_error("uboot-mmc: %s has no INTx virq", d->name);
        return BR_ERR(ENODEV);
    }

    d->base      = pci->bar[0];
    d->intx_virq = pci->intx_virq;
    d->path      = BR_MMC_PATH_DMA;

    /*
     * ① Static interface fields of the upstream host instance. `add_sdhci()` (upstream) fills in
     *    the rest (`version`/`max_clk`/`clk_mul`/`flags`/`cfg`) - that is upstream
     *    `sdhci_setup_cfg()`'s job, we do not rewrite it.
     */
    d->host.name    = d->name;          /* upstream uses it as cfg->name (diagnostics) */
    d->host.ioaddr  = (void *)d->base;
    d->host.mmc     = BR_NULL;
    d->host.ops     = BR_NULL;          /* this change does not use the vendor SDHCI ops table */
    d->host.quirks  = 0u;               /* card-level quirks set by CONFIG_MMC_QUIRKS in mmc.c */

    /*
     * ② The IRQ channel is set up **before** upstream init: the upstream wait loop must be
     *    wakeable from the very first command (`sdhci_init()` writes SIGNAL_ENABLE = 0, and our
     *    wait primitive re-opens it before each sleep).
     */
    int rc = br_sem_init(&d->sem, 0u);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s semaphore init failed rc=%d", d->name, rc);
        return rc;
    }

    const br_irq_attr_t attr = {
        .prio    = BR_IRQ_PRIO_DEFAULT,
        .trigger = BR_IRQ_TRIG_DEFAULT,
        .flags   = 0u,                  /* INLINE dispatch: ISR in IRQ context, it submits bh */
    };
    rc = br_irq_register(d->intx_virq, emmc_isr, d, &attr);
    if (rc != 0) {
        br_log_error("uboot-mmc: br_irq_register(virq=%u) failed rc=%d",
                     (unsigned)d->intx_virq, rc);
        return rc;
    }
    rc = br_irq_enable(d->intx_virq);
    if (rc != 0) {
        br_log_error("uboot-mmc: br_irq_enable(virq=%u) failed rc=%d",
                     (unsigned)d->intx_virq, rc);
        return rc;
    }
    d->chan_ready = BR_TRUE;

    /*
     * ③ Upstream "host → mmc device" assembly. This is exactly upstream `add_sdhci()`'s job:
     *    `sdhci_setup_cfg()` (caps/clock/voltage/host_caps/b_max/ADMA2 descriptor table) +
     *    `mmc_create()` (allocates struct mmc, registers block device numbers 0/1/2/3).
     *    ★ Not one line is rewritten - this is ADR-0026 in practice.
     */
    rc = add_sdhci(&d->host, 0u, 0u);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s add_sdhci failed rc=%d", d->name, rc);
        return rc;
    }
    d->mmc = d->host.mmc;
    if (d->mmc == BR_NULL) {
        br_log_error("uboot-mmc: %s mmc_create returned no device", d->name);
        return BR_ERR(ENOMEM);
    }

    /*
     * ★ Do **not** open `SIGNAL_ENABLE` here (the easiest place in this file to get wrong; hit in
     *   practice): a level interrupt asserts when `INT_STATUS & SIGNAL_ENABLE`, and `INT_STATUS`
     *   often holds bits upstream has "read but not yet cleared" (RESPONSE etc.). Open it with no
     *   thread waiting and the line **keeps delivering throughout non-transfer periods** - and the
     *   core IRQCONF self-test runs its thread-only API enforcement case in bh context exactly in
     *   that window; the two paths cross and blow up:
     *   `[sched] br_task_exit(): must not end a thread in the interrupt bottom half (bh)`.
     *   ⇒ **Invariant: whenever no thread is waiting, `SIGNAL_ENABLE` is 0.**
     *     Its **only** open point is just before the sleep in `uboot_mmc_env_wait()`, and the ISR masks it.
     */

    br_log_info("uboot-mmc: SDHCI @ 0x%lx (caps=0x%08x, max_clk=%u Hz, intx virq=%u)",
                (unsigned long)d->base, (unsigned)sdhci_readl(&d->host, SDHCI_CAPABILITIES),
                (unsigned)d->host.max_clk, (unsigned)d->intx_virq);

    /*
     * ④ Start the upstream protocol core: CMD0/CMD1/CMD2/CMD3/CMD9/CMD7 + EXT_CSD + bus
     *    width/timing switch. Upstream `mmc_get_op_cond()` calls `cfg->ops->init`
     *    (= `sdhci_init()`), which clears SIGNAL_ENABLE to 0 ⇒ open it once more afterwards
     *    (then maintained by the wait primitive).
     */
    rc = mmc_init(d->mmc);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s mmc_init failed rc=%d", d->name, rc);
        return rc;
    }
    /* ★ Upstream `sdhci_init()` (via `mmc_get_op_cond()` → `cfg->ops->init`) writes
     *   `SIGNAL_ENABLE` = 0 - exactly our invariant (no thread waiting ⇒ no delivery). **Not re-opened**. */

    struct mmc      *mmc   = d->mmc;
    struct blk_desc *bdesc = mmc_get_blk_desc(mmc);
    if ((bdesc == BR_NULL) || (bdesc->lba == 0u) || (mmc->read_bl_len == 0u)) {
        return BR_ERR(EIO);           /* capacity unreadable = identification incomplete, stop */
    }

    d->geom.sector_size = (br_u32)mmc->read_bl_len;
    d->geom.n_sectors   = (br_u64)bdesc->lba;
    d->geom.max_rw      = BR_EMMC_MAX_RW_SECTORS;
    d->geom.align       = 4u;         /* ADMA2 descriptor address must be word-aligned (PIO needs none) */
    d->geom.flags       = BR_BDEV_F_DMA;

    /* ★ Gate criterion line (wording byte-identical to the old form; all values from **upstream** state).
     * ★ Mode name: upstream's `mmc_mode_name()` is compiled out by its own
     *   `#if CONFIG_LOGLEVEL >= LOGL_DEBUG` (see "semantics split" in compat/mmc_env.h) and does
     *   **not exist** at link time ⇒ provide a **minimal mapping** of the first few upstream
     *   `enum bus_mode` entries, with the raw upstream enum value (criterion does not depend on it). */
    const char *mode_name;
    switch (mmc->selected_mode) {
    case MMC_LEGACY: mode_name = "legacy"; break;
    case MMC_HS:     mode_name = "HS";     break;
    case MMC_HS_52:  mode_name = "HS-52MHz"; break;
    case MMC_DDR_52: mode_name = "DDR52";  break;
    default:         mode_name = "other";  break;
    }
    br_log_info("uboot-mmc: eMMC ready (rca=%u, %lu sectors = %lu MiB, %u-bit, %s, %u Hz)",
                (unsigned)mmc->rca, (unsigned long)bdesc->lba,
                (unsigned long)(bdesc->lba / 2048u),
                (unsigned)mmc->bus_width, mode_name,
                (unsigned)mmc->clock);

    rc = br_bdev_register(d->name, &s_emmc_ops, d);
    if (rc != 0) {
        br_log_error("uboot-mmc: bdev register failed rc=%d (name=%s)", rc, d->name);
        return rc;
    }

    br_log_info("uboot-mmc: /dev/%s ready (%lu sectors, %u-bit, %u Hz, path=%s)",
                d->name, (unsigned long)d->geom.n_sectors,
                (unsigned)mmc->bus_width, (unsigned)mmc->clock,
                (d->path == BR_MMC_PATH_PIO) ? "PIO" : "ADMA2");
    return 0;
}

int uboot_mmc_start(void)
{
    const br_u32 total = br_pci_dev_count();
    br_u32 found = 0u;

    /* Upstream's device list must be initialised once (it resets `cur_dev_num` to 0 ⇒ the four
     * cards get devnum = 0..3, matching emmc0..emmc3; see task spec §2.2). Not via
     * `mmc_initialize()`: that is u-boot's "static board mmc list" path; devices here come from
     * PCIe enumeration (see board_mmc_init note). */
    mmc_list_init();

    for (br_u32 i = 0u; i < total; i++) {
        const br_pci_dev_t *d = br_pci_dev_at(i);
        if (d == BR_NULL) {
            continue;
        }
        if ((d->vendor != BR_EMMC_PCI_VENDOR) || (d->device != BR_EMMC_PCI_DEVICE)) {
            continue;
        }
        if (s_dev_n >= BR_EMMC_MAX_DEVS) {
            br_log_error("uboot-mmc: card count over limit %u (sdhci-pci #%lu not taken over)",
                         (unsigned)BR_EMMC_MAX_DEVS, (unsigned long)(found + 1u));
            return BR_ERR(ENOSPC);
        }

        emmc_dev_t *dev = &s_dev[s_dev_n];
        emmc_make_name(dev->name, s_dev_n);     /* name first: failure logs must name the card */
        s_dev_n++;                              /* reserve: not reclaimed on failure (boot-time stop) */

        const int rc = emmc_bring_up(d, dev);
        if (rc != 0) {
            return rc;
        }
        found++;
    }

    if (found == 0u) {
        br_log_error("uboot-mmc: composition has pci-core but no sdhci-pci found (%04x:%04x)",
                     (unsigned)BR_EMMC_PCI_VENDOR, (unsigned)BR_EMMC_PCI_DEVICE);
        return BR_ERR(ENODEV);
    }

    br_log_info("uboot-mmc: took over %lu sdhci-pci cards", (unsigned long)found);
    return 0;
}
