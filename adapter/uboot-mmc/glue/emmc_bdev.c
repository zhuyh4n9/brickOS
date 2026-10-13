/*
 * Behaviour glue for the upstream `drivers/mmc/` originals: which card, IRQ line, bdev and instance
 * table. Upstream needs runtime (compat/) plus this glue; its protocol/host state is `struct mmc`
 * and `struct sdhci_host`.
 *
 * Upstream `sdhci.c` completes by polling and yields the CPU only via `get_timer()`/`udelay()`,
 * which compat/ implements as "yield until next ms or a device interrupt". This file supplies the
 * other half: the ISR reads `INT_STATUS` only (never clears it -- the upstream loop detects
 * completion by reading it) and writes `SIGNAL_ENABLE` = 0 to deassert the level line, then submits
 * the bh; the bh does `br_sem_give()`; the wait primitive re-opens `SIGNAL_ENABLE` before sleeping
 * so each round can be woken again. The env layer maps the MMIO address reported by
 * `uboot_mmc_env_reg_touched()` back to a registered window, so each card has its own semaphore and
 * IRQ channel (no global singleton).
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

/* Lifecycle hooks (symbol prefix + phase; referenced by generated `plugin_desc.c`) */
int uboot_mmc_early_init(void);
int uboot_mmc_init(void);
int uboot_mmc_start(void);

/* QEMU `-device sdhci-pci` identity (measured). */
#define BR_EMMC_PCI_VENDOR  0x1b36u
#define BR_EMMC_PCI_DEVICE  0x0007u

/* Interrupts SDHCI signals: the set upstream `sdhci_init()` writes to `INT_ENABLE`;
 * `INT_STATUS & SIGNAL_ENABLE` asserts the level line. */
#define BR_EMMC_INT_OURS  (SDHCI_INT_CMD_MASK | SDHCI_INT_DATA_MASK)

/* Upper bound on the SDHCI register window (minimum legal BAR0 size). */
#define BR_EMMC_BAR_WINDOW  0x100u

/* ---- instance table ---- */

/* Runtime state of one card. No global singleton: each card has its own upstream host instance,
 * protocol core, geometry, IRQ channel and name; `bdev ops` `priv` points here. */
typedef struct emmc_dev {
    struct sdhci_host  host;        /* upstream host state (register window/flags/adma table) */
    struct mmc        *mmc;         /* upstream protocol core (made by upstream mmc_create) */
    br_bdev_geom_t     geom;        /* n_sectors/max_rw filled in start; flags computed per call */
    br_uintptr_t       base;        /* SDHCI register window (BAR0), answered to GET_INFO */
    br_u32             path;        /* BR_MMC_PATH_* (lands in host.flags USE_ADMA bit) */
    br_u32             intx_virq;
    br_bool            chan_ready;  /* IRQ channel registered and enabled (else only tick yields) */
    br_sem_t           sem;         /* ISR -> bh -> waiter wake channel */
    volatile br_u32    pending;     /* INT_STATUS snapshot accumulated by ISR (diagnostics) */
    volatile br_u32    isr_count;   /* ISR entries (observability criterion, see emmc_internal.h) */
    volatile br_u32    bh_count;    /* gives done inside bh */
    volatile br_u32    bh_drop;     /* br_work_submit queue full -> wake directly in ISR */
    volatile br_u32    wake_irq;    /* waits woken by a device interrupt */
    volatile br_u32    wake_sem;    /* token already present at take (ISR/bh gave) -> returned, no sleep */
    volatile br_u32    wake_to;     /* slept the full window with no ISR (the real "fallback") */
    volatile br_u32    wake_spin;   /* `udelay()` (busy-wait) calls - no sleep, not counted above */
    char               name[BR_EMMC_NAME_MAX];
} emmc_dev_t;

static emmc_dev_t s_dev[BR_EMMC_MAX_DEVS];
static br_u32     s_dev_n;          /* **registered instance count** (probe only grows; see emmc_bring_up) */

/* ---- IRQ channel ---- */

static void emmc_signal_enable(emmc_dev_t *d)
{
    sdhci_writel(&d->host, (u32)BR_EMMC_INT_OURS, SDHCI_SIGNAL_ENABLE);
}

/* Bottom half: wake only (IRQs off, non-blocking). */
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

    /* Read `INT_STATUS` only, never clear it: the upstream wait loop detects completion by reading
     * it. Deassert the level line by masking `SIGNAL_ENABLE` (re-opened by `emmc_env_wait`). */
    sdhci_writel(&d->host, 0u, SDHCI_SIGNAL_ENABLE);

    d->pending |= st;
    d->isr_count++;

    if (br_work_submit(emmc_irq_bh, d) != 0) {
        /* IR-10: this submit is the only carrier of the interrupt event, so if the bh cannot be
         * queued, hand it to the waiter here or the event goes silent. */
        d->bh_drop++;
        (void)br_sem_give(&d->sem);
    }
}

/* Runtime hooks declared in `compat/mmc_env.h`, implemented here: memalign (DMA-safe ADMA2
 * descriptor table), reg_touched (last MMIO address), wait (yield until next ms or device IRQ). */

/* ---- memalign: DMA-aligned allocation ----
 * u-boot's `memalign.h` contract is "at least align-aligned, usable for DMA". The ADMA2 descriptor
 * table is read by the device, so it must come from the DMA pool (physically contiguous); upstream
 * calls this once per card and there is no free semantics, hence the small static registry. */
#define BR_EMMC_ALIGN_SLOTS  8u

static br_dma_buf_t s_align_slot[BR_EMMC_ALIGN_SLOTS];
static br_u32       s_align_n;

void *uboot_mmc_env_memalign(br_size_t align, br_size_t size)
{
    if (size == 0u || s_align_n >= BR_EMMC_ALIGN_SLOTS) {
        return BR_NULL;
    }

    /* At least one cache line: upstream flushes ROUND(table_len, ARCH_DMA_MINALIGN). */
    br_size_t need = (size < (br_size_t)ARCH_DMA_MINALIGN) ? (br_size_t)ARCH_DMA_MINALIGN : size;

    br_dma_buf_t b;
    if (br_dma_alloc(need, BR_DMA_F_ALIGN4K, &b) != 0) {
        return BR_NULL;
    }

    /* `br_dma_alloc` gives 4 KiB alignment >> ARCH_DMA_MINALIGN(64). */
    const br_uintptr_t want = (align == 0u) ? 1u : (br_uintptr_t)align;
    if (((br_uintptr_t)b.vaddr % want) != 0u) {
        (void)br_dma_free(&b);
        return BR_NULL;
    }

    s_align_slot[s_align_n] = b;
    s_align_n++;
    return b.vaddr;
}

/* ---- MMIO address -> instance lookup ---- */
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

/* ---- wait: yield the CPU until the next ms or a device interrupt ----
 * Upstream `get_timer()`/`udelay()` all land here, so its polling loop becomes "sleep -> device
 * interrupt -> woken by bh -> read INT_STATUS again". If the address is in no SDHCI window, the
 * channel is not ready yet, or the scheduler is not up, fall back to one tick so the env layer is
 * always usable. Otherwise re-open this card's `SIGNAL_ENABLE` first, then sleep for at most one
 * tick: a lost interrupt costs one tick, never a hang. */
/* ---- spin: busy-wait (u-boot `udelay` semantics) ----
 * Upstream uses `udelay()` only for "register stable/reset done" (10/100/1000/2000 us), which has
 * no device event to wait for; one tick of sleep (5 ms) would inflate those, so do not yield. */
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

    /* No SDHCI window / channel not registered / scheduler not up yet: yield once for `win`. */
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
     * Open the signal source before sleeping: a level interrupt asserts on `INT_STATUS &
     * SIGNAL_ENABLE`, so it must be open to be woken by the next interrupt, and `INT_STATUS` must
     * not be cleared (the upstream loop reads it). The awaited bit is often already set, in which
     * case the line asserts at once. Masking after the wake, or opening only sources not already
     * pending, both starve the next wait (measured: waits fall back to the 1 ms timeout). The only
     * open point is here; upstream `sdhci_send_command()` clears `INT_STATUS` at command end.
     */
    emmc_signal_enable(d);

    (void)br_sem_take(&d->sem, (br_time_t)win);

    /* Accounting, three outcomes: `wake_irq` (ISR grew in this window), `wake_sem` (token already
     * present at take => returned immediately, still interrupt evidence), `wake_to` (slept the full
     * window, the fallback). Case 2 is common because the ISR often gives the token while upstream
     * reads more registers before waiting again. */
    if (d->isr_count != isr_before) {
        d->wake_irq++;
    } else if ((br_clock_now() - t0) < ((br_time_t)win / 2u)) {
        d->wake_sem++;
    } else {
        d->wake_to++;
    }
}

/* Weak "board" hooks upstream wants: probing is done by glue (PCIe enumeration), not
 * `mmc_initialize()`; semantics match u-boot's default (-1 = no static mmc list). */
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

/* Upstream block-layer hooks. Semantics match `drivers/mmc/mmc_legacy.c`'s `mmc_select_hwpartp()`:
 * upstream registers it in the `U_BOOT_LEGACY_BLK(mmc)` table, which this build does not have. */
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

/* `part_init()`: upstream `mmc_startup()` calls it at the end when `!DM_MMC`. This build
 * deliberately does no software partitioning (v1 exposes the raw device), so it is empty. */
void part_init(struct blk_desc *desc)
{
    (void)desc;
}

/*
 * `blk_dread`/`blk_dwrite` are deliberately not defined here: this plugin's upstream has no call
 * site (non-BLK upstream installs `mmc_bread`/`mmc_bwrite` into `blk_desc`), while
 * `adapter/uboot-fat/glue/fat_blk.c` defines them for its caller, so defining them here would
 * duplicate symbols. `compat/part.h` still declares them (upstream header surface).
 */

/* ---- bdev ops ---- */

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

    /* Upstream `mmc_bread()` returns 0 out of range (u-boot's "0 = error"), while the bdev contract
     * is "count or negative errno", so translate to -EIO before any command reaches the card
     * (TC-EMMC-004: out of range must not reach the card). */
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
     * WORKAROUND(br-wa-emmc-001): v1 flush only waits for the card to be not busy: no CMD6
     * CACHE_FLUSH is issued and CACHE_CTRL is not checked; see `WORKAROUNDS.md`.
     * Upstream has no flush entry point, so this uses upstream's `mmc_poll_for_busy()` (its
     * `mmc_wait_dat0` is -ENOSYS here, so it takes the SEND_STATUS polling branch).
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

/* Runtime SET_PATH: flips the `USE_ADMA` bit in upstream `host.flags`. `sdhci_send_command()` picks
 * DMA vs PIO from that bit alone, so there is no threshold/alignment fallback logic. */
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

/* ---- observability ---- */

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
    /* Invariant: no thread waiting => INTx line not asserted. Assertion is hardware-defined as
     * `INT_STATUS & SIGNAL_ENABLE`, so measure that rather than the source bit. */
    const u32 st = sdhci_readl(&s_dev[idx].host, SDHCI_INT_STATUS);
    const u32 se = sdhci_readl(&s_dev[idx].host, SDHCI_SIGNAL_ENABLE);

    return ((st & se) == 0u) ? BR_TRUE : BR_FALSE;
}

/* ---- lifecycle ---- */

int uboot_mmc_early_init(void)
{
    return 0;   /* no IRQs/no heap yet; the register window waits for pci-core CORE enumeration */
}

int uboot_mmc_init(void)
{
    return 0;   /* see header and README: real init happens in start (global IRQs enabled) */
}

/* Build `emmc<d>` from the device index (no snprintf; only 0..9). */
static void emmc_make_name(char *out, br_u32 idx)
{
    out[0] = 'e';
    out[1] = 'm';
    out[2] = 'm';
    out[3] = 'c';
    out[4] = (char)('0' + (char)(idx % 10u));
    out[5] = '\0';
}

/* Probe and register one card; the first failing step stops. */
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

    /* 1. Static interface fields of the upstream host instance; `add_sdhci()` fills the rest
     * (`version`/`max_clk`/`clk_mul`/`flags`/`cfg`) via upstream `sdhci_setup_cfg()`. */
    d->host.name    = d->name;          /* upstream uses it as cfg->name (diagnostics) */
    d->host.ioaddr  = (void *)d->base;
    d->host.mmc     = BR_NULL;
    d->host.ops     = BR_NULL;          /* this change does not use the vendor SDHCI ops table */
    d->host.quirks  = 0u;               /* card-level quirks set by CONFIG_MMC_QUIRKS in mmc.c */

    /* 2. The IRQ channel must be up before upstream init: the wait loop must be wakeable from the
     * first command (`sdhci_init()` writes SIGNAL_ENABLE = 0; the wait primitive re-opens it). */
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

    /* 3. Upstream host -> mmc device assembly, exactly `add_sdhci()`: `sdhci_setup_cfg()` (caps,
     * clock, b_max, ADMA2 table) plus `mmc_create()`. No line rewritten. */
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
     * Do not open `SIGNAL_ENABLE` here: with no thread waiting, a level line driven by `INT_STATUS`
     * bits upstream read but did not clear keeps delivering and crosses core's IRQCONF selftest
     * (which runs in bh context). Invariant: no waiter => `SIGNAL_ENABLE` is 0, opened only by the
     * wait primitive.
     */

    br_log_info("uboot-mmc: SDHCI @ 0x%lx (caps=0x%08x, max_clk=%u Hz, intx virq=%u)",
                (unsigned long)d->base, (unsigned)sdhci_readl(&d->host, SDHCI_CAPABILITIES),
                (unsigned)d->host.max_clk, (unsigned)d->intx_virq);

    /* 4. Start the upstream protocol core (CMD0..CMD7 + EXT_CSD + bus width/timing); upstream's
     * `cfg->ops->init` (`sdhci_init()`) clears SIGNAL_ENABLE, which is our invariant. */
    rc = mmc_init(d->mmc);
    if (rc != 0) {
        br_log_error("uboot-mmc: %s mmc_init failed rc=%d", d->name, rc);
        return rc;
    }
    /* Upstream `sdhci_init()` writes `SIGNAL_ENABLE` = 0, exactly the invariant above; not
     * re-opened. */

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

    /* Gate criterion line, all values from upstream state. `mmc_mode_name()` is compiled out by
     * upstream's `#if CONFIG_LOGLEVEL >= LOGL_DEBUG`, so map the first `enum bus_mode` entries. */
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

    /* Initialise upstream's device list once (resets `cur_dev_num` to 0, giving devnum 0..3 =
     * emmc0..emmc3); devices come from PCIe enumeration, not `mmc_initialize()`. */
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
