/*
 * brickOS prototype v0.2.0: generic device registry (framework/dev-core). Owns one static flat
 * namespace: name-rule enforcement, verbatim br_dev_class_entry_t storage (incl. the open_file
 * hook), lookup/enumeration. Knows nothing about cdev/bdev/flash and calls no subclass ops.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

/* Lifecycle hooks (referenced by the generated plugin_desc.c; not in [[export]]). */
int dev_core_early_init(void);
int dev_core_init(void);
int dev_core_start(void);

/* ---- registry ---- */

/* Static table (prototype: no dynamic allocation). `name` is the registry key stored beside the
 * entry, matching br_dev_add's parameter split. */
typedef struct dev_slot {
    br_bool              used;
    char                 name[BR_DEV_NAME_MAX];
    br_dev_class_entry_t entry;
} dev_slot_t;

static dev_slot_t s_devs[BR_DEV_MAX];
static br_u32     s_count;   /* entries; first s_count are valid (no unregister in v1) */

/* ---- name rules ---- */

br_bool br_dev_name_valid(const char *name)
{
    if (name == BR_NULL) {
        return BR_FALSE;
    }

    br_u32 i = 0u;
    for (; name[i] != '\0'; i++) {
        const char c = name[i];
        if (i == 0u) {
            /* First char must be [a-z]: a leading digit would read as a numeric suffix rather
             * than a device name. */
            if (c < 'a' || c > 'z') {
                return BR_FALSE;
            }
            continue;
        }
        const br_bool digit = (c >= '0' && c <= '9');
        const br_bool lower = (c >= 'a' && c <= 'z');
        if (!digit && !lower) {
            return BR_FALSE;   /* uppercase/'_'/'-'/'/' forbidden (no slash: path component) */
        }
    }

    /* At least 1 char, and room for the trailing '\0' (else the devfs-side copy truncates). */
    return (i > 0u && i < (br_u32)BR_DEV_NAME_MAX) ? BR_TRUE : BR_FALSE;
}

/* ---- lookup and registration ---- */

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
    /* class_id NONE has no shape interpreter => nobody could open it, so reject it here. */
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
    s->entry = *entry;                 /* stored by value: caller's table stays reusable */

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

/* ---- lifecycle ---- */

/* EARLY: no-op; statics are BSS, and registration waits for the CORE phase (heap ready). */
int dev_core_early_init(void)
{
    return 0;
}

/* CORE: no-op; BSS is the initial state. No empty init API for its own sake. */
int dev_core_init(void)
{
    return 0;
}

/* START: conformance cases run via br_plugin_manager_selftest() (see src/dev_selftest.c); only
 * a registry summary is logged here as boot evidence. */
int dev_core_start(void)
{
    br_log_info("dev: %u device(s) registered", br_dev_count());
    return 0;
}
