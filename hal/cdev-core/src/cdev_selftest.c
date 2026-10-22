/*
 * brickOS prototype v0.2.0: cdev-core selftest suite (TC-CDEV-*). Cases go through the public
 * br_cdev.h/br_dev.h API except TC-CDEV-004/005 (cdev entries recognized by open_file pointer
 * identity) and TC-CDEV-006 (adapter slot shape), which read src/cdev_internal.h. Returns the
 * failure count; logs [CDEVCONF] PASS/FAIL and SUMMARY.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>

#include <br/dev/br_cdev.h>
#include <br/dev/br_dev.h>

#include "cdev_internal.h"

/* Selftest entry (referenced by the generated plugin_desc.c `.selftest` hook). */
int cdev_core_selftest(void);

/* ---- conformance cases ---- */

/* Minimal ops for the cases only, exercising "illegal name / missing required slot". They are
 * never registered, so /dev is untouched. */
static int conf_open(void *dev_priv, br_u32 flags, void **sess)
{
    (void)dev_priv; (void)flags;
    if (sess != BR_NULL) {
        *sess = BR_NULL;
    }
    return BR_OK;
}

static int conf_close(void *sess)
{
    (void)sess;
    return BR_OK;
}

static br_u32 s_pass;
static br_u32 s_fail;

static void pc(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[CDEVCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[CDEVCONF] FAIL %s %s", tag, what);
    }
}

int cdev_core_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[CDEVCONF] cdev-core conformance (registration preconditions / session adapter / flash subtype)");

    br_cdev_ops_t good = { 0 };
    good.open  = conf_open;
    good.close = conf_close;

    /* ---- TC-CDEV-001: illegal name => -EINVAL and no registry pollution. Only invalid input
     * is tested: v1 has no unregister, so the positive path is left to real drivers. ---- */
    {
        const br_u32 before = br_dev_count();
        const int r_upper = br_cdev_register("Cdev0", &good, BR_NULL);   /* leading uppercase */
        const int r_slash = br_cdev_register("c/dev", &good, BR_NULL);   /* contains slash */
        const int r_empty = br_cdev_register("", &good, BR_NULL);        /* empty name */
        const br_bool ok = (r_upper == BR_ERR(EINVAL)) &&
                           (r_slash == BR_ERR(EINVAL)) &&
                           (r_empty == BR_ERR(EINVAL)) &&
                           (br_dev_count() == before);
        pc(ok, "TC-CDEV-001", "illegal name => -EINVAL and registry count unchanged");
    }

    /* ---- TC-CDEV-002: open/close missing or ops NULL => -EINVAL ---- */
    {
        br_cdev_ops_t only_open = { 0 };
        only_open.open = conf_open;                    /* close missing */
        br_cdev_ops_t only_close = { 0 };
        only_close.close = conf_close;                 /* open missing */

        const int r1 = br_cdev_register("cdevx", &only_open,  BR_NULL);
        const int r2 = br_cdev_register("cdevx", &only_close, BR_NULL);
        const int r3 = br_cdev_register("cdevx", BR_NULL,     BR_NULL);
        const br_bool ok = (r1 == BR_ERR(EINVAL)) &&
                           (r2 == BR_ERR(EINVAL)) &&
                           (r3 == BR_ERR(EINVAL));
        pc(ok, "TC-CDEV-002", "ops missing open/close or ops NULL => -EINVAL");
    }

    /* ---- TC-CDEV-003: unregistered/empty name => BR_NULL and out-param untouched ---- */
    {
        void *priv = (void *)(br_uintptr_t)1u;
        const br_bool ok = (br_cdev_get("nosuchcdev", &priv) == BR_NULL) &&
                           (br_cdev_get(BR_NULL, BR_NULL) == BR_NULL) &&
                           (priv == (void *)(br_uintptr_t)1u);
        pc(ok, "TC-CDEV-003", "unregistered/empty name => BR_NULL and out-param untouched");
    }

    /* ---- TC-CDEV-004: br_cdev_get round trip over registered cdev entries ---- */
    {
        /* open_file hook this file installs: identifies "own entries" by pointer identity */
        const cdev_open_file_hook_t own_hook = cdev_internal_open_file_hook();
        br_bool ok = BR_TRUE;
        br_u32  checked = 0u;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            const char *n = br_dev_name_at(i);
            if (e == BR_NULL || n == BR_NULL) {
                continue;
            }
            if (e->class_id != (br_u16)BR_CLASS_CDEV || e->open_file != own_hook) {
                continue;   /* flash subtype / other subcategory is not this case's business */
            }
            void *priv = BR_NULL;
            const br_cdev_ops_t *got = br_cdev_get(n, &priv);
            ok = ok && (got != BR_NULL) && (got == (const br_cdev_ops_t *)e->class_priv);
            checked++;
        }
        if (checked == 0u) {
            br_log_info("[CDEVCONF] note: no cdev device in this build => TC-CDEV-004 only took the 'not applicable' branch");
        }
        pc(ok, "TC-CDEV-004", "br_cdev_get(name) == entry's registered ops (per-cdev round-trip)");
    }

    /* ---- TC-CDEV-005: duplicate name => -EEXIST (existing name from the table) ---- */
    {
        const cdev_open_file_hook_t own_hook = cdev_internal_open_file_hook();
        const char *first = BR_NULL;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_CDEV &&
                e->open_file == own_hook) {
                first = br_dev_name_at(i);
                break;
            }
        }
        br_bool ok = BR_TRUE;
        if (first != BR_NULL) {
            void *priv = BR_NULL;
            const br_cdev_ops_t *ops = br_cdev_get(first, &priv);
            ok = (ops != BR_NULL) && (br_cdev_register(first, ops, priv) == BR_ERR(EEXIST));
        } else {
            br_log_info("[CDEVCONF] note: no cdev device => TC-CDEV-005 only took the 'not applicable' branch");
        }
        pc(ok, "TC-CDEV-005", "duplicate name => -EEXIST (uniqueness enforced by dev-core, passed through here)");
    }

    /* ---- TC-CDEV-006: adapter slot shape (lseek/fsync/poll_attach null => -ENOTSUP) ---- */
    {
        const br_file_ops_t *fops = cdev_internal_file_ops();
        const br_bool ok = (fops->open != BR_NULL) &&
                           (fops->read != BR_NULL) &&
                           (fops->write != BR_NULL) &&
                           (fops->ioctl != BR_NULL) &&
                           (fops->poll != BR_NULL) &&
                           (fops->close != BR_NULL) &&
                           (fops->lseek == BR_NULL) &&
                           (fops->fsync == BR_NULL) &&
                           (fops->poll_attach == BR_NULL);
        pc(ok, "TC-CDEV-006", "adapter forwarding slots complete; lseek/fsync/poll_attach null (= -ENOTSUP)");
    }

    /* ---- TC-CDEV-007: flash subtype preconditions and lookup ---- */
    {
        br_flash_ops_t fops = { 0 };
        br_flash_geom_t geom = { 0 };

        const int r_null     = br_flash_register("nor0", BR_NULL, &geom, BR_NULL);
        const int r_zerogeom = br_flash_register("nor0", &fops, &geom, BR_NULL);   /* all-zero geometry */
        geom.read_size = 1u; geom.prog_size = 1u;
        geom.block_size = 4096u; geom.block_count = 64u;
        const int r_noacts = br_flash_register("nor0", &fops, &geom, BR_NULL);     /* read/program/erase missing */

        br_flash_geom_t got;
        const int r_geom_missing = br_flash_geom_get("nor0", &got);
        const int r_geom_null    = br_flash_geom_get("nor0", BR_NULL);

        const br_bool ok = (r_null == BR_ERR(EINVAL)) &&
                           (r_zerogeom == BR_ERR(EINVAL)) &&
                           (r_noacts == BR_ERR(EINVAL)) &&
                           (r_geom_missing == BR_ERR(ENODEV)) &&
                           (r_geom_null == BR_ERR(EINVAL)) &&
                           (br_flash_get("nor0", BR_NULL) == BR_NULL);
        pc(ok, "TC-CDEV-007", "flash: geometry self-consistency/required actions checked at registration; unregistered lookup => BR_NULL/-ENODEV");
    }

    /* ---- TC-CDEV-008: flash entries have open_file == BR_NULL (v1 raw flash has no file
     * surface); wiring one on turns this red, so the case and the doc must change. ---- */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            const char *n = br_dev_name_at(i);
            if (e != BR_NULL && br_flash_get(n, BR_NULL) != BR_NULL) {
                ok = ok && (e->open_file == BR_NULL);
            }
        }
        pc(ok, "TC-CDEV-008", "flash entries open_file == BR_NULL (v1 raw flash has no file surface)");
    }

    br_log_info("[CDEVCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
