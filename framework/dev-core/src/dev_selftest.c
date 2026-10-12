/*
 * brickOS prototype v0.2.0 — dev-core selftest suite (framework/dev-core/src/dev_selftest.c)
 *
 * Design basis and ruling: `docs/decisions/0010-plugin-selftest.md` (selftest separate from core code).
 *
 * ## Why selftest is not in `dev_core.c` (the reason this file exists)
 *   The test entry is not the plugin's public surface: if it stayed in `include/`, "add a case" would become a
 *   golden interface change, and consumers (other plugins) must never call it. Moved into a separate `src/dev_selftest.c`,
 *   `dev_core.c` keeps only the production path; the cases still live in the same plugin's compilation unit set (`[build].sources` covers
 *   every `.c` under `src/`).
 *
 * ## What it tests and where the production boundary is
 *   Device-domain registry conformance suite (own numbering `TC-DEV-*`): driven only through the public API
 *   of `br/dev/br_dev.h` (naming rules / registry / ioctl encoding). No case here tests an internal
 *   mechanism as the object under test, so no internal header is needed — compare vfs-core's `src/vfs_internal.h`:
 *   those accessors exist only for checks inexpressible via the public API, and there are none here.
 *
 * ## Contract with br_plugin_manager_selftest()
 *   Returns the failure count (0 = all green); logs are emitted here (`[DEVCONF] PASS/FAIL ...` + `SUMMARY`).
 *   Failure does not halt — the gate decides (`[DEVCONF] FAIL` is in tests/gates.toml forbid).
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

/* Selftest entry prototype (defined at the end of this file; referenced by the `.selftest` hook in the generated `plugin_desc.c`). */
int dev_core_selftest(void);

/* ==================================================================== Conformance cases */

static br_u32 s_pass;
static br_u32 s_fail;

static void pc(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[DEVCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[DEVCONF] FAIL %s %s", tag, what);
    }
}

int dev_core_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[DEVCONF] dev-core conformance (registry / naming / ioctl encoding)");

    /* ---- TC-DEV-001: naming rules (pure function: touches no registry, so counterexamples can be enumerated freely) ---- */
    {
        const br_bool ok =
            (br_dev_name_valid("uart0")   == BR_TRUE)  &&
            (br_dev_name_valid("a")       == BR_TRUE)  &&
            (br_dev_name_valid("nor0x")   == BR_TRUE)  &&
            (br_dev_name_valid(BR_NULL)   == BR_FALSE) &&
            (br_dev_name_valid("")        == BR_FALSE) &&
            (br_dev_name_valid("Uart0")   == BR_FALSE) &&   /* uppercase */
            (br_dev_name_valid("0uart")   == BR_FALSE) &&   /* leading digit */
            (br_dev_name_valid("u/art")   == BR_FALSE) &&   /* slash: name = path component */
            (br_dev_name_valid("u_art")   == BR_FALSE) &&   /* underscore */
            (br_dev_name_valid("uart0uart0uart0uart0uart0uart0uart0") == BR_FALSE); /* too long */
        pc(ok, "TC-DEV-001", "naming rule [a-z][a-z0-9]* / no slash / length bound");
    }

    /* ---- TC-DEV-002: invalid registration rejected without polluting the registry ----
     * Why only invalid input and no test device: dev-core has no unregister API (v1 has no hotplug —
     * design SD-4/O-S3), so once registered it appears in /dev forever; a case must not toss
     * garbage into the observed namespace. The positive registration path is covered by real drivers
     * (io/uart-pl011 registers uart0 in its CORE init); this case only confirms bad input is refused and the count is unchanged. */
    {
        const br_u32 before = br_dev_count();
        br_dev_class_entry_t e = { 0u, BR_NULL, BR_NULL, BR_NULL };

        const int r1 = br_dev_add("Bad", &e);          /* invalid name (uppercase) */
        e.class_id = BR_CLASS_NONE;
        const int r2 = br_dev_add("testdev", &e);      /* class_id = NONE: no shape interpreter */
        const int r3 = br_dev_add("testdev", BR_NULL); /* NULL entry */
        const int r4 = br_dev_add(BR_NULL, BR_NULL);   /* both NULL */

        const br_bool ok = (r1 == BR_ERR(EINVAL)) && (r2 == BR_ERR(EINVAL)) &&
                           (r3 == BR_ERR(EINVAL)) && (r4 == BR_ERR(EINVAL)) &&
                           (br_dev_count() == before) &&
                           (br_dev_lookup("testdev") == BR_NULL);
        pc(ok, "TC-DEV-002", "invalid registration rejected (-EINVAL) and registry count unchanged");
    }

    /* ---- TC-DEV-003: lookup and missing (missing must return BR_NULL, not an error code) ---- */
    {
        const br_bool ok = (br_dev_lookup("nosuchdev") == BR_NULL) &&
                           (br_dev_lookup(BR_NULL) == BR_NULL) &&
                           (br_dev_name_at(br_dev_count()) == BR_NULL) &&
                           (br_dev_entry_at(br_dev_count()) == BR_NULL);
        pc(ok, "TC-DEV-003", "unregistered/out-of-range => BR_NULL");
    }

    /* ---- TC-DEV-004: enumeration and lookup share one source (name_at <-> lookup round trip per entry) ---- */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const char *n = br_dev_name_at(i);
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            if (n == BR_NULL || e == BR_NULL) {
                ok = BR_FALSE;
                break;
            }
            if (br_dev_name_valid(n) == BR_FALSE) {
                ok = BR_FALSE;
                break;
            }
            if (br_dev_lookup(n) != e) {
                ok = BR_FALSE;
                break;
            }
        }
        pc(ok, "TC-DEV-004", "enumerated entry: valid name and lookup(name) == entry_at(i)");
    }

    /* ---- TC-DEV-005: duplicate name => -EEXIST (uses an existing name from the table, adds no entry) ---- */
    {
        br_bool ok = BR_TRUE;
        const br_u32 n = br_dev_count();
        if (n > 0u) {
            const char *first = br_dev_name_at(0u);
            const br_dev_class_entry_t *e = br_dev_entry_at(0u);
            ok = (first != BR_NULL) && (e != BR_NULL) &&
                 (br_dev_add(first, e) == BR_ERR(EEXIST));
        } else {
            /* Empty table: no existing name available => this case is not applicable. Honestly report PASS but state
             * the precondition in the description, so log readers do not think duplicate names were tested. */
            br_log_info("[DEVCONF] note: registry empty => TC-DEV-005 only exercised the not-applicable branch");
        }
        pc(ok, "TC-DEV-005", "duplicate name => -EEXIST (uniqueness enforcement)");
    }

    /* ---- TC-DEV-006: ioctl encoding (matches Linux _IOC bit layout; compile-time only) ---- */
    {
        typedef struct { br_u32 a; br_u32 b; } ioc_payload_t;
        const br_u32 cmd = BR_IOWR('u', 7u, ioc_payload_t);
        const br_bool ok = (BR_IOC_DIR(cmd)  == (BR_IOC_READ | BR_IOC_WRITE)) &&
                           (BR_IOC_TYPE(cmd) == (br_u32)'u') &&
                           (BR_IOC_NR(cmd)   == 7u) &&
                           (BR_IOC_SIZE(cmd) == (br_u32)sizeof(ioc_payload_t));
        pc(ok, "TC-DEV-006", "ioctl encoding dir[31:30]/size[29:16]/type[15:8]/nr[7:0]");
    }

    br_log_info("[DEVCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
