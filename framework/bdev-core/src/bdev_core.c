/*
 * brickOS prototype v0.2.0 — block device subcategory (framework/bdev-core)
 *
 * Design basis: `docs/7-storage/7-02-bdev.md`
 *   §1 (block device class: geometry + sector read/write/flush + `br_bdev_register`/`br_bdev_get`;
 *     "bdev-core API: block devices register into the dev-core namespace" — the origin of the
 *     dev-core dependency)
 *   §2 (SD-9 partition mapper = v1.x → this cut keeps only the design)
 *   §3 (SD-8 page cache is a **stacking plugin**; this file only supplies the ground for "upper
 *     layers see the same `br_bdev_ops`")
 *   §4 (DMA/alignment/cache contract: when `align` is non-zero the caller aligns, otherwise the
 *     driver bounces internally)
 * Per-item rulings and deviations (including the origin of the `BR_BDEV_F_DMA/PIO` bits) are in
 * `docs/decisions/0021-bdev-pci-emmc.md`.
 *
 * ## This file's three duties (nothing else)
 *   ① `br_bdev_register`: validate (name / mandatory ops slots / geometry consistency) → add a
 *      `BR_CLASS_BDEV` entry in dev-core (`open_file = BR_NULL` — D21: raw block access is v2);
 *   ② `br_bdev_get`: retrieve `{ops, priv}` by name (the binding point for FS/stacking plugins);
 *   ③ static pool and bookkeeping (prototype stance: no dynamic allocation, same as
 *      dev-core/cdev-core).
 *
 * ## Why geometry is called once at registration
 *   Among the mandatory slots of a block device, `flush` is a **semantic precondition** (page
 *   cache relies on it to reach the medium, SD-8), and self-consistency questions like "is this
 *   read-only media" can only be answered by geometry. Rather than admit a pile of half-finished
 *   devices and blow up at the first I/O, call the driver once at registration: geometry
 *   consistency is a **precondition of registration** (same shape as the cdev flash subtype's
 *   geometry validation, except there geometry is a parameter and here it is an op).
 *   Cost: the driver's `geometry` must answer in the CORE phase (at registration) — natural for
 *   the "init device first, register second" order, and the only order v1 allows.
 *
 * ## Why DMA/PIO are bits in **geometry** rather than fields in ops
 *   The maintainer requires "data transfers must state DMA/PIO" (ADR-0021 D-4). Geometry is an
 *   observation surface **answerable again on every call** ⇒ after a runtime DMA/PIO switch the
 *   consumer sees the new value on its next `geometry`; as constant ops fields, switching would
 *   mean rewriting the ops table (while ops is const and its layout is in golden). This also
 *   lets "the currently active path" and "geometry" be read at one place, with no two truths.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>

#include <br/dev/br_bdev.h>
#include <br/dev/br_dev.h>

#include "bdev_internal.h"     /* internal view for self-test (see that header; unused in production) */

/* Lifecycle hook prototypes (name = symbol_prefix + phase; referenced by generated
 * `plugin_desc.c`; not in [[export]]) */
int bdev_core_early_init(void);
int bdev_core_init(void);
int bdev_core_start(void);

/* ==================================================================== static pool */

#define BR_BDEV_MAX   8u   /* block device upper bound (prototype: static bound, same "enough is enough" as BR_CDEV_MAX) */

/*
 * One bdev registration record. The `dev_priv` handed to dev-core is the **slot pointer** (not
 * the driver private itself) — so "this entry belongs to this file" has pointer-identity proof
 * (same as cdev-core), and when bdev later gains an open_file hook, the hook can see both ops and
 * driver private.
 */
typedef struct bdev_slot {
    br_bool             used;
    const br_bdev_ops_t *ops;
    void                *priv;
} bdev_slot_t;

static bdev_slot_t s_bdevs[BR_BDEV_MAX];
static br_u32      s_bdev_n;

/* ==================================================================== geometry validation */

static br_bool bdev_is_pow2(br_u32 v)
{
    return ((v != 0u) && ((v & (v - 1u)) == 0u)) ? BR_TRUE : BR_FALSE;
}

/*
 * Geometry consistency criteria (enforced at registration; each corresponds to a table that
 * "looks computable but computes wrong"):
 *   - sector_size is a power of two (a non-power-of-two is almost certainly a unit mistake) and non-zero;
 *   - n_sectors > 0 (zero capacity has no readable LBA);
 *   - 0 < max_rw <= n_sectors (a bound above the medium length = a path that always fails);
 *   - align is 0 or a power of two (0 = any, design `7-02` §4);
 *   - the DMA/PIO bits **cannot both be set** (self-contradictory); neither set = "path
 *     undeclared", allowed.
 */
static br_bool bdev_geom_ok(const br_bdev_geom_t *g)
{
    if (g->sector_size == 0u || bdev_is_pow2(g->sector_size) == BR_FALSE) {
        return BR_FALSE;
    }
    if (g->n_sectors == (br_u64)0u) {
        return BR_FALSE;
    }
    if (g->max_rw == 0u || (br_u64)g->max_rw > g->n_sectors) {
        return BR_FALSE;
    }
    if (g->align != 0u && bdev_is_pow2(g->align) == BR_FALSE) {
        return BR_FALSE;
    }
    if ((g->flags & (BR_BDEV_F_DMA | BR_BDEV_F_PIO)) == (BR_BDEV_F_DMA | BR_BDEV_F_PIO)) {
        return BR_FALSE;
    }
    return BR_TRUE;
}

/* ==================================================================== slot lookup */

static bdev_slot_t *bdev_find(const char *name)
{
    const br_dev_class_entry_t *e = br_dev_lookup(name);

    if (e == BR_NULL || e->class_id != (br_u16)BR_CLASS_BDEV) {
        return BR_NULL;
    }

    /* The same class_id may later hold subtypes (e.g. a partition mapper) ⇒ distinguish by "does
     * the slot fall in this file's pool", not by comparing class_priv (the theoretical case where
     * two drivers' ops happen to share an address). */
    bdev_slot_t *slot = (bdev_slot_t *)e->dev_priv;
    for (br_u32 i = 0u; i < s_bdev_n; i++) {
        if (&s_bdevs[i] == slot && s_bdevs[i].used == BR_TRUE) {
            return slot;
        }
    }
    return BR_NULL;
}

/* ==================================================================== register and retrieve */

int br_bdev_register(const char *name, const br_bdev_ops_t *ops, void *priv)
{
    if (ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /* Mandatory slots (the four actions of design `7-02` §1; ioctl/suspend/resume optional ⇒ -ENOTSUP) */
    if (ops->read == BR_NULL || ops->write == BR_NULL ||
        ops->flush == BR_NULL || ops->geometry == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (br_dev_name_valid(name) == BR_FALSE) {
        return BR_ERR(EINVAL);   /* covers name == BR_NULL / empty / uppercase / containing slash */
    }

    /* Geometry is a precondition of registration (see file header): the driver's own error is
     * passed through unchanged, consistency is judged by this file. */
    br_bdev_geom_t geom;
    const int grc = ops->geometry(priv, &geom);
    if (grc != BR_OK) {
        return grc;
    }
    if (bdev_geom_ok(&geom) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }

    if (s_bdev_n >= (br_u32)BR_BDEV_MAX) {
        return BR_ERR(ENOSPC);
    }

    bdev_slot_t *slot = &s_bdevs[s_bdev_n];
    slot->used = BR_TRUE;
    slot->ops  = ops;
    slot->priv = priv;

    const br_dev_class_entry_t e = {
        .class_id   = (br_u16)BR_CLASS_BDEV,
        .class_priv = (void *)ops,     /* shape interpretation belongs to this file; dev-core only passes through */
        .open_file  = BR_NULL,         /* ★ D21: raw block access in v1 is v2 (visible but not openable) */
        .dev_priv   = slot,
    };

    const int rc = br_dev_add(name, &e);
    if (rc != BR_OK) {
        slot->used = BR_FALSE;         /* rollback: a failure leaves no half-registered slot */
        return rc;                     /* -EINVAL / -EEXIST / -ENOSPC passed through unchanged */
    }

    s_bdev_n++;
    br_log_info("bdev: registered /dev/%s (sector=%u, %lu sectors, max_rw=%u, %s%s)",
                name, geom.sector_size, (unsigned long)geom.n_sectors, geom.max_rw,
                ((geom.flags & BR_BDEV_F_DMA) != 0u) ? "DMA" :
                (((geom.flags & BR_BDEV_F_PIO) != 0u) ? "PIO" : "path?"),
                ((geom.flags & BR_BDEV_F_READONLY) != 0u) ? ", ro" : "");
    return BR_OK;
}

const br_bdev_ops_t *br_bdev_get(const char *name, void **priv)
{
    bdev_slot_t *slot = bdev_find(name);
    if (slot == BR_NULL) {
        return BR_NULL;                /* not registered: does not write *priv (same contract as cdev-core) */
    }
    if (priv != BR_NULL) {
        *priv = slot->priv;
    }
    return slot->ops;
}

/* ==================================================================== lifecycle */

/* EARLY: no action (the static pool is BSS; no heap or threads yet). Device registration happens
 * in each driver's CORE init. */
int bdev_core_early_init(void)
{
    return 0;
}

/* CORE: no action. bdev-core has no state of its own to initialize — it "enters the stage" when a
 * driver calls `br_bdev_register`. Deliberately no empty API (R-S4: every line added to a framework
 * file must have a consumer). */
int bdev_core_init(void)
{
    return 0;
}

/* START phase: this file's conformance cases do **not** run here — after ADR-0010 they are driven
 * uniformly by core's `br_plugin_manager_selftest()` through the descriptor's `.selftest` hook
 * (see src/bdev_selftest.c). Here only a one-line registry summary is kept as boot evidence. */
int bdev_core_start(void)
{
    br_u32 n = 0u;
    for (br_u32 i = 0u; i < br_dev_count(); i++) {
        const br_dev_class_entry_t *e = br_dev_entry_at(i);
        if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV) {
            n++;
        }
    }
    br_log_info("bdev: %u block device(s) in registry", n);
    return 0;
}

/* ==================================================================== internal view for self-test
 *
 * See the boundary rules in `src/bdev_internal.h`. Here **only** the two things the self-test
 * really uses are exposed: capacity and occupied slot count (to assert the structural invariant
 * "table does not overflow").
 * Both **forward** to the same production state instead of copying constants — otherwise the case
 * compares against a copy.
 */

br_u32 bdev_internal_capacity(void)
{
    return (br_u32)BR_BDEV_MAX;
}

br_u32 bdev_internal_slot_count(void)
{
    return s_bdev_n;
}
