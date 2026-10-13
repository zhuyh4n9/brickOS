/* Trace observation service (public surface). The 16 B event ring and ISR emit point live in core
 * (br/core/br_trace.h br_trace_emit, on the CA-3 ISR whitelist); this plugin is the only consumer:
 * drain, decode id->name, count, present as lines. Without the plugin the ring still records.
 * ISR safety: all functions here are thread-only (drain advances the consume cursor). */
#ifndef BR_DEBUG_BR_TRACE_SVC_H
#define BR_DEBUG_BR_TRACE_SVC_H

#include <br/core/br_types.h>

/* Base for dynamic event ids; core fixed ids are 1..BR_TRACE__COUNT-1, so ranges never overlap. */
#define BR_TRACE_SVC_ID_BASE   1024u
/* Dynamic id slots (manifest trimming deferred to v1.x). */
#define BR_TRACE_SVC_ID_SLOTS  32u

/* LATE-phase init: clear counters + register this plugin's own event names. Returns 0. */
int br_trace_svc_init(void);

/* Marker: register `tag` in the dynamic name table (hash -> slot, linear probing for an empty
 * slot; table full or same name with a different pointer => -ENOSPC); then emit one event with the
 * dynamic id, a = 32-bit hash of tag, b = current clock (us). Thread context only; ISRs use
 * br_trace_emit. Returns the event id (>= BR_TRACE_SVC_ID_BASE) or a negative errno. */
int br_trace_svc_marker(const char *tag);

/* Register a dynamic event name; returns id or negative errno (linear probing, full => -ENOSPC). */
int br_trace_svc_register(const char *name);

/* id -> name: check the dynamic table, then core's fixed table (br_trace_name); unknown => "?". */
const char *br_trace_svc_name(br_u32 id);

/* Drain and present at most `max_events` from the core ring (0 = unlimited, until exhausted), one
 * `[TRACE] #<seq> id=<id> name=<name> a=0x<hex> b=0x<hex>` line each, accumulating a per-id count.
 * Returns the number of events drained this time. */
br_u32 br_trace_svc_report(br_u32 max_events);

/* Summary line `[TRACE] total=<n> drained=<n> overrun=<n> ids=<n> live=<n>`: total = cumulative
 * emitted; drained = cumulative drained here; overrun = producer overwrites of an unconsumed slot
 * (>0 => untrustworthy); ids = distinct ids seen; live = still unconsumed. Returns `drained`. */
br_u32 br_trace_svc_summary(void);

/* Cumulative **consumed** count for one id (observation/test criterion). */
br_u32 br_trace_svc_seen(br_u32 id);

/* Clear the "consumed" counters and the live table (does not touch the core ring or the producer cursor). */
void br_trace_svc_reset(void);

#endif /* BR_DEBUG_BR_TRACE_SVC_H */
