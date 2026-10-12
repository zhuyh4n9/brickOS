/*
 * service/hexdump — hex + ASCII rendering primitives (implementation)
 *
 * Design source: `docs/5-debug/5-01-debug.md`
 *   §2 debug bridge `MEMRD` / host-side `brickie dbg` (renders memory read back into readable lines);
 *   §3 mini ramdump output shape (the "capture path uses static buffers only" constraint lands here:
 *      this file uses a single-line stack buffer, no allocation and no whole-range buffering).
 *
 * ★ The format is a contract (case TC-DBG-020 asserts it byte by byte); the authoritative
 *   description is in the file header of include/br/debug/br_hexdump.h. This implementation expands it into accountable fields:
 *
 *     [addr 16][2 spaces][group area 49][1 space]['|'][ASCII 16]['|']['\n'] = 87 bytes
 *
 *   Group area 49 = 16 groups × (2 hex + 1 separator space) + 1 widened space after group 8.
 *   So between the last hex group and '|' there are 2 spaces (the group-area trailing separator plus the explicit space),
 *   matching the header sample `…00  |Hello, world!...|`; a full line is 87 bytes (including trailing '\n').
 *
 * ★ This plugin does no bounds checking: "whoever reads memory must consult the region table"
 *   (that is service/dump's job, see the br_hexdump.h header). Here only the caller's base/len become lines.
 *
 * ISR safety: all thread-only (writes the console).
 */
#include <br/debug/br_hexdump.h>

#include <br/core/br_console.h>
#include <br/core/br_log.h>

#include "hexdump_internal.h"    /* Line format constants (see that header; single source of truth shared by selftest and production) */

/* =====================================================================
 * Line format constants (all accountable; contract pinned at compile time via _Static_assert)
 *
 * The single source of truth for `HD_ADDR_DIGITS` / `HD_LINE_BYTES` / `HD_LINE_BUF` is
 * `src/hexdump_internal.h` (selftest needs them for static buffers and length assertions); here the
 * contract is only pinned at compile time. `HD_GROUP_EXTRA_AT` is a pure rendering detail unused by selftest, so it stays in this file.
 * ===================================================================== */

#define HD_GROUP_EXTRA_AT 8u   /* widen by 1 space after group 8 */

_Static_assert(HD_LINE_BYTES == 87u, "hexdump line byte-count contract must be 87");
_Static_assert(HD_LINE_BUF <= 128u, "line buffer must stay within 128 B");

static const char HD_HEX[] = "0123456789abcdef";

/* =====================================================================
 * Rendering (no br_log_info formatting, so bytes are fully controlled)
 * ===================================================================== */

/*
 * Output sink: the logical position always advances, but bytes are only written while
 * room remains before the trailing NUL — matching snprintf truncation (pos may exceed cap; the return value still reports the needed byte count).
 */
typedef struct {
    char      *out;
    br_size_t  cap;
    br_size_t  pos;
} hd_sink_t;

static void hd_putc(hd_sink_t *s, char c)
{
    if ((s->out != BR_NULL) && ((s->pos + 1u) < s->cap)) {
        s->out[s->pos] = c;
    }
    s->pos++;
}

static void hd_hex2(hd_sink_t *s, br_u8 v)
{
    hd_putc(s, HD_HEX[(v >> 4) & 0x0fu]);
    hd_putc(s, HD_HEX[v & 0x0fu]);
}

static void hd_addr16(hd_sink_t *s, br_uintptr_t addr)
{
    for (br_u32 k = HD_ADDR_DIGITS; k > 0u; k--) {
        hd_putc(s, HD_HEX[(addr >> ((k - 1u) * 4u)) & 0x0fu]);
    }
}

/* ASCII column: 0x20..0x7E verbatim, everything else (including '\n' and '\0') becomes '.' */
static char hd_ascii(br_u8 v)
{
    return ((v >= 0x20u) && (v <= 0x7eu)) ? (char)v : '.';
}

/* Render one line; n <= BR_HEXDUMP_WIDTH. Missing groups use 3 spaces, missing ASCII uses spaces. */
static void hd_render_line(hd_sink_t *s, br_uintptr_t addr,
                           const br_u8 *p, br_size_t n)
{
    hd_addr16(s, addr);
    hd_putc(s, ' ');
    hd_putc(s, ' ');

    for (br_u32 i = 0u; i < BR_HEXDUMP_WIDTH; i++) {
        if ((br_size_t)i < n) {
            hd_hex2(s, p[i]);
        } else {
            hd_putc(s, ' ');
            hd_putc(s, ' ');
        }
        hd_putc(s, ' ');                              /* group separator */
        if (i == (HD_GROUP_EXTRA_AT - 1u)) {
            hd_putc(s, ' ');                          /* widen after group 8 */
        }
    }

    hd_putc(s, ' ');                                  /* end of group area: explicit 1 space */
    hd_putc(s, '|');
    for (br_u32 i = 0u; i < BR_HEXDUMP_WIDTH; i++) {
        hd_putc(s, ((br_size_t)i < n) ? hd_ascii(p[i]) : ' ');
    }
    hd_putc(s, '|');
    hd_putc(s, '\n');
}

/* Needed byte count (excluding the trailing NUL) */
static br_size_t hd_needed(br_size_t len)
{
    return ((len + ((br_size_t)BR_HEXDUMP_WIDTH - 1u)) / (br_size_t)BR_HEXDUMP_WIDTH)
           * (br_size_t)HD_LINE_BYTES;
}

/* =====================================================================
 * Public surface
 * ===================================================================== */

br_size_t br_hexdump_to(char *out, br_size_t cap, const void *base, br_size_t len)
{
    /* NULL base with nonzero length => no address to render (honestly report 0, do not invent an address) */
    if ((base == BR_NULL) && (len > 0u)) {
        return 0u;
    }

    const br_size_t need = hd_needed(len);

    /* Two-phase call: report the length only */
    if ((out == BR_NULL) || (cap == 0u)) {
        return need;
    }

    hd_sink_t s;
    s.out = out;
    s.cap = cap;
    s.pos = 0u;

    const br_u8 *p = (const br_u8 *)base;
    for (br_size_t off = 0u; off < len; off += (br_size_t)BR_HEXDUMP_WIDTH) {
        br_size_t n = len - off;
        if (n > (br_size_t)BR_HEXDUMP_WIDTH) {
            n = (br_size_t)BR_HEXDUMP_WIDTH;
        }
        hd_render_line(&s, (br_uintptr_t)base + off, p + off, n);
    }

    /* Guarantee NUL termination (cap > 0): untruncated => at need; truncated => at cap-1 */
    out[(s.pos < cap) ? s.pos : (cap - 1u)] = '\0';
    return need;
}

br_size_t br_hexdump(const void *base, br_size_t len)
{
    if ((base == BR_NULL) && (len > 0u)) {
        return 0u;
    }

    char      line[HD_LINE_BUF];   /* 88 B: render line by line, no whole-range buffering */
    br_size_t total = 0u;

    const br_u8 *p = (const br_u8 *)base;
    for (br_size_t off = 0u; off < len; off += (br_size_t)BR_HEXDUMP_WIDTH) {
        br_size_t n = len - off;
        if (n > (br_size_t)BR_HEXDUMP_WIDTH) {
            n = (br_size_t)BR_HEXDUMP_WIDTH;
        }

        hd_sink_t s;
        s.out = line;
        s.cap = (br_size_t)HD_LINE_BUF;
        s.pos = 0u;
        hd_render_line(&s, (br_uintptr_t)base + off, p + off, n);

        /* Use br_console_write: the console layer handles the terminal-friendly '\n' -> "\r\n" conversion */
        br_console_write(line, s.pos);
        total += s.pos;
    }

    return total;
}

br_size_t br_hexdump_line_bytes(void)
{
    return (br_size_t)HD_LINE_BYTES;
}

int br_hexdump_init(void)
{
    /* This plugin is stateless. It does not register trace event names across plugins: the declaration
     * surface has no service/hexdump -> service/trace [[dep]] edge (noted in br_hexdump.h's
     * init comment); a cross-plugin call would introduce an undeclared dependency. */
    return 0;
}

/* =====================================================================
 * Plugin lifecycle hooks (name = symbol_prefix(short) + phase; referenced by the generated
 * `build/gen/service/hexdump/plugin_desc.c`; design 1-01 §9 / 3-05 §2)
 * Prototypes here satisfy -Wmissing-prototypes (hook names are derived by the generator, not in public headers or [[export]]).
 * ===================================================================== */
int hexdump_early_init(void);
int hexdump_init(void);
int hexdump_start(void);

/* EARLY phase: no action (this plugin is stateless and registers nothing). */
int hexdump_early_init(void)
{
    return 0;
}

/* LATE phase (Service category => completion point ②): forwards to the existing init. */
int hexdump_init(void)
{
    return br_hexdump_init();
}

/* START phase: this service has nothing to do after "interrupts usable / threads creatable". */
int hexdump_start(void)
{
    return 0;
}

