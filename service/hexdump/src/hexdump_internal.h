/*
 * brickOS prototype v0.2.0 — service/hexdump **plugin-private** internal contract (not exported, not in include/)
 *
 * Audience: this plugin's own `src/hexdump_selftest.c`. Living in `src/` (rather than `include/`) is deliberate
 * -- everything under `include/` is the **plugin's external surface** (governed by `brickie check` interface
 * domains, and part of the `descriptor_include` selection scope); the three things here are needed **only by
 * selftest and the production implementation**.
 *
 * ## Why selftest reads internals
 *   The criterion of TC-DBG-020/021 is **byte-for-byte comparison of the whole line**, so the case needs its own
 *   line buffer and "expected length":
 *     - `HD_LINE_BUF` is the line buffer capacity (the case's got/expect static arrays are sized by it);
 *     - `HD_LINE_BYTES` is the machine-readable form of the **format contract** "a full line is 87 bytes"; the
 *       case uses it for the length assertion (the library's `br_hexdump_line_bytes()` is a run-time value and
 *       cannot size a static array);
 *     - `HD_ADDR_DIGITS` is the field width used when the case renders the address field **itself** (deliberately
 *       written apart from the implementation; the two must agree, see hd_tc_addr16 in hexdump_selftest.c).
 *   Moving these three into the public header would be worse: format constants in the golden interface surface
 *   would make any later line-width change an interface change.
 *
 * ## Boundary discipline (same origin as `src/vfs_internal.h` in framework/vfs-core)
 *   Expose only the three **constants** selftest actually uses (moved from the production implementation's private
 *   macros, values unchanged), and no mutable state; all line rendering logic stays `static`. The case does not
 *   share the implementation's digit table -- it carries its own, otherwise a wrong table in the implementation
 *   would make the case wrong too.
 */
#ifndef BR_DEBUG_BR_HEXDUMP_INTERNAL_H
#define BR_DEBUG_BR_HEXDUMP_INTERNAL_H

#include <br/core/br_types.h>
#include <br/debug/br_hexdump.h>

/* Address field is fixed 16-digit lowercase hex (the production `HD_ADDR_DIGITS`) */
#define HD_ADDR_DIGITS   16u

/* Full line bytes = 16 + 2 + 49 + 1 + 1 + 16 + 1 + 1 = 87 (including the trailing '\n'); see the
 * format contract in br_hexdump.h. Outside this header the production implementation has one more
 * `_Static_assert(HD_LINE_BYTES == 87u, ...)` pinning the number at compile time. */
#define HD_LINE_BYTES    (HD_ADDR_DIGITS + 2u                                   \
                          + (BR_HEXDUMP_WIDTH * 3u + 1u)                        \
                          + 1u + 1u + BR_HEXDUMP_WIDTH + 1u + 1u)

/* Capacity of the one-line render buffer (87 + NUL slack); the case sizes its static buffers by it */
#define HD_LINE_BUF      (HD_LINE_BYTES + 1u)

#endif /* BR_DEBUG_BR_HEXDUMP_INTERNAL_H */
