/*
 * brickOS prototype v0.2.0 - block device subcategory (hal/bdev-core). Owns a static pool of
 * BR_CLASS_BDEV entries in dev-core: br_bdev_register validates name/ops/geometry once at
 * registration (geometry is a registration precondition, so a bad device never enters the table),
 * and br_bdev_get binds {ops, priv} by name. No dynamic allocation; no unregister (v1 static).
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>

#include <br/dev/br_bdev.h>
#include <br/dev/br_dev.h>

/* Internal view for the self-test (unused in production; see that header). */
#include "bdev_internal.h"

/* Lifecycle hooks referenced by generated plugin_desc.c (not exported). */
int bdev_core_early_init(void);
int bdev_core_init(void);
int bdev_core_start(void);

/* Block device upper bound (prototype: static bound, enough is enough). */
#define BR_BDEV_MAX   8u

/*
 * One bdev registration record. dev-core receives the slot pointer (not the driver private), so
 * "this entry belongs to this file" has pointer identity; ops and priv stay reachable there.
 */
typedef struct bdev_slot {
    br_bool             used;
    const br_bdev_ops_t *ops;
    void                *priv;
} bdev_slot_t;

static bdev_slot_t s_bdevs[BR_BDEV_MAX];
static br_u32      s_bdev_n;

static br_bool bdev_is_pow2(br_u32 v)
{
    return ((v != 0u) && ((v & (v - 1u)) == 0u)) ? BR_TRUE : BR_FALSE;
}

/*
 * Geometry criteria enforced at registration: sector_size non-zero and a power of two; n_sectors >
 * 0; 0 < max_rw <= n_sectors; align 0 or a power of two; DMA and PIO not both set (neither = path
 * undeclared, allowed).
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

static bdev_slot_t *bdev_find(const char *name)
{
    const br_dev_class_entry_t *e = br_dev_lookup(name);

    if (e == BR_NULL || e->class_id != (br_u16)BR_CLASS_BDEV) {
        return BR_NULL;
    }

    /* Identify by "slot falls in this pool", not by class_priv: the class may hold subtypes. */
    bdev_slot_t *slot = (bdev_slot_t *)e->dev_priv;
    for (br_u32 i = 0u; i < s_bdev_n; i++) {
        if (&s_bdevs[i] == slot && s_bdevs[i].used == BR_TRUE) {
            return slot;
        }
    }
    return BR_NULL;
}

int br_bdev_register(const char *name, const br_bdev_ops_t *ops, void *priv)
{
    if (ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /* read/write/flush/geometry are required; ioctl/suspend/resume optional => -ENOTSUP */
    if (ops->read == BR_NULL || ops->write == BR_NULL ||
        ops->flush == BR_NULL || ops->geometry == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (br_dev_name_valid(name) == BR_FALSE) {
        return BR_ERR(EINVAL);   /* covers name == BR_NULL / empty / uppercase / containing slash */
    }

    /* Geometry is a registration precondition: pass the driver's error through, judge it here. */
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
        .class_priv = (void *)ops,     /* shape interpretation belongs to this file; dev-core passes through */
        .open_file  = BR_NULL,         /* raw block access is v2: visible but not openable */
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
        return BR_NULL;                /* not registered: does not write *priv */
    }
    if (priv != BR_NULL) {
        *priv = slot->priv;
    }
    return slot->ops;
}

/* EARLY: no action (the static pool is BSS); drivers register in their CORE init. */
int bdev_core_early_init(void)
{
    return 0;
}

/* CORE: no state of its own; it enters the stage when a driver calls br_bdev_register. */
int bdev_core_init(void)
{
    return 0;
}

/* START: registry summary only; conformance cases run via core's br_plugin_manager_selftest(). */
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

/* Self-test view: capacity and occupied slot count, forwarding to production state (not copies). */

br_u32 bdev_internal_capacity(void)
{
    return (br_u32)BR_BDEV_MAX;
}

br_u32 bdev_internal_slot_count(void)
{
    return s_bdev_n;
}
