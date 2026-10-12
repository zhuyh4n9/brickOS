/*
 * brickOS prototype v0.1.0 — compiler support routines (memcpy/memmove/memset/memcmp)
 *
 * ★ This is not "implementing libc" but a **hard obligation of freestanding code generation**:
 *   even with `-ffreestanding -fno-builtin`, GCC's **middle/back end** still lowers
 *   "whole-block moves" (struct assignment, struct zeroing, large array init) into calls to
 *   `memcpy/memmove/memset` — `-fno-builtin` disables **builtin recognition**, not lowering.
 *   Hit once for real: a struct assignment in `br_mem_layout()` was lowered to `bl memcpy`,
 *   so the `-nostdlib` link failed at once (`undefined reference to memcpy`).
 *   So this file is a **precondition for linking at all**, not an optional accelerator.
 *
 * Ownership (why core and not runtime/posix):
 *   By design the POSIX symbol surface belongs to the POSIX runtime (D18; in this tree the
 *   `runtime/posix` plugin, ADR-0015). ★ **This file will not disappear when that plugin
 *   lands** — the reason is **phase**, not scheduling: core needs `memset` for control blocks
 *   already in EARLY (heap bring-up, before the MMU is on), while `runtime/posix` is a
 *   LATE-phase service. Symbol unification (moving mem* into that plugin too) is impossible
 *   given the phase fact that core precedes services — recorded as debt in ADR-0014 §4, not an
 *   oversight.
 *   This file is therefore deliberately **minimal**: only the four the compiler emits, with
 *   signatures strictly per AAPCS64/standard C, exporting no other libc name (the remaining
 *   `str*` belong to `runtime/posix`; the two sets do not overlap).
 *
 * Safety discipline (relation to `-mstrict-align`): this file is also called **before the MMU
 *   is on** (`br_mem_init` zeroes control blocks); all memory access is then Device-nGnRnE, so
 *   **unaligned wide access raises an Alignment fault**. Wide access is therefore used only
 *   when the **pointer is 8-byte aligned**, byte by byte otherwise — this is not an
 *   optimization tradeoff but a requirement to be correct under both memory models.
 */
#include "string_internal.h"

#if defined(__GNUC__)
/*
 * Prevent the optimizer from turning this function itself into a call to memcpy
 * (loop-distribute-patterns does that at -O2, which is infinite recursion + a link-time
 * self-reference). GCC-specific attribute, hence wrapped in __GNUC__.
 */
#  define BR_NO_LOOP_PATTERN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#  define BR_NO_LOOP_PATTERN
#endif

static br_bool is_word_aligned(const void *p)
{
    return (((br_uintptr_t)p & 7u) == 0u) ? BR_TRUE : BR_FALSE;
}

/*
 * ★ **The 8-byte move must never be written as `__builtin_memcpy(..., 8)`** (hit for real,
 *   ADR-0003 §5): under this file's compile combination (`-ffreestanding -fno-builtin` +
 *   `optimize("no-tree-loop-distribute-patterns")` on the function), GCC **lowers** that
 *   fixed-size-8 `__builtin_memcpy` into a libcall to `memcpy` — so memcpy contains
 *   `bl memcpy`, and whenever n ≥ 8 and the pointers are aligned it **recurses forever**,
 *   growing the stack without bound, punching through the boot stack into the .bss page
 *   tables, showing as "translation fault at boot".
 *   Use **explicit wide access** instead: `aligned(1)` stops the compiler assuming alignment
 *   (alignment is already guaranteed by `is_word_aligned`), `may_alias` avoids strict-aliasing
 *   UB; neither emits a call.
 *   Acceptance criterion = `objdump -d <this .o> | grep -A40 '<memcpy>:' | grep -c 'bl.*memcpy'`
 *   must be 0 (see the verification record).
 */
typedef br_u64 br_word_u __attribute__((aligned(1), may_alias));

BR_NO_LOOP_PATTERN void *memcpy(void *dst, const void *src, br_size_t n)
{
    br_u8       *d = (br_u8 *)dst;
    const br_u8 *s = (const br_u8 *)src;
    br_size_t    i = 0u;

    if (is_word_aligned(dst) && is_word_aligned(src)) {
        while ((i + 8u) <= n) {
            const br_word_u w = *(const br_word_u *)(const void *)(s + i);
            *(br_word_u *)(void *)(d + i) = w;
            i += 8u;
        }
    }
    while (i < n) {
        d[i] = s[i];
        i++;
    }
    return dst;
}

BR_NO_LOOP_PATTERN void *memmove(void *dst, const void *src, br_size_t n)
{
    br_u8       *d = (br_u8 *)dst;
    const br_u8 *s = (const br_u8 *)src;

    /* overlapping and dst higher ⇒ copy tail to head byte by byte (d, s are byte views, compare is well defined) */
    if ((d > s) && (d < (s + n))) {
        br_size_t i = n;
        while (i > 0u) {
            i--;
            d[i] = s[i];
        }
        return dst;
    }
    return memcpy(dst, src, n);
}

BR_NO_LOOP_PATTERN void *memset(void *dst, int c, br_size_t n)
{
    br_u8    *d = (br_u8 *)dst;
    br_size_t i = 0u;
    const br_u8 b = (br_u8)c;

    if (is_word_aligned(dst)) {
        br_u64 w = 0u;
        for (br_u32 k = 0u; k < 8u; k++) {
            w |= ((br_u64)b) << (8u * k);
        }
        while ((i + 8u) <= n) {
            *(br_word_u *)(void *)(d + i) = w;   /* explicit wide access (no __builtin_memcpy, see above) */
            i += 8u;
        }
    }
    while (i < n) {
        d[i] = b;
        i++;
    }
    return dst;
}

int memcmp(const void *a, const void *b, br_size_t n)
{
    const br_u8 *x = (const br_u8 *)a;
    const br_u8 *y = (const br_u8 *)b;

    for (br_size_t i = 0u; i < n; i++) {
        if (x[i] != y[i]) {
            return (x[i] < y[i]) ? -1 : 1;
        }
    }
    return 0;
}
