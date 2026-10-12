/*
 * service/hexdump — hex + ASCII rendering primitives (public declaration surface)
 *
 * Design source: `docs/5-debug/5-01-debug.md` §2 (debug bridge `MEMRD`, host-side
 * `brickie dbg`) and §3 (ramdump output). This plugin is the **lowest-level rendering
 * primitive**: it turns memory into lines, shared by dump / bridge / panic reports; it does
 * **no bounds checking** (that is service/dump's job — whoever reads memory must consult the
 * region table).
 *
 * ★ The format is a **contract** (case TC-DBG-020 asserts it byte by byte), so it is fixed here:
 *
 *   one line per 16 bytes, line = <addr:16 lowercase hex digits> + two spaces
 *                      + 16 groups of `<2 hex digits>` (1 space between groups; **one extra
 *                        space** after group 8)
 *                      + two spaces + '|' + 16 ASCII chars + '|' + '\n'
 *   trailing line shorter than 16 bytes: missing groups use **3 spaces** (keeps the '|' column
 *   aligned);
 *   ASCII column: 0x20..0x7E verbatim, everything else '.' — including '\n'/'\0'.
 *
 * Full example (16-byte input "Hello, world!\n\0\0" at base 0x40088000 in a static payload):
 *
 *   0000000040088000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 0a 00 00  |Hello, world!...|
 *
 * ⚠ The base in the example is only a **format** illustration (the address field is always 16
 *   lowercase hex digits); case `TC-DBG-020` renders the address field from the **real address**
 *   of the payload under test and compares the rest byte by byte — on target one cannot read 16
 *   bytes at the .text.boot entry 0x40080000 (not writable there, contents unknown).
 *
 * Return semantics follow snprintf: `br_hexdump_to` returns the number of bytes **needed**
 * (excluding the trailing NUL), and truncation does not affect it — so callers can detect
 * truncation with `ret >= cap` (case TC-DBG-021).
 */
#ifndef BR_DEBUG_BR_HEXDUMP_H
#define BR_DEBUG_BR_HEXDUMP_H

#include <br/core/br_types.h>

#define BR_HEXDUMP_WIDTH   16u   /* bytes per line (contract; format in the file header) */

/* LATE-phase init: this plugin is stateless, it only resets internal state and returns 0.
 * (No cross-plugin trace event-name registration: the declaration surface has no
 *  backtrace/hexdump → service/trace `[[dep]]` edge, see this plugin's README "declaration
 *  surface debt".) */
int br_hexdump_init(void);

/* The self-test entry (TC-DBG-02x) moved to `src/hexdump_selftest.c` (ADR-0010): the generated
 * artifact emits `.selftest = hexdump_selftest` per symbol_prefix, and core drives all of them
 * after start.
 * ★ Test entries do **not** enter this plugin's golden interface surface — otherwise editing a
 *   case would become a public interface change (the interface hash covers exactly the
 *   declaration surface), conflicting with "self-tests separate from production". */

/*
 * Render into a buffer: write at most `cap` bytes (including the trailing NUL). Return the
 * number of bytes **needed** (excluding NUL).
 *   cap == 0 or out == NULL ⇒ only report the needed length (for two-pass calls).
 */
br_size_t br_hexdump_to(char *out, br_size_t cap, const void *base, br_size_t len);

/* Render to console (putc per line; no allocation, no whole-buffer staging). Returns the
 * number of bytes rendered. */
br_size_t br_hexdump(const void *base, br_size_t len);

/* Bytes in one line (= return value of one `br_hexdump_to` when len<=16, excluding NUL);
 * lets dump callers budget buffers. */
br_size_t br_hexdump_line_bytes(void);

#endif /* BR_DEBUG_BR_HEXDUMP_H */
