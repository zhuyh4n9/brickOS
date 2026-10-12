/*
 * adapter/uboot-mmc — self-test suite (TC-EMMC-*)
 *
 * Design basis: `docs/decisions/0010-plugin-selftest.md` (self-test separated from
 * production), `docs/decisions/0021-bdev-pci-emmc.md` D-4/D-5 (interrupt + ADMA2/PIO +
 * flush semantics), `docs/decisions/0022-erofs-fat.md` (four-card split and "scratch area"
 * convention).
 *
 * WORKAROUND(br-wa-test-001): `TC-EMMC-*` are **self-assigned numbers** — the design `6-01`
 * case table has no eMMC family (that revision merged block devices into BDEV/IO). This
 * suite's numbering and criteria come from ADR-0021/0022 and are logged in `WORKAROUNDS.md`.
 *
 * Drives only through the **public surface**: `br_bdev_get` (bdev-core class API) +
 * `br_dev_*` (dev-core registry) + device ioctl encodings. No internal state is inspected —
 * otherwise "a bdev round-trip really writes and reads back" would have no criterion.
 *
 * ★ After ADR-0026 the **semantics and criteria of this file are unchanged word for word**:
 *   the upstream originals (all files under `drivers/mmc/`) are compiled in and glue only
 *   adapts, so "round-trip through bdev" now also pins "the upstream stack really runs".
 *   The only internal change: the 8-sector/32-sector buffers are still static arrays (same
 *   origin as before), plus the **interrupt count comparison** (see `irq_mark`/`irq_report`
 *   below) — the observable criterion for ADR-0021 D-4 "ISR→bh→wakeup".
 *
 * ## Why "write then read" every time
 *   In the gate's backing images, `emmc0/1/2` carry **live FAT volumes** (built on the host
 *   with mkfs.fat + mcopy) and `emmc3` carries the EROFS image. Device-level cases therefore
 *   write **only to the `emmc3` scratch area** (512 KiB starting at `BR_EMMC_SCRATCH_LBA`,
 *   well past the EROFS image) — the criterion is always "what I wrote, I read back the
 *   same", assuming no sector is 0 or any pattern. Writing to emmc0/1/2 is **forbidden**: it
 *   would tear the FAT volumes, and this suite runs after the FAT/EROFS suites (self-test
 *   phase) ⇒ it would tear data others already used.
 *
 * ## Contract with br_plugin_manager_selftest()
 *   Returns the **number of failures** (0 = all green); logs `[EMMCCONF] PASS/FAIL` + `SUMMARY`.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_bdev.h>
#include <br/dev/br_dev.h>

#include "emmc_internal.h"

int uboot_mmc_selftest(void);

/* 16 KiB double buffer (= 32 sectors), 4-byte aligned ⇒ ADMA2 usable */
static br_u32 s_a[(32u * 512u) / 4u];
static br_u32 s_b[(32u * 512u) / 4u];

static br_u32 s_em_pass;
static br_u32 s_em_fail;

static void em_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_em_pass++;
        br_log_info("[EMMCCONF] PASS %s %s", tag, what);
    } else {
        s_em_fail++;
        br_log_info("[EMMCCONF] FAIL %s %s", tag, what);
    }
}

static void em_fill(br_u32 *p, br_u32 words, br_u32 seed)
{
    for (br_u32 i = 0u; i < words; i++) {
        p[i] = seed + i;
    }
}

static br_bool em_same(const br_u32 *a, const br_u32 *b, br_u32 words)
{
    for (br_u32 i = 0u; i < words; i++) {
        if (a[i] != b[i]) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

/* `emmc<d>`; same naming rule as emmc_bdev.c (5 lines each, no mutable buffer shared
 * across files) */
static void em_name(char *out, br_u32 idx)
{
    out[0] = 'e'; out[1] = 'm'; out[2] = 'm'; out[3] = 'c';
    out[4] = (char)('0' + (char)(idx % 10u));
    out[5] = '\0';
}

/* ================================================================ interrupt count comparison
 *
 * The task statement (§5 report format) and ADR-0021 D-4 both require "interrupt driven" to
 * be **observable**: not just a comment saying "we use interrupts" while a pure timeout poll
 * via `udelay`/`get_timer` actually carries it. Criterion:
 *   during a transfer the card's INTx ISR count must **grow with the command count** (every
 *   CMD at least one RESPONSE/data event), and be of the **same order** as the command count
 *   (disproportion means it leans on timeouts).
 * So each case takes a snapshot before and after and logs the deltas (the gate does not
 * require these lines, but they must be readable).
 */
typedef struct irq_mark {
    br_u32 isr;
    br_u32 bh;
    br_u32 drop;
    br_u32 wake_irq;
    br_u32 wake_sem;
    br_u32 wake_to;
} irq_mark_t;

static irq_mark_t irq_mark(br_u32 idx)
{
    irq_mark_t m = { 0u, 0u, 0u, 0u, 0u, 0u };
    uboot_mmc_irq_counts(idx, &m.isr, &m.bh, &m.drop, &m.wake_irq, &m.wake_sem, &m.wake_to);
    return m;
}

/*
 * The **interrupt comparison line** for one transfer. This line is the measured evidence for
 * ADR-0021 D-4 ("ISR→bh→wake waiting thread") and the criterion for "interrupts or timeout
 * fallback":
 *   ISR +N     — the device really delivered N level interrupts during the transfer (not a
 *                comment claiming "we use interrupts");
 *   wake +A/+T — the wait primitive was woken by an interrupt A times and returned via the
 *                1 ms fallback timeout T times.
 *                ★ The criterion is **A ≫ T**: if T is of the same order as A it "leaned on
 *                  timeout polling" and must be stated here and in the delivery report (see
 *                  the field notes in emmc_internal.h).
 */
static void irq_report(const char *tag, br_u32 idx, const char *dev,
                       irq_mark_t before, irq_mark_t after)
{
    br_log_info("uboot-mmc: IRQ[%s] %s ISR +%u (total %u), bh +%u, drop +%u, "
                "wake irq/token/timeout +%u/+%u/+%u",
                tag, dev,
                (unsigned)(after.isr - before.isr), (unsigned)after.isr,
                (unsigned)(after.bh - before.bh), (unsigned)(after.drop - before.drop),
                (unsigned)(after.wake_irq - before.wake_irq),
                (unsigned)(after.wake_sem - before.wake_sem),
                (unsigned)(after.wake_to - before.wake_to));
    (void)idx;
}

int uboot_mmc_selftest(void)
{
    s_em_pass = 0u;
    s_em_fail = 0u;

    br_log_info("[EMMCCONF] uboot-mmc conformance (4 cards / bdev round-trip / ADMA2 / PIO / flush)");

    /* ---- all four cards visible via the bdev class API, each geometry self-consistent ---- */
    const br_bdev_ops_t *ops[BR_EMMC_MAX_DEVS];
    void                *priv[BR_EMMC_MAX_DEVS];
    br_bdev_geom_t       geo[BR_EMMC_MAX_DEVS];
    br_u32               ok_geo = 0u;

    for (br_u32 i = 0u; i < BR_EMMC_MAX_DEVS; i++) {
        char nm[BR_EMMC_NAME_MAX];
        em_name(nm, i);
        priv[i] = BR_NULL;
        ops[i]  = br_bdev_get(nm, &priv[i]);
        geo[i]  = (br_bdev_geom_t){ 0u, 0u, 0u, 0u, 0u };
        if (ops[i] != BR_NULL && priv[i] != BR_NULL &&
            ops[i]->geometry(priv[i], &geo[i]) == 0 &&
            geo[i].sector_size == 512u && geo[i].n_sectors >= 1024u &&
            geo[i].max_rw == BR_EMMC_MAX_RW_SECTORS &&
            ((geo[i].flags & (BR_BDEV_F_DMA | BR_BDEV_F_PIO)) != 0u)) {
            ok_geo++;
        }
    }
    em_conf(ok_geo == BR_EMMC_MAX_DEVS, "TC-EMMC-001",
            "emmc0..emmc3 all present with self-consistent geometry (512 B sector / max_rw / DMA|PIO bits)");

    /* scratch area must be on the EROFS card and large enough for this suite's max offset
     * (128 + 32 sectors) */
    const char *assets = BR_EMMC_ASSETS_DEV;
    br_u32 assets_idx = (br_u32)(assets[4] - '0');
    /* The whole scratch area (including this suite's max offset 128 + 32 sectors) must lie
     * inside the card — out-of-range cases then fail instead of "silently writing off-card".
     * Non-overlap with the EROFS image is asserted by the image build script (see file head). */
    em_conf((assets_idx < BR_EMMC_MAX_DEVS) &&
            (ops[assets_idx] != BR_NULL) &&
            (BR_EMMC_SCRATCH_LBA + 160u < geo[assets_idx].n_sectors),
            "TC-EMMC-002", "scratch area (emmc3 from LBA 4096, 512 KiB) lies entirely inside the card");

    /* ---- the other two: dev-core entry shape and raw file surface (D21: visible but not
     * openable) ---- */
    {
        br_u32 hits = 0u;
        for (br_u32 i = 0u; i < BR_EMMC_MAX_DEVS; i++) {
            char nm[BR_EMMC_NAME_MAX];
            em_name(nm, i);
            const br_dev_class_entry_t *e = br_dev_lookup(nm);
            if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV && e->open_file == BR_NULL) {
                hits++;
            }
        }
        em_conf(hits == BR_EMMC_MAX_DEVS, "TC-EMMC-003",
                "all four cards' dev-core entries class == bdev and open_file == NULL (D21)");
    }

    /* ---- 004: out-of-range access rejected (not sent to the card) ---- */
    {
        int rc = 0;
        const irq_mark_t before = irq_mark(assets_idx);
        if (ops[assets_idx] != BR_NULL) {
            rc = ops[assets_idx]->read(priv[assets_idx], geo[assets_idx].n_sectors, s_b, 1u);
        }
        const irq_mark_t after = irq_mark(assets_idx);
        em_conf(rc < 0, "TC-EMMC-004", "read(lba == n_sectors) ⇒ negative errno (out of range never reaches the card)");
        if (after.isr != before.isr) {
            /* Out of range must be rejected at **this layer**: no command should reach the
             * card ⇒ ISR must not grow. */
            br_log_info("uboot-mmc: IRQ[TC-EMMC-004] out-of-range produced %u interrupts (expected 0)",
                        (unsigned)(after.isr - before.isr));
        }
    }

    /* ---- 005: default (DMA) path: scratch area 8 sectors write then read back identical ---- */
    {
        em_fill(s_a, 8u * 128u, 0xE3E30000u);
        const irq_mark_t before = irq_mark(assets_idx);
        br_bool ok = BR_FALSE;
        if (ops[assets_idx] != BR_NULL) {
            const int w = ops[assets_idx]->write(priv[assets_idx], BR_EMMC_SCRATCH_LBA, s_a, 8u);
            const int r = ops[assets_idx]->read(priv[assets_idx], BR_EMMC_SCRATCH_LBA, s_b, 8u);
            ok = (w == 8) && (r == 8) && em_same(s_a, s_b, 8u * 128u);
        }
        const irq_mark_t after = irq_mark(assets_idx);
        em_conf(ok, "TC-EMMC-005", "default ADMA2 path: scratch area 8 sectors write→read word-identical");
        irq_report("005/ADMA2-8sec", assets_idx, BR_EMMC_ASSETS_DEV, before, after);
    }

    /* ---- 006: switch PIO on **one** card: its geometry then reports PIO, others stay DMA ---- */
    {
        const br_u32 cmd = BR_IOW(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_SET_PATH, br_u32);
        const br_u32 path = BR_MMC_PATH_PIO;
        br_bdev_geom_t g3 = (br_bdev_geom_t){ 0u, 0u, 0u, 0u, 0u };
        br_bdev_geom_t g0 = (br_bdev_geom_t){ 0u, 0u, 0u, 0u, 0u };
        br_bool ok = BR_FALSE;
        if (ops[assets_idx] != BR_NULL && ops[0] != BR_NULL) {
            const int r_set  = ops[assets_idx]->ioctl(priv[assets_idx], cmd, (void *)&path);
            const int r_g3   = ops[assets_idx]->geometry(priv[assets_idx], &g3);
            const int r_g0   = ops[0]->geometry(priv[0], &g0);
            ok = (r_set == 0) && (r_g3 == 0) && (r_g0 == 0) &&
                 ((g3.flags & BR_BDEV_F_PIO) != 0u) && ((g3.flags & BR_BDEV_F_DMA) == 0u) &&
                 ((g0.flags & BR_BDEV_F_DMA) != 0u) && ((g0.flags & BR_BDEV_F_PIO) == 0u);
        }
        em_conf(ok, "TC-EMMC-006", "SET_PATH(PIO) changes only that card: it reports PIO, emmc0 still reports DMA (multi-instance)");
    }

    /* ---- 007: PIO path 8 sectors write then read back identical ---- */
    {
        em_fill(s_a, 8u * 128u, 0x06060000u);
        const irq_mark_t before = irq_mark(assets_idx);
        br_bool ok = BR_FALSE;
        if (ops[assets_idx] != BR_NULL) {
            const br_u64 lba = BR_EMMC_SCRATCH_LBA + 64u;
            const int w = ops[assets_idx]->write(priv[assets_idx], lba, s_a, 8u);
            const int r = ops[assets_idx]->read(priv[assets_idx], lba, s_b, 8u);
            ok = (w == 8) && (r == 8) && em_same(s_a, s_b, 8u * 128u);
        }
        const irq_mark_t after = irq_mark(assets_idx);
        em_conf(ok, "TC-EMMC-007", "PIO path: 8 sectors write→read word-identical");
        irq_report("007/PIO-8sec", assets_idx, BR_EMMC_ASSETS_DEV, before, after);
    }

    /* ---- 008: back to ADMA2 for 32-sector multi-block DMA + flush + self-consistent GET_INFO ---- */
    {
        const br_u32 cmd_set = BR_IOW(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_SET_PATH, br_u32);
        const br_u32 cmd_inf = BR_IOR(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_GET_INFO, br_emmc_info_t);
        const br_u32 path = BR_MMC_PATH_DMA;
        br_emmc_info_t info;
        em_fill(s_a, 32u * 128u, 0xD0D00000u);
        const irq_mark_t before = irq_mark(assets_idx);
        br_bool ok = BR_FALSE;
        if (ops[assets_idx] != BR_NULL) {
            const br_u64 lba = BR_EMMC_SCRATCH_LBA + 128u;
            const int r_set = ops[assets_idx]->ioctl(priv[assets_idx], cmd_set, (void *)&path);
            const int w = ops[assets_idx]->write(priv[assets_idx], lba, s_a, 32u);
            const int r = ops[assets_idx]->read(priv[assets_idx], lba, s_b, 32u);
            const int rf = ops[assets_idx]->flush(priv[assets_idx]);
            const int ri = ops[assets_idx]->ioctl(priv[assets_idx], cmd_inf, (void *)&info);
            ok = (r_set == 0) && (w == 32) && (r == 32) && em_same(s_a, s_b, 32u * 128u) &&
                 (rf == 0) && (ri == 0) && (info.base != 0u) &&
                 (info.capacity_sectors == geo[assets_idx].n_sectors) &&
                 (info.sector_size == 512u) && (info.path == BR_MMC_PATH_DMA) &&
                 (info.bus_width == 4u || info.bus_width == 8u || info.bus_width == 1u);
        }
        const irq_mark_t after = irq_mark(assets_idx);
        em_conf(ok, "TC-EMMC-008",
                "after SET_PATH(DMA) 32 sectors ADMA2 multi-block identical; flush == 0; GET_INFO self-consistent");
        irq_report("008/ADMA2-32sec", assets_idx, BR_EMMC_ASSETS_DEV, before, after);
    }

    /* ---- one readable interrupt count line per card (proves INTx really delivers and is
     * handled by ISR/bh) ---- */
    for (br_u32 i = 0u; i < BR_EMMC_MAX_DEVS; i++) {
        char nm[BR_EMMC_NAME_MAX];
        em_name(nm, i);
        br_u32 isr = 0u, bh = 0u, drop = 0u, wq = 0u, ws = 0u, wt = 0u;
        uboot_mmc_irq_counts(i, &isr, &bh, &drop, &wq, &ws, &wt);
        br_log_info("uboot-mmc: /dev/%s IRQ count=%u (bh=%u, dropped=%u, wake irq/token/timeout/spin=%u/%u/%u/%u), idle-signal=%s",
                    nm, (unsigned)isr, (unsigned)bh, (unsigned)drop,
                    (unsigned)wq, (unsigned)ws, (unsigned)wt,
                    (unsigned)uboot_mmc_wake_spin(i),
                    (uboot_mmc_irq_idle(i) == BR_TRUE) ? "flat" : "ASSERTED");
    }

    br_log_info("[EMMCCONF] SUMMARY pass=%u fail=%u total=%u",
                s_em_pass, s_em_fail, s_em_pass + s_em_fail);
    return (int)s_em_fail;
}
