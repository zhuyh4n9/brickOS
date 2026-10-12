/*
 * brickOS prototype v0.2.0 — generic device registry (framework/dev-core)
 *
 * Design basis: `docs/8-device/8-01-device.md` §1.1/§1.3 (layering and framework ownership), §2 (registry
 * and sub-class registration protocol), §3 (shape belongs to sub-classes, dev-core **defines no ops shape**),
 * §4 (error model = negative errno), §5 (ioctl encoding). Case-by-case rulings in `docs/decisions/0009-vfs-storage-stack.md`.
 *
 * ## Everything this file does (three things only, R-S4's line of defense)
 *   ① one static registry (single flat namespace) + name-rule enforcement;
 *   ② store `br_dev_class_entry_t` (incl. the open_file hook) **verbatim**, retrieve by name/index;
 *   ③ ioctl encoding macros (in the header, purely compile-time).
 * It does not know what cdev/bdev/flash are, and calls no sub-class ops — shape and semantics live in the sub-class frameworks.
 *
 * ## Why the registry lives in a **plugin** rather than core
 *   Design D19/`1-01` §4.5: "capability frameworks become plugins" — a product that does not choose the device framework
 *   must not pay RAM/code for it; the discipline (API face into golden/gates) is the same as core. So the `br_*`
 *   prefix here is governance discipline, not core substance.
 *
 * ## Why duplicate names are also enforced at **runtime**
 *   A device name is a string the driver assembles at runtime (may contain a SoC instance number), and the declaration face
 *   of `brickie check` cannot see it — same shape as `br_service_publish`'s `-EEXIST`: one runtime self-check is very cheap,
 *   while "two drivers fighting over the same /dev/uart0" is the kind of error that **silently** disables one side.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

/* Lifecycle hook prototypes (name = symbol_prefix + phase; referenced by the generated `plugin_desc.c`;
 * hooks are a composition-time contract and **not** in [[export]] ⇒ not part of the interface hash, hence not declared in the public header). */
int dev_core_early_init(void);
int dev_core_init(void);
int dev_core_start(void);

/* ==================================================================== registry */

/*
 * Static table (prototype approach: no dynamic allocation — same technique as the IRQ/PIC pools and the TCB pool).
 * `name` and `entry` are stored separately: the name is the **registry key** (design `8-01` §2 puts name in
 * `br_dev_add`'s parameters rather than in the struct), so type dependencies need not make entry carry string lifetime.
 */
typedef struct dev_slot {
    br_bool              used;
    char                 name[BR_DEV_NAME_MAX];
    br_dev_class_entry_t entry;
} dev_slot_t;

static dev_slot_t s_devs[BR_DEV_MAX];
static br_u32     s_count;   /* entry count (compact: no holes on removal — v1 has no unregister, but keep the "first count entries valid" invariant) */

/* ==================================================================== name rules */

br_bool br_dev_name_valid(const char *name)
{
    if (name == BR_NULL) {
        return BR_FALSE;
    }

    br_u32 i = 0u;
    for (; name[i] != '\0'; i++) {
        const char c = name[i];
        if (i == 0u) {
            /* First char must be a lowercase letter (design `8-01` §2: `[a-z][a-z0-9]*`).
             * Why a leading digit is forbidden: devfs node names are path components, and `7-01` §1's naming rule
             * states a `[a-z]` start — a leading digit would confuse a "device name" with a "possible numeric suffix". */
            if (c < 'a' || c > 'z') {
                return BR_FALSE;
            }
            continue;
        }
        const br_bool digit = (c >= '0' && c <= '9');
        const br_bool lower = (c >= 'a' && c <= 'z');
        if (!digit && !lower) {
            return BR_FALSE;   /* uppercase / '_' / '-' / '/' are all forbidden (no slash: name = path component) */
        }
    }

    /* Length: at least 1 char, and room must be left for the trailing '\0' (otherwise the devfs-side copy truncates). */
    return (i > 0u && i < (br_u32)BR_DEV_NAME_MAX) ? BR_TRUE : BR_FALSE;
}

/* ==================================================================== lookup and registration */

static br_u32 find_index(const char *name)
{
    if (name == BR_NULL) {
        return (br_u32)-1;
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (s_devs[i].used == BR_FALSE) {
            continue;
        }
        const char *a = s_devs[i].name;
        const char *b = name;
        while (*a != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return (br_u32)-1;
}

int br_dev_add(const char *name, const br_dev_class_entry_t *entry)
{
    if (entry == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (br_dev_name_valid(name) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    /* An entry with class_id = NONE has no shape interpreter ⇒ nobody can open it, so it is the caller's error.
     * Explicit rejection is more honest than "store it but it can never be opened". */
    if (entry->class_id == BR_CLASS_NONE) {
        return BR_ERR(EINVAL);
    }
    if (find_index(name) != (br_u32)-1) {
        return BR_ERR(EEXIST);
    }
    if (s_count >= (br_u32)BR_DEV_MAX) {
        return BR_ERR(ENOSPC);
    }

    dev_slot_t *s = &s_devs[s_count];
    s->used  = BR_TRUE;
    s->entry = *entry;                 /* struct stored by value: the caller's (sub-class framework's) static table stays reusable */

    br_u32 i = 0u;
    for (; name[i] != '\0' && i + 1u < (br_u32)BR_DEV_NAME_MAX; i++) {
        s->name[i] = name[i];
    }
    s->name[i] = '\0';

    s_count++;
    return BR_OK;
}

const br_dev_class_entry_t *br_dev_lookup(const char *name)
{
    const br_u32 i = find_index(name);
    return (i == (br_u32)-1) ? BR_NULL : &s_devs[i].entry;
}

br_u32 br_dev_count(void)
{
    return s_count;
}

const char *br_dev_name_at(br_u32 index)
{
    if (index >= s_count || s_devs[index].used == BR_FALSE) {
        return BR_NULL;
    }
    return s_devs[index].name;
}

const br_dev_class_entry_t *br_dev_entry_at(br_u32 index)
{
    if (index >= s_count || s_devs[index].used == BR_FALSE) {
        return BR_NULL;
    }
    return &s_devs[index].entry;
}

/* ==================================================================== lifecycle */

/* EARLY phase: no-op. The registry's statics are BSS (already zeroed by start.S), and there are no threads/heap yet —
 * device registration waits for the **CORE phase** (sub-class framework init), when the heap is ready (br_malloc usable for driver-private data). */
int dev_core_early_init(void)
{
    return 0;
}

/* CORE phase: no-op. dev-core has no state needing "init" (BSS is the initial value) —
 * deliberately does **not** invent an empty init API (R-S4: framework code must not carry anything with no consumer). */
int dev_core_init(void)
{
    return 0;
}

/* START phase: this component's conformance cases **do not run here** — after ADR-0010 they are driven uniformly by
 * core's `br_plugin_manager_selftest()` through the descriptor's `.selftest` hook (see src/dev_selftest.c).
 * Only one registry summary line is kept here as boot evidence (same shape as vfs-core's start: start only reports state). */
int dev_core_start(void)
{
    br_log_info("dev: %u device(s) registered", br_dev_count());
    return 0;
}
