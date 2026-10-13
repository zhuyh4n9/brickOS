/*
 * Hex + ASCII rendering primitives (public surface): the lowest-level rendering, memory -> lines,
 * shared by dump / bridge / panic reports. Does no bounds checking; whoever reads memory must
 * consult the region table first.
 *
 * Format contract (TC-DBG-020 asserts it byte by byte): one line per 16 bytes:
 *   <addr:16 lowercase hex digits> + 2 spaces + 16 groups of <2 hex digits> (1 space between
 *   groups, one extra space after group 8) + 2 spaces + '|' + 16 ASCII chars + '|' + '\n'
 * A trailing line shorter than 16 bytes pads missing groups with 3 spaces, keeping the '|' column
 * aligned. ASCII column: 0x20..0x7E verbatim, everything else '.', including '\n'/'\0'.
 *
 * Return semantics follow snprintf: br_hexdump_to returns the bytes needed (excluding the trailing
 * NUL), truncation does not change it, so callers detect truncation with `ret >= cap` (TC-DBG-021).
 */
#ifndef BR_DEBUG_BR_HEXDUMP_H
#define BR_DEBUG_BR_HEXDUMP_H

#include <br/core/br_types.h>

/* Bytes per line (contract; format documented above). */
#define BR_HEXDUMP_WIDTH   16u

/* LATE-phase init: the plugin is stateless, so this only returns 0. */
int br_hexdump_init(void);

/* Render into out, writing at most cap bytes including the trailing NUL. Returns the bytes needed
 * (excluding NUL); cap == 0 or out == NULL reports the needed length only (two-pass calls). */
br_size_t br_hexdump_to(char *out, br_size_t cap, const void *base, br_size_t len);

/* Render to console, putc per line, no whole-buffer staging. Returns bytes rendered. */
br_size_t br_hexdump(const void *base, br_size_t len);

/* Bytes in one line (= br_hexdump_to return for len <= 16, excluding NUL); buffer budgeting. */
br_size_t br_hexdump_line_bytes(void);

#endif /* BR_DEBUG_BR_HEXDUMP_H */
