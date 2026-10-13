/*
 * Service registry (core side): maps name -> pointer only, never interprets the ops type
 * (type safety is between plugins). publish runs in the provider's init and lookup in the
 * consumer's init, so init order guarantees a hit. Duplicate name => -EEXIST (no overwrite);
 * not found => BR_NULL.
 */
#ifndef BR_CORE_BR_SVC_H
#define BR_CORE_BR_SVC_H

#include <br/core/br_types.h>

/* Registry capacity: static upper bound (the design allows manifest trimming). */
#ifndef BR_SERVICE_MAX
#define BR_SERVICE_MAX 32u
#endif

/* `ops` is owned by the provider (usually a static const struct). Duplicate name => -EEXIST;
 * table full => -ENOSPC; null name/ops => -EINVAL. */
int br_service_publish(const char *name, const void *ops);

/* Not present => BR_NULL. */
const void *br_service_lookup(const char *name);

/* ---- observation ---- */
br_u32 br_service_count(void);
const char *br_service_name_at(br_u32 index);   /* index out of range => BR_NULL */
/* Lookup count for this service. */
br_u32 br_service_lookup_hits(const char *name);

/* The selftest suite is declared in core/selftest/ (test surface, not plugin API). */

#endif /* BR_CORE_BR_SVC_H */
