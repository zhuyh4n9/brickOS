/*
 * service/dump — dump service (public declaration surface): region list / memory
 * dump / heap / trace / backtrace / leaks —— the v1 deliverable subset of the
 * "mini ramdump" in 5-01 §3.
 *
 * Design source: `docs/5-debug/5-01-debug.md`
 *   §2 `MEMRD`/`TRACE_READ` (region/heap summary of `GETINFO`) and
 *   §3 capture set ("register set, trace ring, memory region table, plugin table,
 *   arena stats") + "constraint: the capture path uses only static buffers, never
 *   touches the heap/scheduler".
 *
 * Ownership boundary (why dump is an orchestrator, not another implementation):
 *   - presentation primitive → `service/hexdump` (this plugin's dependency);
 *   - stack backtrace   → `service/backtrace`;
 *   - ring consumption → `service/trace`;
 *   - leak/redzone accounting → `service/memleak`;
 *   - this plugin does only three things: (1) region-table bounds check (dump
 *     refused for undeclared regions); (2) orchestrate the above outputs into one
 *     "scene"; (3) LATE-phase init of its own state + startup snapshot.
 *     ★ The debug-domain selftest entry (the conformance function dump exported
 *     before ADR-0010) was moved out of this header: selftest is a test surface,
 *     not a plugin capability; putting it in the golden interface would make
 *     "change one test case" an interface change.
 *
 * ISR safety: all thread-only (prints, and walks heap observation).
 */
#ifndef BR_DEBUG_BR_DUMP_H
#define BR_DEBUG_BR_DUMP_H

#include <br/core/br_types.h>

/* br_dump_memory flags (append-only) */
#define BR_DUMP_F_STRICT   0x0000u   /* default: undeclared address ⇒ -EINVAL (refuse) */
#define BR_DUMP_F_FORCE    0x0001u   /* bypass the region-table bounds check (P4/machine semantics) */

/*
 * ★ Exact v1 semantics of `BR_DUMP_F_FORCE` (ruling + rationale, see ADR-0003 §2.4/§2.6):
 *   it bypasses the region-table check (does not return `-EINVAL`), but does NOT
 *   actually read undeclared addresses —— the page table is built from the region
 *   snapshot, off-table addresses are invalid (e.g. `0x42000000` lies outside the
 *   RAM window and is the anchor `irq_conf.c` uses to force a translation fault),
 *   and the presentation primitive (hexdump) has no extable-protected read path
 *   (the full `MEMRD` form in design `5-01 §2` requires one, not delivered in v1)
 *   ⇒ a real read is an unrecoverable data abort, turning the "debug service" into
 *   a "crash source".
 *   So v1 FORCE = print the header + truthfully mark "not read", return 0; a real
 *   read needs an extable-protected probe (re-feed item).
 */

/* Per-dump byte upper bound (v1 static budget; prevents one dump from saturating UART) */
#define BR_DUMP_MAX_BYTES  4096u

/* LATE-phase init: register this plugin's own trace event names (reachable for
 * offline decode). Returns 0.
 * (No longer also "selftest prerequisite": selftest is driven uniformly by the
 * plugin manager after all STARTs, see the file header.) */
int br_dump_init(void);

/*
 * The debug-domain selftest entry (the conformance function dump exported before
 * ADR-0010) has been removed, no longer a public API:
 *   - each plugin's cases live in `src/<short>_selftest.c`, hooked via the
 *     descriptor `.selftest`, and core drives them in one pass
 *     (`br_plugin_manager_selftest`);
 *   - dump's own cases (TC-DBG-030/031/032/100) moved to `src/dump_selftest.c`,
 *     entry name `dump_selftest` derived from symbol_prefix by the generator;
 *   - the old entry's "call the four inits on behalf" orchestration is gone too:
 *     init is driven by plugin_manager in `[[dep]]` topological order (ADR-0005
 *     ruling G6: non-zero rc ⇒ stop).
 * ★ The test entry does NOT enter this plugin's golden interface surface ——
 *   otherwise changing one case becomes a public interface change (the interface
 *   hash covers exactly the declaration surface), conflicting with the
 *   "selftest separated from production" ruling.
 */

/* Print the region list (the "memory region table" of design §3 capture set): one
 * line each
 *   `[DUMP] region[<i>] kind=<name> attrs=0x<hex> base=0x<hex> size=<n>`
 * plus a `[DUMP] regions=<n>` line. Returns the number of regions printed. */
br_u32 br_dump_regions(void);

/*
 * Dump a memory range per the region table (header + hexdump). Semantics:
 *   - address undeclared ⇒ `-EINVAL` (unless flags has BR_DUMP_F_FORCE);
 *   - spans two regions ⇒ dump only the part inside the first region (with a
 *     truncation notice);
 *   - len > BR_DUMP_MAX_BYTES ⇒ truncate to the bound (with a notice);
 *   - len == 0 ⇒ print only the header, return 0.
 * Returns the number of bytes actually dumped, or a negative errno.
 */
int br_dump_memory(br_uintptr_t addr, br_size_t len, br_u32 flags);

/* Print heap stats + pool geometry ("arena stats" of design §3): one `[DUMP] heap ...` line each. */
br_u32 br_dump_heap(void);

/* Consume and present trace (delegates to service/trace report; `max_events` semantics as there). Returns consumed count. */
br_u32 br_dump_trace(br_u32 max_events);

/* Capture + print the current stack backtrace (delegates to service/backtrace). Returns frame count. */
br_u32 br_dump_backtrace(void);

/* Print the leak report (delegates to service/memleak). Returns live block count. */
br_u32 br_dump_leaks(void);

/* A full scene: regions → heap → leaks → trace → backtrace, first line
 * `[DUMP] ===== snapshot start =====`, last line `[DUMP] ===== snapshot end =====`.
 * Returns the number of lines printed (so cases can assert "everything was printed"). */
br_u32 br_dump_all(void);

#endif /* BR_DEBUG_BR_DUMP_H */
