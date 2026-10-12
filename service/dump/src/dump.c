/*
 * service/dump — snapshot dump + debug-domain conformance orchestrator (impl)
 *
 * Design source: `docs/5-debug/5-01-debug.md`
 *   §2 `MEMRD`/`TRACE_READ`/`GETINFO` (debug bridge read commands);
 *   §3 mini ramdump capture set ("register set, trace ring, **memory region table**,
 *       arena stats") and constraint "capture path uses **static buffers only**,
 *       never touches heap/scheduler".
 *
 * Ownership boundaries (`br_dump.h` header, ADR-0003 §2.6): this plugin is an
 * **orchestrator**, not another implementation —
 *   presentation primitives → `service/hexdump` (bounds check is not its job:
 *       whoever reads memory asks the region table);
 *   stack backtrace → `service/backtrace` (capture only; symbolization is for the
 *       host-side offline tool);
 *   ring consumption → `service/trace` (the ring lives in core, this plugin is the
 *       only consumer);
 *   leak/redzone accounting → `service/memleak` (reads core's heap observation
 *       contract only).
 *   This plugin does exactly three things: (1) bounds check against the region
 *   table; (2) orchestrate the above into one snapshot; (3) LATE-phase init of its
 *   own state + boot snapshot.
 *
 * ★ The old debug-domain conformance entry point is **no longer in this file**
 *   (ADR-0010): before the split, that exported conformance function both ran
 *   dump's own cases and called trace/backtrace/hexdump/memleak `*_selftest()` and
 *   `*_init()` (a stand-in from v0.1 when there was no phase machine,
 *   WORKAROUND(br-wa-boot-001)). Now:
 *     - each plugin's selftest entry lives in its own `src/<short>_selftest.c`,
 *       exposed via the descriptor hook `.selftest`, and **core drives them in one
 *       pass** (`br_plugin_manager_selftest`);
 *     - init is driven by plugin_manager in `[[dep]]` topological order (this
 *       plugin **no longer** calls other plugins' init).
 *   dump's own cases moved to `src/dump_selftest.c`; ledger conventions are in that
 *   file's header.
 *
 * ★ Presentation sink debt: currently writes straight to the early console, not
 *   through the `5-01 §2` debug bridge (COBS + CRC16 framing + `MEMRD`/`TRACE_READ`
 *   command surface, M3). The repayment action registered under
 *   `WORKAROUND(br-wa-debug-002)` = route the presentation layer through bridge
 *   framing (content and bounds checks unchanged, only the sink changes).
 *
 * Threading discipline: all **thread-only** (prints, walks the region table and
 * heap observation).
 */
#include <br/debug/br_dump.h>

#include <br/debug/br_bt.h>
#include <br/debug/br_hexdump.h>
#include <br/debug/br_memleak.h>
#include <br/debug/br_trace_svc.h>

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>

/* =====================================================================
 * Helpers: libc-free fixed-size string append + region attribute decode
 *
 * The attribute's readable string is given as "flag string + kind", e.g.
 * `RO|CACHED|kind=heap`; if not one of the four attribute bits is set it encodes
 * as `RW` (RW is not an attrs bit, just the default readable/writable form).
 * ===================================================================== */

static void dump_buf_add(char *buf, br_size_t cap, br_size_t *len, const char *s)
{
    if (cap == 0u) {
        return;
    }
    while ((*s != '\0') && ((*len + 1u) < cap)) {
        buf[*len] = *s;
        (*len)++;
        s++;
    }
    buf[*len] = '\0';
}

static void dump_attr_add(char *buf, br_size_t cap, br_size_t *len,
                          br_bool *first, const char *tok)
{
    if (!*first) {
        dump_buf_add(buf, cap, len, "|");
    }
    *first = BR_FALSE;
    dump_buf_add(buf, cap, len, tok);
}

/* BR_MM_KIND_* → name (matches the `regions` axis values in design 3-01 §13.6) */
static const char *dump_kind_name(br_u32 attrs)
{
    switch (BR_MM_KIND(attrs)) {
    case BR_MM_KIND_NONE:     return "none";
    case BR_MM_KIND_IMAGE:    return "image";
    case BR_MM_KIND_STACK:    return "stack";
    case BR_MM_KIND_HEAP:     return "heap";
    case BR_MM_KIND_CONTIG:   return "contig";
    case BR_MM_KIND_PAGE:     return "page";
    case BR_MM_KIND_DMA:      return "dma";
    case BR_MM_KIND_MMIO:     return "mmio";
    case BR_MM_KIND_RESERVED: return "reserved";
    default:                  return "?";
    }
}

static void dump_attrs_str(char *buf, br_size_t cap, br_u32 attrs)
{
    br_size_t len   = 0u;
    br_bool   first = BR_TRUE;

    if (cap == 0u) {
        return;
    }
    buf[0] = '\0';

    if ((attrs & BR_MM_RO) != 0u) {
        dump_attr_add(buf, cap, &len, &first, "RO");
    }
    if ((attrs & BR_MM_NX) != 0u) {
        dump_attr_add(buf, cap, &len, &first, "NX");
    }
    if ((attrs & BR_MM_DEVICE) != 0u) {
        dump_attr_add(buf, cap, &len, &first, "DEVICE");
    }
    if ((attrs & BR_MM_CACHED) != 0u) {
        dump_attr_add(buf, cap, &len, &first, "CACHED");
    }
    if (first) {
        dump_attr_add(buf, cap, &len, &first, "RW");
    }
    dump_attr_add(buf, cap, &len, &first, "kind=");
    dump_buf_add(buf, cap, &len, dump_kind_name(attrs));
}

/* =====================================================================
 * LATE-phase init
 * ===================================================================== */

int br_dump_init(void)
{
    /*
     * Register this plugin's two event names ("start/end of one snapshot dump").
     * The return value may be negative (`br_trace_svc_register` rejects when the
     * trace service is not initialized), but **each plugin's init is the caller's
     * responsibility in phase order**, dump does not init it (see README
     * "ownership boundaries") ⇒ failure here is not fatal, continue; the id is
     * also discarded (v1 emits no own events on the dump path, the name table
     * exists only for offline decoding).
     */
    (void)br_trace_svc_register("dump.begin");
    (void)br_trace_svc_register("dump.end");
    return 0;
}

/* =====================================================================
 * (1) region listing (the "memory region table" of the design §3 capture set)
 * ===================================================================== */

br_u32 br_dump_regions(void)
{
    const br_u32 count = br_mm_region_count();
    br_u32       printed = 0u;

    for (br_u32 i = 0u; i < count; i++) {
        const br_mm_region_t *region = br_mm_region_get(i);
        char                  attrs[64];

        /* Observation contract: out of range ⇒ NULL. A count mismatch means core's
         * observation surface is inconsistent; the test case reports it red */
        if (region == BR_NULL) {
            continue;
        }
        dump_attrs_str(attrs, sizeof attrs, region->attrs);
        br_log_info("[DUMP] region[%u] kind=%s attrs=0x%x (%s) base=0x%lx size=%lu",
                    i, dump_kind_name(region->attrs), region->attrs,
                    attrs, region->base, region->size);
        printed++;
    }

    br_log_info("[DUMP] regions=%u", count);
    return printed;
}

/* =====================================================================
 * (2) bounds-checked memory dump
 * ===================================================================== */

/*
 * ★ Key ruling: `BR_DUMP_F_FORCE` + **undeclared address** ⇒ print the header
 * only, **do not read memory**.
 *
 * Rationale (ADR-0003 §2.4/§2.5): the page tables are built from the region table
 * snapshot, so addresses outside it are invalid; e.g. `0x42000000` lies outside the
 * RAM window (`0x40000000 + 8 MiB`) — in `irq_conf.c` it is exactly the anchor that
 * "reliably takes a translation fault". And hexdump is a pure presentation
 * primitive with **no** extable-protected read primitive (the full MEMRD form in
 * design §2 needs it, v1 did not ship it) ⇒ actually reading here is an
 * unrecoverable data abort, turning the "debug service" into a "crash source".
 *
 * So in this prototype FORCE narrows to: **bypass the region table check** (do not
 * return -EINVAL), faithfully print the header and a "not read" note, return 0.
 * Making it really read needs an extable-protected probe (backport item, see
 * README "to backport") — not hiding the risk inside dump.
 */
int br_dump_memory(br_uintptr_t addr, br_size_t len, br_u32 flags)
{
    const br_mm_region_t *region = br_mm_region_find(addr);
    char       note[96];
    br_size_t  note_len = 0u;
    br_size_t  n        = len;
    br_bool    cap_hit  = BR_FALSE;
    br_bool    end_hit  = BR_FALSE;
    br_bool    unread   = BR_FALSE;
    const char *kind;

    note[0] = '\0';

    /* Default (STRICT): reject any undeclared address */
    if ((region == BR_NULL) && ((flags & BR_DUMP_F_FORCE) == 0u)) {
        return BR_ERR(EINVAL);
    }

    kind = (region != BR_NULL) ? dump_kind_name(region->attrs) : "none";

    /* Upper bound truncation: prevents "one dump saturating the UART" */
    if (n > BR_DUMP_MAX_BYTES) {
        n       = BR_DUMP_MAX_BYTES;
        cap_hit = BR_TRUE;
    }

    if (region != BR_NULL) {
        /* Past the first region's end ⇒ truncate at the region end (do not touch
         * the second region) */
        const br_size_t off   = addr - region->base;
        const br_size_t avail = (off < region->size) ? (region->size - off) : 0u;

        if (n > avail) {
            n       = avail;
            end_hit = BR_TRUE;
        }
    } else {
        unread = BR_TRUE;
        n      = 0u;
    }

    if (cap_hit) {
        dump_buf_add(note, sizeof note, &note_len, " (truncated to BR_DUMP_MAX_BYTES)");
    }
    if (end_hit) {
        dump_buf_add(note, sizeof note, &note_len, " (truncated to region end)");
    }
    if (unread) {
        dump_buf_add(note, sizeof note, &note_len, " (force: region unknown, not read)");
    }

    br_log_info("[DUMP] memory base=0x%lx len=%lu region=%s flags=0x%x%s",
                addr, n, kind, flags, note);

    if (n > 0u) {
        (void)br_hexdump((const void *)addr, n);
    }
    return (int)n;
}

/* =====================================================================
 * (3) heap stats + pool geometry (the design §3 "arena stats")
 * ===================================================================== */

/*
 * Returns the **number of printed lines** (the header only requires "print heap
 * stats + pool geometry", no return value specified; lines are chosen here so
 * `br_dump_all()` can accumulate a "everything was indeed printed" criterion).
 * When a sub-observation fails it also prints one explanatory line and counts it —
 * the return value always equals the number of lines this function printed.
 */
br_u32 br_dump_heap(void)
{
    br_u32 lines = 0u;
    br_mem_layout_t lay;

    if (br_mem_layout(&lay) == 0) {
        br_log_info("[DUMP] heap pool base=0x%lx size=%lu",
                    lay.heap.base, lay.heap.size);
        lines++;
        br_log_info("[DUMP] heap pool.contig base=0x%lx size=%lu",
                    lay.contig.base, lay.contig.size);
        lines++;
        br_log_info("[DUMP] heap pool.page base=0x%lx size=%lu page_size=%u page_total=%u",
                    lay.page.base, lay.page.size, lay.page_size, lay.page_total);
        lines++;
        br_log_info("[DUMP] heap pool.dma base=0x%lx size=%lu",
                    lay.dma.base, lay.dma.size);
        lines++;
    } else {
        br_log_info("[DUMP] heap not ready (br_mem_layout)");
        lines++;
    }

    {
        br_heap_stats_t st;

        if (br_heap_stats_get(&st) == 0) {
            br_log_info("[DUMP] heap stats total=%lu used=%lu free=%lu max_free=%lu high_water=%lu",
                        st.total, st.used, st.free_bytes, st.max_free_block, st.high_water);
            lines++;
            br_log_info("[DUMP] heap alloc n_alloc=%u n_free=%u n_live=%u n_fail=%u",
                        st.n_alloc, st.n_free, st.n_live, st.n_fail);
            lines++;
            br_log_info("[DUMP] heap guard redzone=%u canary=%u double_free=%u bad_free=%u owners=%u",
                        st.redzone_hits, st.canary_hits, st.double_free,
                        st.bad_free, st.owner_count);
            lines++;
        } else {
            br_log_info("[DUMP] heap stats unavailable (br_heap_stats_get)");
            lines++;
        }
    }

    {
        br_page_stats_t pg;

        if (br_page_stats(&pg) == 0) {
            br_log_info("[DUMP] page stats total=%u used=%u free=%u fail=%u max_run=%u",
                        pg.total, pg.used, pg.free, pg.fail, pg.max_run);
            lines++;
        } else {
            br_log_info("[DUMP] page stats unavailable (br_page_stats)");
            lines++;
        }
    }

    return lines;
}

/* =====================================================================
 * (4) three forwarding calls (this plugin does not reimplement them)
 * ===================================================================== */

br_u32 br_dump_trace(br_u32 max_events)
{
    /* max_events semantics same as `br_trace_svc_report` (0 = unlimited, report all) */
    return br_trace_svc_report(max_events);
}

br_u32 br_dump_backtrace(void)
{
    return br_bt_print();
}

br_u32 br_dump_leaks(void)
{
    return br_memleak_report();
}

/* =====================================================================
 * (5) one complete snapshot
 * ===================================================================== */

/*
 * Line accumulation convention (the header requires "return the number of printed
 * lines so cases can assert everything was printed"):
 *   fixed titles 2 (first/last) + [regions: count + 1 (the regions=<n> line)]
 *   + heap section (br_dump_heap reports its own line count)
 *   + leaks section (approximated: 1 summary line + 1 line per live block)
 *   + trace section (event count) + backtrace section (frame count).
 *
 * ★ Why the leaks section is an "approximation" and not an exact line count:
 *   `br_memleak_report()`'s declared surface returns only the **live block count**;
 *   its real output lines = 1 + owner count + Σ min(8, live blocks per owner), and
 *   neither the "owner distribution" nor the "8 detail lines per owner cap" is on
 *   the returned surface (the declared surface is frozen, no new functions allowed).
 *   So the approximation is the **same order** but can be off in both directions
 *   (many owners undercounts the owner count, many blocks in one owner overcounts
 *   because of the 8-line cap). Callers only use it to judge "all printed"
 *   (>= threshold), **never for equality assertions**. Exact per-line accounting
 *   needs memleak to return a line count — recorded in README "to backport".
 */
br_u32 br_dump_all(void)
{
    br_u32 lines = 0u;
    br_u32 nreg;
    br_u32 live;
    br_u32 events;
    br_u32 frames;

    br_log_info("[DUMP] ===== snapshot start =====");
    lines++;

    nreg    = br_dump_regions();
    lines  += nreg + 1u;

    lines  += br_dump_heap();

    live    = br_dump_leaks();
    lines  += live + 1u;          /* approximation: one summary line + one line per live block */

    events  = br_dump_trace(0u);
    lines  += events;

    frames  = br_dump_backtrace();
    lines  += frames;

    br_log_info("[DUMP] ===== snapshot end =====");
    lines++;

    return lines;
}

/* =====================================================================
 * Plugin lifecycle hooks (name = symbol_prefix(short) + phase; referenced by the
 * generated `build/gen/service/dump/plugin_desc.c`; design 1-01 §9 / 3-05 §2)
 * Self-provided prototypes satisfy -Wmissing-prototypes (hook names are derived by
 * the generator, not in the public header nor in [[export]]).
 *
 * ★ LATE init only does **this plugin's own** work (its own init + boot snapshot) —
 *   the debug-domain consistency cases already moved to `src/dump_selftest.c` per
 *   ADR-0010; their driver is no longer init but the **plugin manager**, which after
 *   all STARTs uniformly calls the descriptor's `.selftest` hook. Hence the
 *   `app/hello → service/dump` M0 exemption **was removed from
 *   `product.toml [lint].allow_edges`** (with it removed `brickie check` is still
 *   0 errors = evidence), and dump also **no longer calls** the init of
 *   trace/backtrace/hexdump/memleak: the four `kind = "init"` edges in brick.toml
 *   guarantee their init already ran in the LATE phase under plugin_manager in
 *   topological order (phase monotonicity is enforced there; non-zero rc ⇒
 *   `[PLUGIN] FAIL` + halt, ADR-0005 ruling G6).
 * ===================================================================== */
int dump_early_init(void);
int dump_init(void);
int dump_start(void);

/* EARLY phase: no action (no heap/threads, interrupts off; dump itself reads the
 * heap, so it must wait for LATE). */
int dump_early_init(void)
{
    return 0;
}

/* LATE phase (Service category ⇒ (2) completion point): own init → boot snapshot.
 *
 * There is **no** conformance call here: cases are driven by core's selftest pass
 * (see dump_selftest.c header). It also does not call other plugins' init — that is
 * exactly the orchestration ADR-0010 removed (the four inits are done by
 * plugin_manager in `[[dep]]` topological order **before** this function). */
int dump_init(void)
{
    const int rc = br_dump_init();
    if (rc != 0) {
        return rc;   /* fail-fast halt is performed by plugin_manager (ruling G6) */
    }

    /* One boot snapshot (where the three gates capture evidence; line conventions in br_dump.h) */
    const br_u32 dump_lines = br_dump_all();
    br_log_info("dbg: boot snapshot lines=%lu", (br_u64)dump_lines);

    /* init's return value only expresses "did this plugin initialize" (see ADR-0005
     * §2.5); case red/green is judged by the gates ([DBGCONF] FAIL forbid), the two
     * are not mixed. */
    return 0;
}

/* START phase: this service has nothing that needs interrupts/threads to be available. */
int dump_start(void)
{
    return 0;
}

