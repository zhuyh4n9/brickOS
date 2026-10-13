/*
 * Dump service implementation: one snapshot scene orchestrating hexdump (presentation), backtrace,
 * trace and memleak. This plugin only bounds-checks against the region table, sequences outputs,
 * and does LATE-phase init of its own state + boot snapshot. Capture uses static buffers only and
 * never touches the heap or scheduler.
 *
 * WORKAROUND(br-wa-boot-001): the old exported conformance entry (which also called the four
 * plugins' init) is gone; init is driven by plugin_manager in `[[dep]]` topological order and each
 * plugin's selftest lives in src/<short>_selftest.c.
 * WORKAROUND(br-wa-debug-002): presentation writes straight to the early console; repay by routing
 * it through the debug bridge framing (content and bounds checks unchanged, only the sink changes).
 *
 * Threading discipline: all thread-only (prints, walks the region table and heap observation).
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

/* Helpers: libc-free fixed-size string append, plus region attribute decode. Attributes render as
 * "flag string + kind", e.g. `RO|CACHED|kind=heap`; if no attribute bit is set they render as `RW`
 * (RW is not an attrs bit, just the default readable/writable form). */

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

/* BR_MM_KIND_* -> name */
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

int br_dump_init(void)
{
    /* Register this plugin's two event names. br_trace_svc_register may return negative (trace
     * service not initialized); init phase order is plugin_manager's job, so this failure
     * is not fatal and the ids are discarded (v1 emits no own events; the table is for offline
     * decoding only). */
    (void)br_trace_svc_register("dump.begin");
    (void)br_trace_svc_register("dump.end");
    return 0;
}

br_u32 br_dump_regions(void)
{
    const br_u32 count = br_mm_region_count();
    br_u32       printed = 0u;

    for (br_u32 i = 0u; i < count; i++) {
        const br_mm_region_t *region = br_mm_region_get(i);
        char                  attrs[64];

        /* Observation contract: out of range => NULL. A count mismatch means core's observation
         * surface is inconsistent; the test case reports it red */
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

    /* STRICT (default): reject an undeclared address */
    if ((region == BR_NULL) && ((flags & BR_DUMP_F_FORCE) == 0u)) {
        return BR_ERR(EINVAL);
    }

    kind = (region != BR_NULL) ? dump_kind_name(region->attrs) : "none";

    /* Upper bound truncation: prevents one dump from saturating the UART */
    if (n > BR_DUMP_MAX_BYTES) {
        n       = BR_DUMP_MAX_BYTES;
        cap_hit = BR_TRUE;
    }

    if (region != BR_NULL) {
        /* Past the first region's end: truncate there, do not touch the second region */
        const br_size_t off   = addr - region->base;
        const br_size_t avail = (off < region->size) ? (region->size - off) : 0u;

        if (n > avail) {
            n       = avail;
            end_hit = BR_TRUE;
        }
    } else {
        /* FORCE + undeclared address: print the header only, never read (see br_dump.h) */
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

/* Returns the number of lines printed, so br_dump_all() can assert "everything was printed". A
 * failed sub-observation also prints one explanatory line and counts it. */
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

/*
 * Line accumulation: 2 fixed titles + regions (count + 1) + heap (self-reported) + leaks + trace
 * events + backtrace frames.
 * The leaks term is approximate: br_memleak_report() returns only the live block count, while its
 * real output is 1 + owner count + sum of min(8, live blocks per owner). Callers use the total only
 * as a ">= " all-printed threshold, never for equality.
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

/* Plugin lifecycle hooks referenced by the generated plugin_desc.c. Self-declared prototypes
 * -Wmissing-prototypes. LATE init does only this plugin's own work; trace/backtrace/hexdump/memleak
 * init runs earlier in the same phase via the `[[dep]]` edges. */
int dump_early_init(void);
int dump_init(void);
int dump_start(void);

/* EARLY phase: no action; no heap or interrupts yet, and dump reads the heap, so wait for LATE. */
int dump_early_init(void)
{
    return 0;
}

/* LATE phase: own init, then one boot snapshot. Conformance cases are driven by core's selftest
 * pass, and the other plugins' init already ran via plugin_manager in `[[dep]]` order. */
int dump_init(void)
{
    const int rc = br_dump_init();
    if (rc != 0) {
        return rc;   /* plugin_manager halts on non-zero rc */
    }

    /* One boot snapshot (where the three gates capture evidence; line conventions in br_dump.h) */
    const br_u32 dump_lines = br_dump_all();
    br_log_info("dbg: boot snapshot lines=%lu", (br_u64)dump_lines);

    return 0;
}

int dump_start(void)
{
    return 0;
}
