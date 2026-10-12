/*
 * brickOS prototype v0.2.0 — **self-test suite** of framework/bdev-core (src/bdev_selftest.c)
 *
 * Design basis and rulings: `docs/decisions/0010-plugin-selftest.md` (self-test separated from core code),
 * `docs/decisions/0021-bdev-pci-emmc.md` (bdev shape and raw surface = v2).
 *
 * ## What it tests, and where the boundary with production code lies
 *   The block device subclass's **mechanism** (own numbering `TC-BDEV-*`): register-time validation of
 *   name/ops/geometry, retrieval semantics, registry bookkeeping, and "a bdev entry has no file surface"
 *   (D21 ⇒ `/dev/<bdev>` is visible but not openable).
 *   It does **not** test any concrete medium — real block round-trips live in the driver's own suite
 *   (e.g. `adapter/uboot-mmc`'s `TC-EMMC-*`); that is evidence about a "device", not about the "subclass framework".
 *
 * ## Why many cases use "legal name + illegal input"
 *   `br_bdev_register` validates **before entering the table**, so pairing a "legal, currently free name"
 *   with illegal ops/geometry covers every rejection branch while **registering nothing** (the registry is
 *   not polluted, so cases have no ordering coupling). Same trick as cdev-core's
 *   `br_cdev_register('cdevx', <ops with only open>)`.
 *
 * ## Contract with br_plugin_manager_selftest()
 *   Returns the **number of failures** (0 = all green); logs itself (`[BDEVCONF] PASS/FAIL ...` + `SUMMARY`).
 *   Failure does not halt — red/green is judged by the gate (`[BDEVCONF] FAIL` is in tests/gates.toml's forbid).
 */
#include <br/dev/br_bdev.h>     /* public contract (br_bdev_ops_t / br_bdev_geom_t) */

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

#include "bdev_internal.h"

/* Self-test entry prototype (defined at the end of this file; the generated `plugin_desc.c` `.selftest` hook calls it). */
int bdev_core_selftest(void);

/* =====================================================================
 * Probe device (serves only this file, not registered)
 *
 * geometry is answered verbatim from the `br_bdev_geom_t` that priv points at ⇒ one ops table can play
 * both "legal geometry" and "various illegal geometry" roles, without an ops table per illegal value.
 * ===================================================================== */

static int probe_geometry(void *priv, br_bdev_geom_t *g)
{
    if (priv == BR_NULL || g == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *g = *(const br_bdev_geom_t *)priv;
    return 0;
}

static int probe_read(void *priv, br_u64 lba, void *buf, br_size_t n_lba)
{
    (void)priv; (void)lba; (void)buf; (void)n_lba;
    return 0;                       /* never called by this suite: the validation path has no I/O */
}

static int probe_write(void *priv, br_u64 lba, const void *buf, br_size_t n_lba)
{
    (void)priv; (void)lba; (void)buf; (void)n_lba;
    return 0;
}

static int probe_flush(void *priv)
{
    (void)priv;
    return 0;
}

/* "good ops" with all four required slots present (the only variable part is geometry injected via priv) */
static const br_bdev_ops_t s_good_ops = {
    .read     = probe_read,
    .write    = probe_write,
    .flush    = probe_flush,
    .geometry = probe_geometry,
    .ioctl    = BR_NULL,
    .suspend  = BR_NULL,
    .resume   = BR_NULL,
};

/* bad ops with a missing slot (each table lacks exactly one, pinning each "required" slot individually) */
static const br_bdev_ops_t s_no_read_ops = {
    .read = BR_NULL, .write = probe_write, .flush = probe_flush, .geometry = probe_geometry,
};
static const br_bdev_ops_t s_no_write_ops = {
    .read = probe_read, .write = BR_NULL, .flush = probe_flush, .geometry = probe_geometry,
};
static const br_bdev_ops_t s_no_flush_ops = {
    .read = probe_read, .write = probe_write, .flush = BR_NULL, .geometry = probe_geometry,
};
static const br_bdev_ops_t s_no_geom_ops = {
    .read = probe_read, .write = probe_write, .flush = probe_flush, .geometry = BR_NULL,
};

/* legal "geometry baseline" (512 B sectors / 8 sectors / max_rw 4 / no alignment requirement / PIO) */
#define PROBE_GEOM_SECTOR   512u
#define PROBE_GEOM_SECTORS  8u
#define PROBE_GEOM_MAXRW    4u

/* =====================================================================
 * Conformance cases (own numbering TC-BDEV-*; registered in br-wa-test-001)
 * ===================================================================== */

static br_u32 s_bd_pass;
static br_u32 s_bd_fail;

static void bd_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_bd_pass++;
        br_log_info("[BDEVCONF] PASS %s %s", tag, what);
    } else {
        s_bd_fail++;
        br_log_info("[BDEVCONF] FAIL %s %s", tag, what);
    }
}

/* count BR_CLASS_BDEV entries by walking the registry (public API) */
static br_u32 bd_registry_count(void)
{
    br_u32 n = 0u;
    for (br_u32 i = 0u; i < br_dev_count(); i++) {
        const br_dev_class_entry_t *e = br_dev_entry_at(i);
        if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV) {
            n++;
        }
    }
    return n;
}

static const char *bd_first_name(void)
{
    for (br_u32 i = 0u; i < br_dev_count(); i++) {
        const br_dev_class_entry_t *e = br_dev_entry_at(i);
        if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV) {
            return br_dev_name_at(i);
        }
    }
    return BR_NULL;
}

int bdev_core_selftest(void)
{
    s_bd_pass = 0u;
    s_bd_fail = 0u;

    br_log_info("[BDEVCONF] bdev-core conformance (register/get/geometry/raw-face)");

    /* ---- TC-BDEV-001: name rules (legal ops; rejection happens before table entry) ---- */
    {
        br_bdev_geom_t g = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        const br_u32   n0 = br_dev_count();
        const int r_null = br_bdev_register(BR_NULL, &s_good_ops, &g);
        const int r_empty = br_bdev_register("", &s_good_ops, &g);
        const int r_upper = br_bdev_register("Bdev0", &s_good_ops, &g);
        const int r_slash = br_bdev_register("b/dev", &s_good_ops, &g);
        const int r_under = br_bdev_register("b_dev", &s_good_ops, &g);

        const br_bool ok = (r_null == BR_ERR(EINVAL)) &&
                           (r_empty == BR_ERR(EINVAL)) &&
                           (r_upper == BR_ERR(EINVAL)) &&
                           (r_slash == BR_ERR(EINVAL)) &&
                           (r_under == BR_ERR(EINVAL)) &&
                           (br_dev_count() == n0);
        bd_conf(ok, "TC-BDEV-001",
                "NULL/empty/uppercase/slash/underscore name ⇒ -EINVAL and registry unchanged");
    }

    /* ---- TC-BDEV-002: required ops slots (legal free name + missing slot ⇒ -EINVAL, no table entry) ---- */
    {
        br_bdev_geom_t g = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        const br_u32   n0 = br_dev_count();
        const int r_ops  = br_bdev_register("bdevtest0", BR_NULL, &g);
        const int r_rd   = br_bdev_register("bdevtest0", &s_no_read_ops, &g);
        const int r_wr   = br_bdev_register("bdevtest0", &s_no_write_ops, &g);
        const int r_fl   = br_bdev_register("bdevtest0", &s_no_flush_ops, &g);
        const int r_gm   = br_bdev_register("bdevtest0", &s_no_geom_ops, &g);

        const br_bool ok = (r_ops == BR_ERR(EINVAL)) &&
                           (r_rd == BR_ERR(EINVAL)) &&
                           (r_wr == BR_ERR(EINVAL)) &&
                           (r_fl == BR_ERR(EINVAL)) &&
                           (r_gm == BR_ERR(EINVAL)) &&
                           (br_dev_count() == n0) &&
                           (br_bdev_get("bdevtest0", BR_NULL) == BR_NULL);
        bd_conf(ok, "TC-BDEV-002",
                "null ops / missing read|write|flush|geometry ⇒ -EINVAL and the name stays free");
    }

    /* ---- TC-BDEV-003: geometry self-consistency (geometry called at register time and rejected item by item) ---- */
    {
        const br_u32 n0 = br_dev_count();
        /* per item: {geometry, description} — geometry injected via priv */
        br_bdev_geom_t g_zero_sec = { 0u, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_not_pow2 = { 500u, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_zero_n   = { PROBE_GEOM_SECTOR, 0u, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_zero_rw  = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, 0u, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_rw_big   = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS,
                                      PROBE_GEOM_SECTORS + 1u, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_align    = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 3u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_both     = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u,
                                      BR_BDEV_F_DMA | BR_BDEV_F_PIO };

        const int r_sec  = br_bdev_register("bdevtest1", &s_good_ops, &g_zero_sec);
        const int r_np2  = br_bdev_register("bdevtest1", &s_good_ops, &g_not_pow2);
        const int r_n    = br_bdev_register("bdevtest1", &s_good_ops, &g_zero_n);
        const int r_rw0  = br_bdev_register("bdevtest1", &s_good_ops, &g_zero_rw);
        const int r_rwbg = br_bdev_register("bdevtest1", &s_good_ops, &g_rw_big);
        const int r_al   = br_bdev_register("bdevtest1", &s_good_ops, &g_align);
        const int r_both = br_bdev_register("bdevtest1", &s_good_ops, &g_both);

        const br_bool ok = (r_sec == BR_ERR(EINVAL)) &&
                           (r_np2 == BR_ERR(EINVAL)) &&
                           (r_n == BR_ERR(EINVAL)) &&
                           (r_rw0 == BR_ERR(EINVAL)) &&
                           (r_rwbg == BR_ERR(EINVAL)) &&
                           (r_al == BR_ERR(EINVAL)) &&
                           (r_both == BR_ERR(EINVAL)) &&
                           (br_dev_count() == n0);
        bd_conf(ok, "TC-BDEV-003",
                "sector=0/not a power of 2, n_sectors=0, max_rw=0/>n, align not a power of 2, DMA|PIO both set ⇒ -EINVAL");
    }

    /* ---- TC-BDEV-004: retrieval semantics (unregistered is NULL and does not write the out-param) ---- */
    {
        void *sentinel = (void *)&s_bd_pass;      /* any non-NULL sentinel */
        void *priv = sentinel;

        const br_bdev_ops_t *ops = br_bdev_get("nosuchbdev", &priv);
        const br_bdev_ops_t *ops_null = br_bdev_get(BR_NULL, BR_NULL);

        const br_bool ok = (ops == BR_NULL) && (priv == sentinel) && (ops_null == BR_NULL);
        bd_conf(ok, "TC-BDEV-004",
                "unregistered ⇒ NULL and *priv unchanged; br_bdev_get(NULL,NULL) ⇒ NULL");
    }

    /* ---- TC-BDEV-005: raw file surface = v2 (walk registry: bdev entries always have open_file NULL) ----
     * Mechanical criterion: whoever wires a file surface onto bdev (or lets the register path miss the NULL)
     * turns this red (same shape as TC-CDEV-008). */
    {
        br_bool all_null = BR_TRUE;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV && e->open_file != BR_NULL) {
                all_null = BR_FALSE;
            }
        }
        bd_conf(all_null, "TC-BDEV-005",
                "all BR_CLASS_BDEV entries have open_file == NULL (D21: raw block access is v2)");
    }

    /* ---- TC-BDEV-006: live device round-trip (when the composition includes a bdev; no bdev ⇒ "not applicable") ---- */
    {
        const char *name = bd_first_name();
        if (name == BR_NULL) {
            bd_conf(BR_TRUE, "TC-BDEV-006", "no bdev in the composition ⇒ not applicable (trivially passes)");
        } else {
            const br_dev_class_entry_t *e = br_dev_lookup(name);
            void           *priv = BR_NULL;
            const br_bdev_ops_t *ops = br_bdev_get(name, &priv);
            br_bdev_geom_t  g = { 0u, 0u, 0u, 0u, 0u };
            const int       grc = (ops != BR_NULL) ? ops->geometry(priv, &g) : BR_ERR(EINVAL);
            const int       dup = br_bdev_register(name, ops, priv);

            const br_bool ok = (ops != BR_NULL) && (e != BR_NULL) &&
                               (ops == (const br_bdev_ops_t *)e->class_priv) &&
                               (grc == 0) && (g.sector_size > 0u) && (g.n_sectors > 0u) &&
                               (g.max_rw > 0u) && ((br_u64)g.max_rw <= g.n_sectors) &&
                               (dup == BR_ERR(EEXIST));
            bd_conf(ok, "TC-BDEV-006",
                    "retrieved ops==entry class_priv; geometry self-consistent; duplicate register ⇒ -EEXIST");
        }
    }

    /* ---- TC-BDEV-007: pool bookkeeping (used <= capacity, and matches the registry entry count) ---- */
    {
        const br_u32 cap  = bdev_internal_capacity();
        const br_u32 used = bdev_internal_slot_count();
        const br_u32 reg  = bd_registry_count();

        const br_bool ok = (cap >= 1u) && (used <= cap) && (used == reg);
        bd_conf(ok, "TC-BDEV-007",
                "slot_count <= capacity and slot_count == registry bdev entry count");
    }

    /* ---- TC-BDEV-008: flags bitfield (live device; unknown bit ⇒ red) ---- */
    {
        const char *name = bd_first_name();
        if (name == BR_NULL) {
            bd_conf(BR_TRUE, "TC-BDEV-008", "no bdev in the composition ⇒ not applicable (trivially passes)");
        } else {
            void                *priv = BR_NULL;
            const br_bdev_ops_t *ops = br_bdev_get(name, &priv);
            br_bdev_geom_t       g = { 0u, 0u, 0u, 0u, 0u };
            const int            grc = (ops != BR_NULL) ? ops->geometry(priv, &g) : BR_ERR(EINVAL);
            const br_u32         known = BR_BDEV_F_READONLY | BR_BDEV_F_STACKED |
                                         BR_BDEV_F_DMA | BR_BDEV_F_PIO;
            const br_u32         both = BR_BDEV_F_DMA | BR_BDEV_F_PIO;

            const br_bool ok = (grc == 0) && ((g.flags & ~known) == 0u) &&
                               ((g.flags & both) != both);
            bd_conf(ok, "TC-BDEV-008",
                    "geometry.flags holds only known bits and not both DMA/PIO");
        }
    }

    br_log_info("[BDEVCONF] SUMMARY pass=%u fail=%u total=%u",
                s_bd_pass, s_bd_fail, s_bd_pass + s_bd_fail);
    return (int)s_bd_fail;
}
