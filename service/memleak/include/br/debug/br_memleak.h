/*
 * service/memleak — leak and heap-corruption reporting service (public declaration surface)
 *
 * Design source: `docs/5-debug/5-01-debug.md` §4 "ASan / memleak strategy (layered, honest
 * engineering)":
 *   - **cheap target substitute** (v1.x): "TLSF red zones (malloc header/footer guards) +
 *     freelist poisoning + stack canary";
 *   - **target memleak** (v2.0): "per-plugin arena accounting: allocations carry plugin
 *     ownership, leak report per plugin at shutdown/dump".
 *
 * ★ Honest statement (ADR-0003): this prototype implements the **intersection of the two lines
 *   above** —
 *   the v1.x cheap mechanisms (red zones/poisoning/double-free interception, implemented by the
 *   core heap) **are already in place**;
 *   while the v2 per-plugin arena **only reaches "ownership tags"** (core's br_heap_owner_* API:
 *   the "current owner" is recorded in the block header at allocation time), with **no** budget
 *   cap, no enforced ownership, no OOM policy.
 *   So this plugin can answer "which blocks were not returned, who tagged them, where they were
 *   allocated", but not "some plugin exceeded its budget".
 *
 * Dependencies: core's **observation contract**
 * (`br_heap_stats_get`/`br_heap_walk`/`br_heap_check`).
 * This plugin does not modify the heap, only reads it — another instance of "if the consumer is
 * a plugin, the contract cannot hide inside core" (same shape as the treatment of
 * `br_irq_stats_get` in 3-02 §17.4).
 *
 * ISR safety: **thread-only** throughout.
 */
#ifndef BR_DEBUG_BR_MEMLEAK_H
#define BR_DEBUG_BR_MEMLEAK_H

#include <br/core/br_mem.h>
#include <br/core/br_types.h>

/* Max detail blocks listed per owner tag in the report (beyond that, only a count) */
#define BR_MEMLEAK_MAX_ROWS_PER_OWNER  8u

/* LATE-phase init: register this plugin's owner tag (base accounting) + trace event names. Returns 0. */
int br_memleak_init(void);

/* The self-test entry (TC-DBG-04x) moved to `src/memleak_selftest.c` (ADR-0010): the generated
 * artifact emits `.selftest = memleak_selftest` per symbol_prefix, and core drives all of them
 * after start.
 * ★ Test entries do **not** enter this plugin's golden interface surface — otherwise editing a
 *   case would become a public interface change (the interface hash covers exactly the
 *   declaration surface), conflicting with "self-tests separate from production". */

/*
 * Leak report. Prints:
 *   `[LEAK] summary live=<n> bytes=<n> owners=<n> corrupt=<n> high_water=<n>`
 *   one line per owner with live blocks: `[LEAK] owner=<name> live=<n> bytes=<n>`
 *   detail lines under that owner (at most BR_MEMLEAK_MAX_ROWS_PER_OWNER):
 *       `[LEAK]   blk seq=<n> addr=0x<hex> size=<n> caller=0x<hex>`
 * Returns the **number of live blocks** (convenient for case criteria).
 */
br_u32 br_memleak_report(void);

/* Live block count / live block count for one owner (for criteria) */
br_u32 br_memleak_live(void);
br_u32 br_memleak_live_by_owner(br_owner_t owner);

/* Corruption count = violations from core `br_heap_check()` (red zones/magic/poison) + peak observed by this plugin */
br_u32 br_memleak_corruptions(void);

/* This plugin's own owner tag (cases use it to assert "the owner tag can single out leaks it caused") */
br_owner_t br_memleak_owner(void);

#endif /* BR_DEBUG_BR_MEMLEAK_H */
