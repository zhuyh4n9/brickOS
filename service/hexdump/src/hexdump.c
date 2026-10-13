/*
 * Hex + ASCII rendering implementation. The format contract is documented in br_hexdump.h and
 * expanded here as accountable fields: [addr 16][2 spaces][group area 49][1 space]['|'][ASCII 16]
 * ['|']['\n'] = 87 bytes. Group area 49 = 16 groups x (2 hex + 1 separator space) + 1 widened space
 * after group 8; the trailing separator plus the explicit space gives the 2 spaces before
 * '|'. Single-line stack buffer only, no allocation and no whole-range buffering.
 * No bounds checking here (that is service/dump's job). ISR safety: thread-only (writes console).
 */
#include <br/debug/br_hexdump.h>

#include <br/core/br_console.h>
#include <br/core/br_log.h>

/* Line format constants shared by the selftest and production. */
#include "hexdump_internal.h"

/* HD_ADDR_DIGITS / HD_LINE_BYTES / HD_LINE_BUF live in src/hexdump_internal.h (the selftest uses
 * them too); here the contract is only pinned at compile time. HD_GROUP_EXTRA_AT is local: widen by
 * 1 space after group 8. */
#define HD_GROUP_EXTRA_AT 8u

_Static_assert(HD_LINE_BYTES == 87u, "hexdump line byte-count contract must be 87");
_Static_assert(HD_LINE_BUF <= 128u, "line buffer must stay within 128 B");

static const char HD_HEX[] = "0123456789abcdef";

/*
 * Output sink: pos always advances, but bytes are written only while room remains before the
 * trailing NUL -- snprintf truncation (pos may exceed cap; the return reports the need).
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

static br_size_t hd_needed(br_size_t len)
{
    return ((len + ((br_size_t)BR_HEXDUMP_WIDTH - 1u)) / (br_size_t)BR_HEXDUMP_WIDTH)
           * (br_size_t)HD_LINE_BYTES;
}

br_size_t br_hexdump_to(char *out, br_size_t cap, const void *base, br_size_t len)
{
    /* NULL base with nonzero length => no address to render (report 0, do not invent one) */
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

        /* Use br_console_write: the console layer handles the '\n' -> "\r\n" conversion */
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
    /* Stateless. No cross-plugin trace event-name registration: the declaration surface has no
     * service/hexdump -> service/trace `[[dep]]` edge, so a call adds an undeclared dependency. */
    return 0;
}

/* Plugin lifecycle hooks referenced by the generated plugin_desc.c. Prototypes satisfy
 * -Wmissing-prototypes (the hook names are derived by the generator, not in public headers). */
int hexdump_early_init(void);
int hexdump_init(void);
int hexdump_start(void);

/* EARLY phase: no action (stateless, registers nothing). */
int hexdump_early_init(void)
{
    return 0;
}

int hexdump_init(void)
{
    return br_hexdump_init();
}

int hexdump_start(void)
{
    return 0;
}
