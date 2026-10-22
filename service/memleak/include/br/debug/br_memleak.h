/*
 * Leak and heap-corruption reporting service (public surface). Implements the intersection of the
 * cheap v1.x substitute (red zones / poisoning / double-free interception, done by the core
 * heap) and the v2 per-plugin arena, which here only reaches ownership tags: no budget cap, no
 * enforced ownership, no OOM policy. So it can report which blocks were not returned and who tagged
 * them, but not that some plugin exceeded a budget.
 *
 * Dependencies: core's observation contract (br_heap_stats_get / br_heap_walk / br_heap_check).
 * Read-only: never modifies the heap. ISR safety: thread-only throughout.
 */
#ifndef BR_DEBUG_BR_MEMLEAK_H
#define BR_DEBUG_BR_MEMLEAK_H

#include <br/hal/br_mem.h>
#include <br/core/br_types.h>

/* Max detail blocks listed per owner tag in the report (beyond that, only a count) */
#define BR_MEMLEAK_MAX_ROWS_PER_OWNER  8u

/* LATE-phase init: register this plugin's owner tag (base accounting) + trace event names. Returns 0. */
int br_memleak_init(void);

/*
 * Leak report. Prints `[LEAK] summary live=<n> bytes=<n> owners=<n> corrupt=<n> high_water=<n>`,
 * one `[LEAK] owner=<name> live=<n> bytes=<n>` line per owner with live blocks, then at most
 * BR_MEMLEAK_MAX_ROWS_PER_OWNER `[LEAK]   blk seq=<n> addr=0x<hex> size=<n> caller=0x<hex>` rows.
 * Returns the number of live blocks (convenient for case criteria).
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
