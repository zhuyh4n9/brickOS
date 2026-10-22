/*
 * Service registry internal contract (core-private, not for plugins). The public surface cannot
 * remove entries (publishing is one-way), but the selftest suite must clear its temporary
 * `selftest.`-prefixed entries -- hence svc_remove. name_equal is the production comparison,
 * reused by the selftest to recheck what br_service_name_at() returned.
 */
#ifndef BR_SVC_INTERNAL_H
#define BR_SVC_INTERNAL_H

#include <br/core/br_types.h>

/* Name equality, the production comparison used by publish/lookup/lookup_hits. */
br_bool name_equal(const char *a, const char *b);

/* Remove one entry (name not in table => silent no-op); only selftest uses this. */
void svc_remove(const char *name);

#endif /* BR_SVC_INTERNAL_H */
