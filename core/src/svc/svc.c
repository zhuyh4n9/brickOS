/*
 * Service registry (core common): name -> pointer only, the ops type is not interpreted.
 * Duplicate name => -EEXIST (never overwrite, so lookup is boot-order independent); table full =>
 * -ENOSPC; empty name/ops => -EINVAL; lookup miss => BR_NULL.
 * Init-phase only (IRQs off, no threads), so no lock; publishing after START is a caller bug.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_svc.h>
#include <br/core/br_types.h>

#include "svc_internal.h"

/* ---- table ---- */

typedef struct br_svc_entry {
    const char *name;
    const void *ops;
    br_u32      hits;   /* lookup hits */
} br_svc_entry_t;

static br_svc_entry_t s_svc[BR_SERVICE_MAX];
static br_u32 s_count;

/* Name equality for publish/lookup/hits; the selftest also rechecks name_at with it. */
br_bool name_equal(const char *a, const char *b)
{
    if (a == BR_NULL || b == BR_NULL) {
        return BR_FALSE;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0') && (*b == '\0');
}

/* ---- publish/lookup ---- */

int br_service_publish(const char *name, const void *ops)
{
    if (name == BR_NULL || ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (name_equal(s_svc[i].name, name)) {
            return BR_ERR(EEXIST);   /* no overwrite */
        }
    }
    if (s_count >= BR_SERVICE_MAX) {
        return BR_ERR(ENOSPC);
    }
    s_svc[s_count].name = name;
    s_svc[s_count].ops  = ops;
    s_svc[s_count].hits = 0u;
    s_count++;
    return 0;
}

const void *br_service_lookup(const char *name)
{
    if (name == BR_NULL) {
        return BR_NULL;
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (name_equal(s_svc[i].name, name)) {
            s_svc[i].hits++;
            return s_svc[i].ops;
        }
    }
    return BR_NULL;
}

/* ---- observation ---- */

br_u32 br_service_count(void)
{
    return s_count;
}

const char *br_service_name_at(br_u32 index)
{
    if (index >= s_count) {
        return BR_NULL;
    }
    return s_svc[index].name;
}

br_u32 br_service_lookup_hits(const char *name)
{
    if (name == BR_NULL) {
        return 0u;
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (name_equal(s_svc[i].name, name)) {
            return s_svc[i].hits;
        }
    }
    return 0u;
}

/* White-box removal; production has no unpublish, used by the selftest to clear its entries. */
void svc_remove(const char *name)
{
    if (name == BR_NULL) {
        return;
    }
    for (br_u32 i = 0u; i < s_count; i++) {
        if (!name_equal(s_svc[i].name, name)) {
            continue;
        }
        for (br_u32 j = i + 1u; j < s_count; j++) {
            s_svc[j - 1u] = s_svc[j];
        }
        s_count--;
        s_svc[s_count].name = BR_NULL;
        s_svc[s_count].ops  = BR_NULL;
        s_svc[s_count].hits = 0u;
        return;
    }
}
