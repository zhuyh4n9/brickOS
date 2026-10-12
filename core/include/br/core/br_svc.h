/*
 * brickOS prototype v0.2.0 — service registry (core side)
 *
 * Design basis: `3-01` §9 (`br_service_publish` / `br_service_lookup`),
 *           `3-06` (service management), `11-01` (service model), `1-01` §9 (publish/lookup happen in the init phase).
 *
 * Semantics (verbatim from the design, nothing added or dropped):
 *   - **core only maps name -> pointer**, it does not interpret the ops type (type safety is between plugins);
 *   - publish = called by the provider in its own `init`; lookup = called by the consumer in its own `init`
 *     => phase order + init topological order **together guarantee** it is found (the two completion points of 1-01 §9);
 *   - duplicate name => `-EEXIST` (no overwrite -- overwriting would make "who is the real one" depend on boot order);
 *   - not found => return `BR_NULL` (not an error code: the designed signature returns a pointer, `3-01` §9).
 *
 * Deviation from the design: this cut adds two **observation** entries (count/name_at) so 5-01 style dumps and
 * consistency cases can read the registry; they do not change publish/lookup semantics (ADR-0005 §2).
 */
#ifndef BR_CORE_BR_SVC_H
#define BR_CORE_BR_SVC_H

#include <br/core/br_types.h>

/* Registry capacity: static upper bound (same style as the trace ring and IRQ domain pool -- the design
 * allows manifest trimming, v0.2 takes the default). */
#ifndef BR_SERVICE_MAX
#define BR_SERVICE_MAX 32u
#endif

/* Publish a service. `ops` is owned by the provider (usually points to a static const struct);
 * duplicate name => -EEXIST; table full => -ENOSPC; null name/ops => -EINVAL. */
int br_service_publish(const char *name, const void *ops);

/* Look up a service. Not present => BR_NULL. */
const void *br_service_lookup(const char *name);

/* ---- observation ---- */
br_u32 br_service_count(void);
const char *br_service_name_at(br_u32 index);   /* index out of range => BR_NULL */
/* Lookup count for this service (accounting: 5-01 §4 "who uses whom"). */
br_u32 br_service_lookup_hits(const char *name);

/*
 * The registry selftest suite moved to `core/selftest/svc_selftest.c` (ADR-0010), so it is not declared here.
 * Why not keep it in the public header: selftest is a **test surface**, not a plugin capability -- declaring it in
 * `br_*.h` would turn "change one case" into an interface change (the golden interface hash covers exactly the
 * public declaration surface); implementation and declarations both live in core/selftest/.
 */

#endif /* BR_CORE_BR_SVC_H */
