/*
 * brickOS prototype v0.2.0 — service registry (core common)
 *
 * Design basis: `3-01` §9 (`br_service_publish` / `br_service_lookup`), `3-06` (service
 *           management), `1-01` §9 (publish = provider init; lookup = consumer init ⇒ phase
 *           order + init topology order together guarantee it is found).
 *
 * Semantics (verbatim from the design, nothing added or removed):
 *   - core only manages **name → pointer**, it does not interpret the ops type;
 *   - duplicate name ⇒ `-EEXIST` (no overwrite — overwriting would make "who is the real one"
 *     depend on boot order);
 *   - table full ⇒ `-ENOSPC`; empty `name`/empty `ops` ⇒ `-EINVAL`;
 *   - not found ⇒ `BR_NULL` (the designed signature returns a pointer).
 *
 * Single-threaded premise (design `3-06` §2 item 1): **publish/lookup both happen in the init
 * phase** (EARLY/CORE/LATE), when interrupts are globally off and there are no threads ⇒ the
 * registry needs no lock. Publishing after the START phase is the caller's fault; this file
 * neither detects it (nor pretends to be thread-safe).
 *
 * Deviations from the design (recorded in `docs/decisions/0005-plugin-manager.md` §2.7):
 *   - three added **observation** entry points (`br_service_count` / `br_service_name_at` /
 *     `br_service_lookup_hits`, see `br_svc.h`), for dump-style rendering and consistency
 *     cases reading the table;
 *   - a white-box removal entry `svc_remove` (declared in `core/src/svc/svc_internal.h`): the
 *     design has no unpublish, but core self-test (ADR-0010) needs temporary entries with the
 *     `selftest.` prefix and must **clean them up itself** — the case body has moved to
 *     `core/selftest/svc_selftest.c`.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_svc.h>
#include <br/core/br_types.h>

#include "svc_internal.h"

/* ==================================================================== table */

typedef struct br_svc_entry {
    const char *name;
    const void *ops;
    br_u32      hits;   /* lookup hits (accounting: "who uses whom" of 5-01 §4) */
} br_svc_entry_t;

static br_svc_entry_t s_svc[BR_SERVICE_MAX];
static br_u32 s_count;

/* Name equality (comparison for publish/lookup/hits; the self-test also uses it to recheck name_at's return — see svc_internal.h). */
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

/* ==================================================================== publish/lookup */

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

/* ==================================================================== observation */

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

/* White-box removal (the production surface has no unpublish; only the self-test uses it to clear its own temporary entries — see svc_internal.h). */
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

