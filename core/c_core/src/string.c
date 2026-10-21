/*
 * Compiler support routines (memcpy/memmove/memset/memcmp). Not an optional accelerator:
 * -ffreestanding -fno-builtin disables builtin recognition, not GCC's lowering of whole-block
 * moves into mem* libcalls, so this file is a precondition for linking at all.
 * Core owns them because it needs memset in EARLY (heap bring-up, before the MMU is on) while
 * runtime/posix is a LATE service; the exported set is deliberately minimal.
 * Pre-MMU memory is Device-nGnRnE, where unaligned wide access raises an Alignment fault: use
 * wide access only when the pointer is 8-byte aligned, byte by byte otherwise.
 */
#include "string_internal.h"

#if defined(__clang__)
/*
 * Keep the optimizer from turning this function into a call to memcpy
 * (loop-distribute-patterns does that at -O2: infinite recursion plus a link-time
 * self-reference). `no_builtin` is clang's form; clang defines __GNUC__ too, so this branch
 * must come first.
 */
#  define BR_NO_LOOP_PATTERN __attribute__((no_builtin("memcpy", "memmove", "memset")))
#elif defined(__GNUC__)
/* GCC form of the same request (clang rejects the `optimize` attribute outright). */
#  define BR_NO_LOOP_PATTERN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#  define BR_NO_LOOP_PATTERN
#endif

static br_bool is_word_aligned(const void *p)
{
    return (((br_uintptr_t)p & 7u) == 0u) ? BR_TRUE : BR_FALSE;
}

/* Never write the 8-byte move as __builtin_memcpy(..., 8): under this file's compile combination
 * GCC lowers that fixed-size-8 builtin into a memcpy libcall, so memcpy contains a memcpy call and
 * recurses forever for n >= 8 with aligned pointers, punching through the boot stack.
 * Use explicit wide access instead: aligned(1) (alignment is already guaranteed by
 * is_word_aligned) and may_alias (avoids strict-aliasing UB); neither emits a call.
 * Acceptance: objdump -d on this .o shows no `bl memcpy` inside memcpy. */
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

    /* overlapping and dst higher: copy tail to head byte by byte (d, s are byte views) */
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
            *(br_word_u *)(void *)(d + i) = w;   /* explicit wide access (no __builtin_memcpy) */
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
