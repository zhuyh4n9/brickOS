/*
 * brickOS prototype v0.2.0 — character device subclass + generic session adaptation (framework/cdev-core)
 *
 * Design basis: `docs/8-device/8-01-device.md`
 *   §3 (`br_cdev_ops` session-shaped form + `br_cdev_register`; **flash subtype**
 *      `br_flash_ops` — spi-nor/nand hook up here; the unified reserved slots of D22/SD-14)
 *   §2 (open_file hook: returns {fops, fpriv} — the D21/D23 "device → file" adaptation entry)
 *   `docs/7-storage/7-03-concrete-fs.md` §3 ("cdev-core provides generic session adaptation
 *      `br_file_ops` (open creates a session / read/write/ioctl/poll/close forward;
 *      lseek/fsync slots NULL → -ENOTSUP)")
 * Per-item rulings in `docs/decisions/0009-vfs-storage-stack.md`.
 *
 * ## The three duties of this file (nothing else)
 *   ① `br_cdev_register`: validate → register a `BR_CLASS_CDEV` entry in dev-core,
 *      **and automatically install this unit's open_file hook** (drivers implement only
 *      ops; they need not know VFS);
 *   ② the **generic br_file_ops adaptation layer**: translate a "session-shaped cdev" into
 *      the "file face" (`7-03` §3);
 *   ③ flash subtype registration/retrieval (shaped like littlefs's lfs_config, zero glue).
 *
 * ## Why the adaptation layer lives in cdev-core, not devfs
 *   Design D21/SD-13: **devfs depends on no subclass ops shape** (it only obtains
 *   {fops, fpriv} via the open_file hook); vfs-core has zero device knowledge. Putting the
 *   adaptation in devfs would force devfs to know `br_cdev_ops` — exactly the dependency
 *   D21 removed. So "device semantics → file semantics" translation can only happen in the
 *   **subclass framework**: it knows both the device shape and `br_file_ops` (type
 *   dependency, O-S7).
 *
 * ## Where per-file private state lives (ADR-0009 §3 ruling 2)
 *   vfs-core hands the inode-carried `fpriv` (here = this unit's **device slot pointer**)
 *   to `br_file_t`; the adaptation layer's `open` swaps it for its **own allocated**
 *   `cdev_file_t` (holding the session pointer), and `close` frees it. So "one session per
 *   file" has a home without changing `br_file_t`'s layout or having the FS report a
 *   "private tail size".
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>

#include <br/dev/br_cdev.h>
#include <br/dev/br_dev.h>

#include "cdev_internal.h"     /* internal view for self-tests (see that header; not used in production) */

/* Lifecycle hook prototypes (name = symbol_prefix + phase; referenced by generated
 * `plugin_desc.c`; not in [[export]]) */
int cdev_core_early_init(void);
int cdev_core_init(void);
int cdev_core_start(void);

/* ==================================================================== static pools */

#define BR_CDEV_MAX    8u   /* cdev upper bound (prototype: static bound, "enough is enough" like BR_DEV_MAX) */
#define BR_FLASH_MAX   4u   /* flash subtype upper bound */

/*
 * One cdev registration record. The `dev_priv` given to dev-core is a **slot pointer**
 * (not the driver private itself) — because the adaptation layer must find both ops and
 * driver private, and the open_file hook receives only one `void *`.
 */
typedef struct cdev_slot {
    br_bool              used;
    const br_cdev_ops_t *ops;
    void                *dev_priv;
} cdev_slot_t;

typedef struct flash_slot {
    br_bool               used;
    char                  name[BR_DEV_NAME_MAX];   /* for br_flash_get reverse lookup (registry only holds entries) */
    const br_flash_ops_t *ops;
    br_flash_geom_t       geom;
    void                 *priv;
} flash_slot_t;

static cdev_slot_t  s_cdevs[BR_CDEV_MAX];
static br_u32       s_cdev_n;
static flash_slot_t s_flash[BR_FLASH_MAX];
static br_u32       s_flash_n;

/* ==================================================================== string helpers
 * No libc (-ffreestanding): three self-contained compare/copy lines. We do not borrow
 * symbols from core/string.c across units — the plugin's near-zero export surface (CA-10)
 * and the "who depends on whom" declaration surface should not be muddied by a string
 * compare. */

static br_bool cdev_str_eq(const char *a, const char *b)
{
    if (a == BR_NULL || b == BR_NULL) {
        return BR_FALSE;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

static void cdev_str_copy(char *dst, const char *src, br_size_t cap)
{
    br_size_t i = 0u;
    if (cap == 0u) {
        return;
    }
    for (; src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ==================================================================== adaptation layer
 *
 * `br_file_ops` slot semantics (see br_vfs.h): an unsupported slot = BR_NULL ⇒ the caller
 * gets -ENOTSUP. This adaptation layer **deliberately** leaves three slots empty:
 *   - lseek: character devices have no offset semantics (design `7-03` §3 says
 *     "lseek/fsync slots NULL → -ENOTSUP");
 *   - fsync: character devices have no "flush to media";
 *   - poll_attach: the v2 wait-queue slot (reserved by D22/D14 to avoid a layout break).
 */
typedef struct cdev_file {
    const cdev_slot_t *slot;
    void              *sess;   /* driver session (produced by ops->open) */
} cdev_file_t;

static int cf_open(br_file_t *f, br_u32 flags)
{
    /* inode-carried fpriv = this unit's device slot (see cdev_open_file) */
    const cdev_slot_t *slot = (const cdev_slot_t *)br_file_fpriv(f);
    if (slot == BR_NULL || slot->used == BR_FALSE || slot->ops == BR_NULL) {
        return BR_ERR(ENODEV);
    }

    cdev_file_t *cf = (cdev_file_t *)br_malloc((br_size_t)sizeof(cdev_file_t));
    if (cf == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    cf->slot = slot;
    cf->sess = BR_NULL;

    const int rc = slot->ops->open(slot->dev_priv, flags, &cf->sess);
    if (rc != BR_OK) {
        br_free(cf);
        return rc;      /* pass through as-is: an exclusive device returns -EBUSY here (policy belongs to the driver, design `7-01` §1) */
    }

    br_file_set_fpriv(f, cf);   /* ★ overwrite: from "device slot" to "per-file session" */
    return BR_OK;
}

static int cf_close(br_file_t *f)
{
    cdev_file_t *cf = (cdev_file_t *)br_file_fpriv(f);
    if (cf == BR_NULL) {
        return BR_OK;   /* idempotent: nothing to close if no session was created */
    }
    int rc = BR_OK;
    if (cf->slot->ops->close != BR_NULL) {
        rc = cf->slot->ops->close(cf->sess);
    }
    br_free(cf);
    br_file_set_fpriv(f, BR_NULL);
    return rc;
}

/* Session view (shared by the three forwarding slots): no session ⇒ BR_NULL */
static const cdev_file_t *cf_of(br_file_t *f)
{
    const cdev_file_t *cf = (const cdev_file_t *)br_file_fpriv(f);
    if (cf == BR_NULL || cf->slot == BR_NULL || cf->slot->ops == BR_NULL) {
        return BR_NULL;
    }
    return cf;
}

static br_s64 cf_read(br_file_t *f, void *buf, br_size_t n)
{
    const cdev_file_t *cf = cf_of(f);
    if (cf == BR_NULL || cf->slot->ops->read == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return cf->slot->ops->read(cf->sess, buf, n);
}

static br_s64 cf_write(br_file_t *f, const void *buf, br_size_t n)
{
    const cdev_file_t *cf = cf_of(f);
    if (cf == BR_NULL || cf->slot->ops->write == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return cf->slot->ops->write(cf->sess, buf, n);
}

static int cf_ioctl(br_file_t *f, br_u32 cmd, void *arg)
{
    const cdev_file_t *cf = cf_of(f);
    if (cf == BR_NULL || cf->slot->ops->ioctl == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return cf->slot->ops->ioctl(cf->sess, cmd, arg);
}

static int cf_poll(br_file_t *f, br_u32 *events)
{
    const cdev_file_t *cf = cf_of(f);
    if (cf == BR_NULL || cf->slot->ops->poll == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return cf->slot->ops->poll(cf->sess, events);
}

/* Slots use **designated initializers** one-to-one (positional init hides "missed one /
 * moved one" at the assembly level). */
static const br_file_ops_t s_cdev_file_ops = {
    .open        = cf_open,
    .read        = cf_read,
    .write       = cf_write,
    .lseek       = BR_NULL,
    .ioctl       = cf_ioctl,
    .fsync       = BR_NULL,
    .poll        = cf_poll,
    .poll_attach = BR_NULL,
    .close       = cf_close,
};

/* open_file hook (called by dev-core on devfs device-node lookup): returns {fops, fpriv}.
 * `fpriv` = device slot; the adaptation layer's `open` swaps it for the per-file session. */
static int cdev_open_file(void *dev_priv, const br_file_ops_t **fops, void **fpriv)
{
    if (dev_priv == BR_NULL || fops == BR_NULL || fpriv == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *fops  = &s_cdev_file_ops;
    *fpriv = dev_priv;
    return BR_OK;
}

/* ==================================================================== cdev register/retrieve */

int br_cdev_register(const char *name, const br_cdev_ops_t *ops, void *dev_priv)
{
    if (ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /* open/close are **mandatory** (design `8-01` §3: the two endpoints of the session
     * model); the other slots may be NULL. */
    if (ops->open == BR_NULL || ops->close == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (s_cdev_n >= (br_u32)BR_CDEV_MAX) {
        return BR_ERR(ENOSPC);
    }

    cdev_slot_t *slot = &s_cdevs[s_cdev_n];
    slot->used     = BR_TRUE;
    slot->ops      = ops;
    slot->dev_priv = dev_priv;

    const br_dev_class_entry_t e = {
        .class_id   = (br_u16)BR_CLASS_CDEV,
        .class_priv = (void *)ops,      /* shape interpretation belongs here; dev-core only passes through */
        .open_file  = cdev_open_file,
        .dev_priv   = slot,             /* ★ the hook gets the slot, not the driver private */
    };

    const int rc = br_dev_add(name, &e);
    if (rc != BR_OK) {
        slot->used = BR_FALSE;          /* rollback: a failure leaves no "half-registered" slot */
        return rc;                      /* -EINVAL / -EEXIST / -ENOSPC passed through */
    }

    s_cdev_n++;
    return BR_OK;
}

const br_cdev_ops_t *br_cdev_get(const char *name, void **dev_priv)
{
    const br_dev_class_entry_t *e = br_dev_lookup(name);
    if (e == BR_NULL || e->class_id != (br_u16)BR_CLASS_CDEV) {
        return BR_NULL;
    }
    /* The same class_id also holds the flash subtype: distinguish by "does the slot pointer
     * fall in this unit's cdev pool".
     * Do not compare class_priv (that cannot rule out the theoretical case of two drivers'
     * ops at the same address). */
    const cdev_slot_t *slot = (const cdev_slot_t *)e->dev_priv;
    br_bool in_pool = BR_FALSE;
    for (br_u32 i = 0u; i < s_cdev_n; i++) {
        if (&s_cdevs[i] == slot && s_cdevs[i].used == BR_TRUE) {
            in_pool = BR_TRUE;
            break;
        }
    }
    if (in_pool == BR_FALSE) {
        return BR_NULL;
    }
    if (dev_priv != BR_NULL) {
        *dev_priv = slot->dev_priv;
    }
    return slot->ops;
}

/* ==================================================================== flash subtype */
/*
 * flash is a **subtype** of the cdev subclass (design SD-2), so `class_id` is likewise
 * BR_CLASS_CDEV; shape interpretation still belongs here (`br_flash_ops` and `br_cdev_ops`
 * are two shapes).
 * ★ This unit does **not** install the open_file hook (BR_NULL): the file face of raw flash
 *   is not a v1 goal (design `7-03` §3: "the bdev raw block access hook = v2") ⇒ `/dev/nor0`
 *   appears but `br_open` gets -ENOTSUP. Visible but unopenable is more honest than
 *   "invisible" (ADR-0009 §3 ruling 6).
 */
static flash_slot_t *flash_find(const char *name)
{
    for (br_u32 i = 0u; i < s_flash_n; i++) {
        if (s_flash[i].used == BR_TRUE && cdev_str_eq(s_flash[i].name, name) == BR_TRUE) {
            return &s_flash[i];
        }
    }
    return BR_NULL;
}

int br_flash_register(const char *name, const br_flash_ops_t *ops,
                      const br_flash_geom_t *geom, void *priv)
{
    if (ops == BR_NULL || geom == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /* Geometry must be self-consistent: zero read/program granularity or zero blocks
     * ⇒ no FS can compute addresses. Reject at registration so littlefs never gets a
     * table that "looks computable but divides by zero". */
    if (geom->read_size == 0u || geom->prog_size == 0u ||
        geom->block_size == 0u || geom->block_count == 0u) {
        return BR_ERR(EINVAL);
    }
    if (ops->read == BR_NULL || ops->program == BR_NULL || ops->erase == BR_NULL) {
        return BR_ERR(EINVAL);   /* the three mandatory flash subtype actions (design `8-01` §3) */
    }
    if (flash_find(name) != BR_NULL) {
        return BR_ERR(EEXIST);
    }
    if (s_flash_n >= (br_u32)BR_FLASH_MAX) {
        return BR_ERR(ENOSPC);
    }

    flash_slot_t *slot = &s_flash[s_flash_n];
    slot->used = BR_TRUE;
    slot->ops  = ops;
    slot->geom = *geom;
    slot->priv = priv;
    cdev_str_copy(slot->name, name, (br_size_t)BR_DEV_NAME_MAX);

    const br_dev_class_entry_t e = {
        .class_id   = (br_u16)BR_CLASS_CDEV,
        .class_priv = (void *)ops,
        .open_file  = BR_NULL,      /* v1: raw flash has no file face (v2's bdev/raw hook) */
        .dev_priv   = slot,
    };
    const int rc = br_dev_add(name, &e);
    if (rc != BR_OK) {
        slot->used = BR_FALSE;
        return rc;
    }

    s_flash_n++;
    return BR_OK;
}

const br_flash_ops_t *br_flash_get(const char *name, void **priv)
{
    flash_slot_t *slot = flash_find(name);
    if (slot == BR_NULL) {
        return BR_NULL;
    }
    if (priv != BR_NULL) {
        *priv = slot->priv;
    }
    return slot->ops;
}

int br_flash_geom_get(const char *name, br_flash_geom_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    flash_slot_t *slot = flash_find(name);
    if (slot == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    *out = slot->geom;
    return BR_OK;
}

/* ==================================================================== lifecycle */

/* EARLY: no action (static pools are BSS; no heap or threads yet). Device registration
 * happens in each driver's CORE init. */
int cdev_core_early_init(void)
{
    return 0;
}

/* CORE: no action. cdev-core has no state of its own to initialize — it "enters the stage"
 * when a driver calls `br_cdev_register`.
 * Deliberately no empty API (R-S4: every line inside a framework unit needs a consumer). */
int cdev_core_init(void)
{
    return 0;
}

/* START phase: this unit's consistency cases run **not here** — after ADR-0010 they are
 * driven uniformly by core's `br_plugin_manager_selftest()` via the descriptor's
 * `.selftest` hook (see src/cdev_selftest.c).
 * Only a one-line registry summary is left here as boot evidence. */
int cdev_core_start(void)
{
    br_log_info("cdev: %u device(s) in registry", br_dev_count());
    return 0;
}

/* ==================================================================== internal view for self-tests
 *
 * See `src/cdev_internal.h`: these two accessors exist **only** for `cdev_selftest.c` —
 * the subject under test is this file's mechanism (the open_file hook it installs / the
 * `br_file_ops` table of the adaptation layer), which the public API cannot express.
 * ★ Always **forward** to the production implementation (return their addresses), never
 *   copy logic: cases must compare the same truth, otherwise "the hook installed at
 *   registration is the adaptation-layer entry" would be validated against a copy.
 */

cdev_open_file_hook_t cdev_internal_open_file_hook(void)
{
    return cdev_open_file;
}

const br_file_ops_t *cdev_internal_file_ops(void)
{
    return &s_cdev_file_ops;
}
