/*
 * Dump service (public surface): region list, memory dump, heap, trace, backtrace and leaks.
 * This plugin is an orchestrator: hexdump does presentation, the other services do the work, and
 * dump only bounds-checks against the region table and sequences one snapshot. The capture path
 * uses static buffers only and never touches the heap or scheduler.
 * ISR safety: all thread-only (prints, heap observation).
 */
#ifndef BR_DEBUG_BR_DUMP_H
#define BR_DEBUG_BR_DUMP_H

#include <br/core/br_types.h>

/* br_dump_memory flags (append-only). BR_DUMP_F_STRICT is the default: an undeclared address =>
 * -EINVAL. BR_DUMP_F_FORCE bypasses the region-table bounds check. */
#define BR_DUMP_F_STRICT   0x0000u
#define BR_DUMP_F_FORCE    0x0001u

/*
 * BR_DUMP_F_FORCE semantics: it bypasses the region-table check (no -EINVAL) but does not read
 * undeclared addresses. Page tables are built from the region snapshot, so off-table addresses are
 * invalid, and the presentation primitive (hexdump) has no extable-protected read path => a real
 * read is an unrecoverable data abort. FORCE prints the header, marks "not read" and returns 0;
 * a real read needs an extable-protected probe, not delivered in v1.
 */

/* Per-dump byte upper bound (v1 static budget; prevents one dump from saturating UART) */
#define BR_DUMP_MAX_BYTES  4096u

/* LATE-phase init: registers this plugin's own trace event names for offline decode. Returns 0. */
int br_dump_init(void);

/* Print the region list, one line per region:
 *   `[DUMP] region[<i>] kind=<name> attrs=0x<hex> base=0x<hex> size=<n>` + `[DUMP] regions=<n>`.
 * Returns the number of regions printed. */
br_u32 br_dump_regions(void);

/*
 * Dump a memory range per the region table (header + hexdump): an undeclared address => -EINVAL
 * unless BR_DUMP_F_FORCE is set; a range spanning two regions truncates at the first region's end
 * with a notice; len > BR_DUMP_MAX_BYTES truncates with a notice; len == 0 prints the header only.
 * Returns the number of bytes actually dumped, or a negative errno.
 */
int br_dump_memory(br_uintptr_t addr, br_size_t len, br_u32 flags);

/* Print heap stats + pool geometry, one `[DUMP] heap ...` line each. */
br_u32 br_dump_heap(void);

/* Consume and present trace (delegates to service/trace report; `max_events` semantics as there). Returns consumed count. */
br_u32 br_dump_trace(br_u32 max_events);

/* Capture + print the current stack backtrace (delegates to service/backtrace). Returns frame count. */
br_u32 br_dump_backtrace(void);

/* Print the leak report (delegates to service/memleak). Returns live block count. */
br_u32 br_dump_leaks(void);

/* Full scene: regions -> heap -> leaks -> trace -> backtrace, first line
 * `[DUMP] ===== snapshot start =====`, last line `[DUMP] ===== snapshot end =====`.
 * Returns the number of lines printed (so cases can assert "everything was printed"). */
br_u32 br_dump_all(void);

#endif /* BR_DEBUG_BR_DUMP_H */
