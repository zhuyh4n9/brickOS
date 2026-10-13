/*
 * service/hexdump plugin-private contract, not exported and not under include/. Audience: this
 * plugin's own src/hexdump_selftest.c and the production implementation.
 *
 * TC-DBG-020/021 compare whole lines byte by byte, so the case needs HD_LINE_BUF (sizes its static
 * buffers), HD_LINE_BYTES (the "full line is 87 bytes" contract, usable in a static assertion) and
 * HD_ADDR_DIGITS (the field width the case renders itself). Format constants in the public header
 * would make a later line-width change an interface change. No mutable state is exposed; all
 * rendering logic stays static. The case carries its own digit table, so a wrong table in the
 * implementation cannot make the case wrong too.
 */
#ifndef BR_DEBUG_BR_HEXDUMP_INTERNAL_H
#define BR_DEBUG_BR_HEXDUMP_INTERNAL_H

#include <br/core/br_types.h>
#include <br/debug/br_hexdump.h>

/* Address field is fixed 16-digit lowercase hex. */
#define HD_ADDR_DIGITS   16u

/* Full line bytes = 16 + 2 + 49 + 1 + 1 + 16 + 1 + 1 = 87, including the trailing '\n' (format
 * contract in br_hexdump.h); hexdump.c also pins the number with _Static_assert. */
#define HD_LINE_BYTES    (HD_ADDR_DIGITS + 2u                                   \
                          + (BR_HEXDUMP_WIDTH * 3u + 1u)                        \
                          + 1u + 1u + BR_HEXDUMP_WIDTH + 1u + 1u)

/* One-line render buffer capacity (87 + NUL slack); the case sizes its static buffers by it. */
#define HD_LINE_BUF      (HD_LINE_BYTES + 1u)

#endif /* BR_DEBUG_BR_HEXDUMP_INTERNAL_H */
