/*
 * service/trace plugin-private contract, not exported and not under include/. Audience:
 * this plugin's own src/trace_selftest.c.
 *
 * Two TC-DBG-001 criteria target the internal mechanism, which the public API cannot express: the
 * "two tags start in the same slot" premise, and byte-by-byte name decoding. Both forward to the
 * production implementation (never copy logic), so the case runs the real algorithm.
 */
#ifndef BR_DEBUG_BR_TRACE_INTERNAL_H
#define BR_DEBUG_BR_TRACE_INTERNAL_H

#include <br/core/br_types.h>

/* Production impl is static trc_slot_of (FNV-1a then modulo the slot count); the case checks
 * tags' initial slots match, reporting early if the collision premise fails. */
br_u32 br_trace_internal_slot_of(const char *name);

/* Production impl is static trc_streq; the case compares decoded name and tag byte by byte. */
br_bool br_trace_internal_streq(const char *a, const char *b);

#endif /* BR_DEBUG_BR_TRACE_INTERNAL_H */
