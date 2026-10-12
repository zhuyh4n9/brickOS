/*
 * brickOS prototype v0.2.0 — service/trace **plugin-private** internal contract (not exported, not in include/)
 *
 * Audience: this plugin's own `src/trace_selftest.c`. Living in `src/` (rather than `include/`) is
 * deliberate — everything under `include/` is **plugin-outward surface** (governed by `brickie check`'s
 * interface domain, and part of `descriptor_include` selection); the two entries here are needed **only by
 * the selftest**.
 *
 * ## Why the selftest reads internals
 *   Two TC-DBG-001 criteria have **the internal mechanism itself** as their subject, which the public API
 *   cannot express:
 *     - "the two tags start in the same slot" is the **premise** of the assertion "linear probing is really
 *       exercised"; the case must re-check it with `trc_slot_of` (otherwise a future hash change would
 *       silently turn that assertion into a no-op);
 *     - name decoding must compare **byte by byte** (public `br_trace_svc_name` returns only `const char *`).
 *   Making these two public API would be worse: the hash slot layout would enter the golden interface
 *   surface, so any later implementation change becomes an interface change (though it is plainly internal).
 *
 * ## Boundary discipline (same lineage as framework/vfs-core's `src/vfs_internal.h`)
 *   Expose only the two the selftest really uses, and always **forward to the production implementation**
 *   (never copy logic): the case must run the real algorithm, otherwise it verifies a copy. The rest of the
 *   production code (`trace.c`) stays `static`.
 */
#ifndef BR_DEBUG_BR_TRACE_INTERNAL_H
#define BR_DEBUG_BR_TRACE_INTERNAL_H

#include <br/core/br_types.h>

/* Slot index (production impl is static trc_slot_of: FNV-1a hash then modulo slot count).
 * Use: the case asserts the two tags' **initial** slots match — report early if the collision premise fails. */
br_u32 br_trace_internal_slot_of(const char *name);

/* String compare (production impl is static trc_streq): the case uses it to check byte-for-byte that the decoded name == the registered tag. */
br_bool br_trace_internal_streq(const char *a, const char *b);

#endif /* BR_DEBUG_BR_TRACE_INTERNAL_H */
