/*
 * Self-test suite for provider/hexdump: its own cases TC-DBG-020/021, driven only through the public
 * API of br/debug/br_hexdump.h, except for the format constants taken from src/hexdump_internal.h.
 *
 * The cases are deliberately written apart from the implementation: the address field is rendered
 * by this file's hd_tc_addr16 with a local digit table, and the sample suffixes are independent
 * strings copied from the format contract -- sharing them would make a wrong implementation and a
 * wrong case look green together.
 *
 * Returns the failure count (0 = all green) and logs [DBGCONF] PASS/FAIL itself; a failure does not
 * halt. The entry name hexdump_selftest is derived by the generated code from symbol_prefix.
 */
#include <br/core/br_types.h>
#include <br/core/br_log.h>
#include <br/debug/br_hexdump.h>

#include "hexdump_internal.h"

int hexdump_selftest(void);

/* Case-local digit table, independent of the implementation's HD_HEX. */
static const char HD_TC_HEX[] = "0123456789abcdef";

/* Space fragments built from named constants to avoid miscounting runs of spaces: HD_SP5 is 5,
 * HD_SP8 is 8 spaces. */
#define HD_SP5 "     "
#define HD_SP8 "        "
_Static_assert(sizeof(HD_SP5) == 6u, "HD_SP5 must be 5 spaces + NUL");
_Static_assert(sizeof(HD_SP8) == 9u, "HD_SP8 must be 8 spaces + NUL");

/* The 71 bytes of the header sample (base 0x40088000, "Hello, world!\n\0\0") after the address
 * field: the case renders the address field from the payload's real address and appends this suffix
 * byte for byte. */
static const char HD_SAMPLE_SUFFIX[] =
    "  48 65 6c 6c 6f 2c 20 77"   /* 2 spaces after address + groups 1..8 (widening slot after group 8) */
    "  6f 72 6c 64 21 0a 00 00"   /* groups 9..16 */
    "  |Hello, world!...|\n";     /* group end separator + explicit space + '|' + ASCII + '|' + '\n' */
_Static_assert(sizeof(HD_SAMPLE_SUFFIX) == 72u, "sample suffix must be 71 bytes + NUL");

/* 71-byte suffix of the 3-byte tail line {'A',0x1f,0x7f}: 13 missing groups padded with 3 spaces, ASCII column padded */
static const char HD_TAIL_SUFFIX3[] =
    "  "                             /* 2 spaces after the address */
    "41 1f 7f "                      /* 3 real hex groups */
    HD_SP5 HD_SP5 HD_SP5             /* groups 4..8 placeholders: 5 x 3 = 15 spaces */
    " "                              /* widening space after group 8 */
    HD_SP8 HD_SP8 HD_SP8             /* groups 9..16 placeholders: 8 x 3 = 24 spaces */
    " |"                             /* group end separator + explicit space + '|' */
    "A.." HD_SP5 HD_SP8              /* ASCII: 'A' + '.' + '.' + 13 spaces */
    "|\n";
_Static_assert(sizeof(HD_TAIL_SUFFIX3) == 72u, "3-byte tail line suffix must be 71 bytes + NUL");

/* Record one DBGCONF criterion; returns 0/1 so failures can be summed. */
static br_u32 hd_conf(br_bool ok, const char *tag, const char *what)
{
    br_log_info("[DBGCONF] %s %s %s", ok ? "PASS" : "FAIL", tag, what);
    return ok ? 0u : 1u;
}

/* Case-local address rendering (written apart from the implementation; both must agree). */
static void hd_tc_addr16(char *dst, br_uintptr_t v)
{
    for (br_u32 i = 0u; i < HD_ADDR_DIGITS; i++) {
        const br_u32 shift = (HD_ADDR_DIGITS - 1u - i) * 4u;
        dst[i] = HD_TC_HEX[(v >> shift) & 0x0fu];
    }
}

static br_bool hd_tc_eq(const char *a, const char *b, br_size_t n)
{
    for (br_size_t i = 0u; i < n; i++) {
        if (a[i] != b[i]) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

static br_size_t hd_tc_len(const char *s)
{
    br_size_t n = 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int hexdump_selftest(void)
{
    static const br_u8 payload[16] = {
        (br_u8)'H', (br_u8)'e', (br_u8)'l', (br_u8)'l', (br_u8)'o', (br_u8)',',
        (br_u8)' ', (br_u8)'w', (br_u8)'o', (br_u8)'r', (br_u8)'l', (br_u8)'d',
        (br_u8)'!', (br_u8)'\n', 0u, 0u
    };
    static const br_u8 tail3[3] = { (br_u8)'A', 0x1fu, 0x7fu };

    static char got[HD_LINE_BUF];
    static char expect[HD_LINE_BUF];
    br_u32 fails = 0u;

    /* TC-DBG-020: 16-byte line + 3-byte tail line, compared byte by byte. The sample base is only a
     * format example (the .text.boot entry is not writable and its contents cannot be assumed), so
     * the payload lives in a static buffer and the address field is rendered from its real address,
     * while the other 71 bytes are byte-identical to the header sample's suffix. */
    hd_tc_addr16(expect, (br_uintptr_t)(const void *)payload);
    for (br_size_t i = 0u; i < (sizeof(HD_SAMPLE_SUFFIX) - 1u); i++) {
        expect[HD_ADDR_DIGITS + i] = HD_SAMPLE_SUFFIX[i];
    }

    const br_size_t n16 = br_hexdump_to(got, sizeof got, payload, 16u);
    br_bool ok20 = (n16 == (br_size_t)HD_LINE_BYTES)
                   && hd_tc_eq(got, expect, (br_size_t)HD_LINE_BYTES);

    hd_tc_addr16(expect, (br_uintptr_t)(const void *)tail3);
    for (br_size_t i = 0u; i < (sizeof(HD_TAIL_SUFFIX3) - 1u); i++) {
        expect[HD_ADDR_DIGITS + i] = HD_TAIL_SUFFIX3[i];
    }

    const br_size_t n3 = br_hexdump_to(got, sizeof got, tail3, 3u);
    ok20 = ok20 && (n3 == (br_size_t)HD_LINE_BYTES)
           && hd_tc_eq(got, expect, (br_size_t)HD_LINE_BYTES);

    fails += hd_conf(ok20, "TC-DBG-020",
                     "16B sample line + 3B tail line byte-for-byte (padding and ASCII column)");

    /* TC-DBG-021: snprintf-style truncation semantics + line byte count contract */
    static char small[10];
    const br_size_t need       = br_hexdump_to(small, sizeof small, payload, 16u);
    const br_size_t need_null  = br_hexdump_to(BR_NULL, 0u, payload, 16u);
    const br_size_t need_cons  = br_hexdump(payload, 16u);   /* also leaves evidence for the console path */

    const br_bool ok21 = (need == (br_size_t)HD_LINE_BYTES)
                         && (small[9] == '\0') && (hd_tc_len(small) == 9u)
                         && (need_null == (br_size_t)HD_LINE_BYTES)
                         && (need_cons == (br_size_t)HD_LINE_BYTES)
                         && (br_hexdump_line_bytes() == (br_size_t)HD_LINE_BYTES)
                         && (br_hexdump_to(small, 0u, payload, 16u) == (br_size_t)HD_LINE_BYTES)
                         && (br_hexdump_to(small, sizeof small, BR_NULL, 16u) == 0u)
                         && (br_hexdump_to(small, sizeof small, payload, 0u) == 0u);

    fails += hd_conf(ok21, "TC-DBG-021",
                     "cap truncation + NUL termination + length-only report + line_bytes=87");

    return (int)fails;
}
