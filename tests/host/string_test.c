/*
 * brickOS prototype v0.1.0 — **host** tests for compiler support routines
 *
 * Why these four functions deserve a dedicated host test:
 *   They are the product of `core/src/string.c`, part of the foundation the compiler itself emits calls to —
 *   once wrong, **symptoms show up in completely unrelated places** (measured: `bl memcpy` self-recursion inside
 *   memcpy ⇒ unbounded recursion eats through the boot stack ⇒ exception frames land in the page table ⇒ the surface
 *   symptom is "translation fault right at boot after the MMU is on", taking three layers to diagnose, see ADR-0003 §5.2).
 *   The host side can verify them directly: semantics can be compared byte by byte against a reference implementation,
 *   out-of-bounds writes are caught with sentinel bytes, and **self-recursion blows the stack immediately on the host**
 *   (so this case is itself the regression criterion for that bug).
 *
 * Division of labor with the `check-string` gate: the gate looks at **disassembly** (is there a `bl` to itself?),
 * this case looks at **semantics** (copied right? out of bounds?) — one catches "structural self-recursion", the other "incorrect behavior".
 */
#include <br/core/br_types.h>

#include "string_internal.h"

#include <stdio.h>
#include <string.h>

static int s_pass;
static int s_fail;

static void rep(int cond, const char *tag, const char *desc)
{
    if (cond) {
        s_pass++;
        printf("[STRINGTEST] PASS %s %s\n", tag, desc);
    } else {
        s_fail++;
        printf("[STRINGTEST] FAIL %s %s\n", tag, desc);
    }
}

/*
 * Buffer layout: [leading sentinel PAD][payload BUF][trailing sentinel PAD].
 * Sentinels on both sides, so even a 1-byte overflow is caught (same origin as this project's red-zone requirement).
 */
#define PAD 16u
#define BUF 2048u
#define TOTAL (PAD + BUF + PAD)

static br_u8 s_src[TOTAL];
static br_u8 s_dst[TOTAL];
static br_u8 s_ref[TOTAL];

static void fill(br_u8 *b, br_size_t n, br_u32 seed)
{
    br_u32 x = seed;
    for (br_size_t i = 0; i < n; i++) {
        x = (x * 1103515245u) + 12345u;
        b[i] = (br_u8)(x >> 16);
    }
}

/* Sentinels: the first PAD bytes are set to 0xC3 and the PAD bytes starting at `end` to 0x3C.
 * `end` = payload end offset (relative to the buffer head), given by the caller as "where writes really land". */
static void pad_set(br_u8 *b, br_size_t end)
{
    for (br_size_t i = 0; i < PAD; i++) {
        b[i] = 0xC3;
        b[end + i] = 0x3C;
    }
}

static int pad_ok(const br_u8 *b, br_size_t end)
{
    for (br_size_t i = 0; i < PAD; i++) {
        if (b[i] != 0xC3 || b[end + i] != 0x3C) {
            return 0;
        }
    }
    return 1;
}

/* covers 0/1/7/8/17/63/64/257/1024: the small odd ones take the byte path, multiples of 8 the wide-access path */
static const br_size_t k_sizes[] = { 0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 63u, 64u, 257u, 1024u };
#define NSIZES (sizeof k_sizes / sizeof k_sizes[0])

static void case_memcpy(void)
{
    br_u8 *src = s_src + PAD;
    br_u8 *dst = s_dst + PAD;
    int ok = 1;

    fill(s_src, sizeof s_src, 0x1234u);

    for (br_size_t k = 0; k < NSIZES; k++) {
        const br_size_t n = k_sizes[k];

        /* ① both aligned (wide-access path): contents + return value + both sentinels */
        memset(s_dst, 0, sizeof s_dst);
        pad_set(s_dst, PAD + n);
        void *r = memcpy(dst, src, n);
        ok = ok && (r == (void *)dst);
        ok = ok && (memcmp(dst, src, n) == 0);
        ok = ok && pad_ok(s_dst, PAD + n);

        /* ② destination not 8-aligned (forces the byte path); sentinels set by "where writes really land" */
        for (br_size_t off = 1u; off <= 5u; off += 2u) {
            memset(s_dst, 0, sizeof s_dst);
            pad_set(s_dst, PAD + off + n);
            (void)memcpy(dst + off, src, n);
            ok = ok && (memcmp(dst + off, src, n) == 0);
            ok = ok && pad_ok(s_dst, PAD + off + n);
        }

        /* ③ source not 8-aligned (destination still aligned) */
        memset(s_dst, 0, sizeof s_dst);
        pad_set(s_dst, PAD + n);
        (void)memcpy(dst, src + 3u, n);
        ok = ok && (memcmp(dst, src + 3u, n) == 0);
        ok = ok && pad_ok(s_dst, PAD + n);

        /* ④ compare against the reference implementation (separate buffer + byte-by-byte reference copy): redo a clean aligned copy */
        memset(s_ref, 0, sizeof s_ref);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[PAD + i] = src[i];
        }
        memset(s_dst, 0, sizeof s_dst);
        (void)memcpy(dst, src, n);
        ok = ok && (memcmp(dst, s_ref + PAD, n) == 0);
        ok = ok && (memcmp(s_dst, s_ref, PAD + n) == 0);
    }
    rep(ok, "STR-001", "memcpy 12 sizes x aligned/unaligned: contents/return value/both sentinels all correct");

    /* bulk (whole payload) transfer: may only write [PAD, PAD+BUF) */
    memset(s_dst, 0, sizeof s_dst);
    pad_set(s_dst, PAD + BUF);
    (void)memcpy(dst, src, BUF);
    rep(pad_ok(s_dst, PAD + BUF) && (memcmp(dst, src, BUF) == 0), "STR-002",
        "memcpy full 2 KiB transfer: contents match and no overflow");
}

static void case_memmove(void)
{
    br_u8 *base = s_dst + PAD;
    int ok = 1;

    for (br_size_t k = 0; k < NSIZES; k++) {
        const br_size_t n = k_sizes[k];
        if (n == 0u) {
            continue;
        }

        /* ① destination below source (forward overlap) */
        fill(s_dst, sizeof s_dst, 0xABCDu);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[i] = base[64 + i];
        }
        (void)memmove(base, base + 64, n);
        ok = ok && (memcmp(base, s_ref, n) == 0);

        /* ② destination above source (backward overlap: must copy tail-to-head, else self-overwritten) */
        fill(s_dst, sizeof s_dst, 0x5678u);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[i] = base[i];
        }
        (void)memmove(base + 64, base, n);
        ok = ok && (memcmp(base + 64, s_ref, n) == 0);

        /* ③ partial overlap: 1-byte offset */
        fill(s_dst, sizeof s_dst, 0x9999u);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[i] = base[i];
        }
        (void)memmove(base + 1, base, n);
        ok = ok && (memcmp(base + 1, s_ref, n) == 0);

        /* ④ no overlap: equivalent to a copy (source in a separate buffer) */
        fill(s_dst, sizeof s_dst, 0x2468u);
        (void)memmove(base, s_src + PAD, n);
        ok = ok && (memcmp(base, s_src + PAD, n) == 0);

        /* ⑤ n == 0 must not touch any byte */
        if (n != 0u) {
            continue;
        }
    }

    /* ⑥ overlap edge case: destination = source + 1, backward; compared against the reference */
    for (br_size_t k = 0; k < NSIZES; k++) {
        const br_size_t n = k_sizes[k];
        if (n == 0u) {
            continue;
        }
        fill(s_dst, sizeof s_dst, 0x1357u);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[i] = base[i];
        }
        (void)memmove(base + 1, base, n);      /* destination higher: must copy backward */
        ok = ok && (memcmp(base + 1, s_ref, n) == 0);
    }
    rep(ok, "STR-003", "memmove three overlap directions + offset + no overlap: byte-identical to reference");

    /* zero length: not one byte of the buffer may change */
    fill(s_dst, sizeof s_dst, 0x7777u);
    memset(s_ref, 0, sizeof s_ref);
    memcpy(s_ref, s_dst, sizeof s_dst);
    (void)memmove(base, base + 32, 0u);
    rep(memcmp(s_dst, s_ref, sizeof s_dst) == 0, "STR-004",
        "memmove(n=0) writes no bytes");
}

static void case_memset(void)
{
    br_u8 *dst = s_dst + PAD;
    const br_u8 pats[] = { 0x00u, 0x5Au, 0xA5u, 0xFFu, 0x01u };
    int ok = 1;

    for (br_size_t p = 0; p < sizeof pats / sizeof pats[0]; p++) {
        for (br_size_t k = 0; k < NSIZES; k++) {
            const br_size_t n = k_sizes[k];

            /* ① aligned destination: sentinels + byte-by-byte comparison against reference */
            memset(s_dst, 0, sizeof s_dst);
            pad_set(s_dst, PAD + n);
            void *r = memset(dst, pats[p], n);
            ok = ok && (r == (void *)dst);
            ok = ok && pad_ok(s_dst, PAD + n);
            for (br_size_t i = 0; i < n; i++) {
                ok = ok && (dst[i] == pats[p]);
            }

            /* ② destination not 8-aligned: also byte-exact and no overflow */
            memset(s_dst, 0, sizeof s_dst);
            pad_set(s_dst, PAD + 3u + n);
            (void)memset(dst + 3, pats[p], n);
            ok = ok && pad_ok(s_dst, PAD + 3u + n);
            for (br_size_t i = 0; i < n; i++) {
                ok = ok && (dst[3 + i] == pats[p]);
            }
        }
    }
    rep(ok, "STR-005", "memset 5 patterns x 12 sizes x aligned/unaligned: byte-exact and no overflow");
}

static void case_memcmp(void)
{
    static br_u8 a[64];
    static br_u8 b[64];
    int ok = 1;

    fill(a, sizeof a, 0x1111u);
    memcpy(b, a, sizeof a);
    ok = ok && (memcmp(a, b, sizeof a) == 0);
    ok = ok && (memcmp(a, b, 0u) == 0);                 /* n == 0 ⇒ always equal */
    ok = ok && (memcmp(a, a, sizeof a) == 0);

    b[0] ^= 0x01u;
    ok = ok && (memcmp(a, b, sizeof a) != 0);
    ok = ok && (memcmp(b, a, sizeof a) != 0);
    ok = ok && ((memcmp(a, b, sizeof a) < 0) != (memcmp(b, a, sizeof a) < 0));  /* antisymmetric */

    memcpy(b, a, sizeof a);
    b[63] ^= 0x80u;                                     /* differs only in the last byte */
    ok = ok && (memcmp(a, b, 64u) != 0);
    ok = ok && (memcmp(a, b, 63u) == 0);                /* common prefix ⇒ only n matters */

    rep(ok, "STR-006", "memcmp equal/unequal/prefix/antisymmetric/n=0 semantics correct");
}

int main(void)
{
    printf("[STRINGTEST] compiler support routines (core/src/string.c) on host\n");

    case_memcpy();
    case_memmove();
    case_memset();
    case_memcmp();

    printf("[STRINGTEST] SUMMARY pass=%d fail=%d total=%d\n", s_pass, s_fail, s_pass + s_fail);
    return (s_fail == 0) ? 0 : 1;
}
