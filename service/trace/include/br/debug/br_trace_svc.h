/*
 * service/trace — trace observation service (public declaration face)
 *
 * Design source: `docs/5-debug/5-01-debug.md` §1 (trace plugin: 16 B fixed-size event + ring buffer +
 * "zero strings on the hot path: the id→name mapping table is exported with plugin metadata, decoded offline") and
 * the v1.0 plugin list row in `docs/1-architecture/1-03-roadmap.md` §1
 * (`service/trace` | Service | 16B event ring buffer | core | M2).
 *
 * ★ **storage and emit point in core, decode/present/filter in this plugin** (ADR-0003 ruling):
 *   core's `br_trace_emit()` is a member of the CA-3 ISR whitelist (interrupt/fault paths must be able to trace,
 *   and must not depend on "whether the service is already init"), so **the ring itself stays in core** (`br/core/br_trace.h`);
 *   this plugin is its **only consumer**: drain, decode (id→name), count, present as lines.
 *   This layering does not conflict with 5-01 §1 — 5-01 wants "trace is a plugin (the whole layer can be removed)", while
 *   "who owns the ring" is only implementation ownership; with the plugin absent the ring still records, just with no consumer.
 *
 * ISR safety: all functions of this plugin are **thread-only** (drain advances the consume cursor).
 */
#ifndef BR_DEBUG_BR_TRACE_SVC_H
#define BR_DEBUG_BR_TRACE_SVC_H

#include <br/core/br_types.h>

/* Base for dynamic event ids: core fixed ids are 1..BR_TRACE__COUNT-1 (see br/core/br_trace.h),
 * and plugin-dynamically-registered event ids are allocated from this base, so the ranges never overlap. */
#define BR_TRACE_SVC_ID_BASE   1024u
#define BR_TRACE_SVC_ID_SLOTS  32u     /* dynamic id slots (manifest trimming deferred to v1.x) */

/* LATE-phase init: clear counters + register this plugin's own event names. Returns 0. */
int br_trace_svc_init(void);

/* The selftest entry (TC-DBG-00x) has moved to `src/trace_selftest.c` (ADR-0010): the generated artifact emits
 * `.selftest = trace_selftest` per symbol_prefix, driven uniformly by core after all starts.
 * ★ The test entry does **not** enter this plugin's golden interface face — otherwise editing one case would be a public interface change
 *   (the interface hash covers exactly the declaration face), conflicting with the "selftest separated from production" ruling. */

/*
 * marker: called from thread context (not the ISR path — the ISR path uses core's
 * `BR_TRACE_EVT`/`br_trace_emit` directly). Semantics:
 *   ① register `tag` into the dynamic name table (hash → slot; **if the slot is taken by another tag, probe forward linearly** for an
 *      empty slot, scanning at most the whole table — a 32-slot table must not "error out on one hash collision", which would let
 *      "who registered first" decide whether others can register; table full, or same name with a different pointer ⇒ `-ENOSPC`);
 *   ② emit one event `id` (dynamic id), a = 32-bit hash of tag, b = current clock (us).
 * Returns the event id (>= BR_TRACE_SVC_ID_BASE) or a negative errno.
 */
int br_trace_svc_marker(const char *tag);

/* Explicitly register a dynamic event name (lets other plugins name their own ids); returns id or negative errno.
 * Conflict policy same as `br_trace_svc_marker`: linear probing, table full ⇒ `-ENOSPC`. */
int br_trace_svc_register(const char *name);

/* id → name: check the dynamic table first, then fall back to core's fixed table (`br_trace_name`). Unknown ⇒ "?". */
const char *br_trace_svc_name(br_u32 id);

/*
 * Drain and present: take at most `max_events` from the core ring (0 = unlimited, until exhausted),
 * print each as `[TRACE] #<seq> id=<id> name=<name> a=0x<hex> b=0x<hex>`,
 * and accumulate a count for each id. Returns the number of events drained this time.
 */
br_u32 br_trace_svc_report(br_u32 max_events);

/* Summary line: `[TRACE] total=<n> drained=<n> overrun=<n> ids=<n> live=<n>`
 *   total   = cumulative number emitted by producers (br_trace_total)
 *   drained = cumulative number drained by this plugin
 *   overrun = times a producer overwrote an unconsumed slot (br_trace_overrun; >0 ⇒ this history is untrustworthy)
 *   ids     = number of distinct event ids seen
 *   live    = entries still unconsumed (total - drained, lower bound 0)
 * Returns the current value of `drained`. */
br_u32 br_trace_svc_summary(void);

/* Cumulative **consumed** count for one id (observation/test criterion). */
br_u32 br_trace_svc_seen(br_u32 id);

/* Clear the "consumed" counters and the live table (does not touch the core ring or the producer cursor). */
void br_trace_svc_reset(void);

#endif /* BR_DEBUG_BR_TRACE_SVC_H */
