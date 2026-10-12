/*
 * brickOS prototype v0.2.0 — service registry **internal** contract (core-private, not for plugins)
 *
 * Why this header exists (same technique as `core/src/sched/sched_internal.h` / `core/src/irq/irq_internal.h`):
 * the registry selftest suite (`core/selftest/svc_selftest.c`, ADR-0010) needs temporary `selftest.`-prefixed
 * entries for a publish -> lookup round trip, and must **remove them from the table** at the end. The public
 * surface can do neither:
 *
 *   - `svc_remove` (white-box removal): the public API (`br_service_publish` / `br_service_lookup` /
 *     `br_service_count` / `br_service_name_at` / `br_service_lookup_hits`) has **no removal** -- a duplicate
 *     name only reports -EEXIST and publishing is one-way (design `3-01` §9 has no unpublish). So "clear the
 *     entries I published" is **impossible** through the public surface:
 *       * no cleanup => case entries stay in the table and pollute later registry dumps;
 *       * a unique name per run only avoids -EEXIST on repeated runs, it does not fix the point above;
 *       * "rebuild the registry" has no API and should not exist in production semantics -- the registry only grows.
 *     Conclusion: `svc_remove` must be exposed white-box (rather than keeping a parallel table inside the case).
 *   - `name_equal`: the case must verify that the name reported by `br_service_name_at()` is really the one it
 *     published. This is the **production** comparison function (publish/lookup/hits all use it), not a test-only
 *     one -- keeping a single source of truth beats writing another "looks the same" string compare in the case.
 *
 *   Placed under `core/src/svc/` rather than `br/core/br_svc.h`: these are core-private symbols that do **not**
 *   enter the golden interface surface -- so "change one case" does not change the interface hash (same origin as
 *   ADR-0005 ruling 9 on selftest hooks: the test surface is not a plugin capability).
 */
#ifndef BR_SVC_INTERNAL_H
#define BR_SVC_INTERNAL_H

#include <br/core/br_types.h>

/* Name equality (the production comparison used by publish/lookup/lookup_hits; selftest uses it to recheck
 * what name_at returned). */
br_bool name_equal(const char *a, const char *b);

/* Remove one entry from the registry (name not in table => silent return). Production has no unpublish;
 * only selftest uses this to clear its temporary entries. */
void svc_remove(const char *name);

#endif /* BR_SVC_INTERNAL_H */
