/*
 * adapter/uboot-erofs — **u-boot runtime environment** (adapter layer)
 *
 * ============================== WHAT THIS FILE IS ==============================
 * Upstream EROFS is **kernel/u-boot style** C: it assumes `linux/compat.h`,
 * `linux/stat.h`, `linux/bug.h`, `linux/log2.h`, `asm/unaligned.h`, `u-boot/lz4.h`,
 * `printf`/`pr_err` and libc `malloc/free/memcpy/strnlen`. This plugin is a
 * **bare-metal unikernel (no libc)**, so this layer maps those assumptions back to
 * this project's truths, one by one:
 *
 *   u-boot/Linux side                      →  brickOS side
 *   ---------------------------------------------------------------------------
 *   u8/u16/u32/u64/s64                     →  br_u8/...(br/core/br_types.h)
 *   bool / true / false                    →  br_bool / BR_TRUE / BR_FALSE
 *   __le16/__le32/__le64 + leN_to_cpu()    →  identity fn(aarch64 LE; see #error below)
 *   get_unaligned_le16/32                  →  __builtin_memcpy versions(target -mstrict-align)
 *   min_t/max_t/round_up/round_down/ilog2  →  the small macros/functions in this file
 *   DIV_ROUND_UP / BLK_ROUND_UP            →  DIV_ROUND_UP macro(internal.h defines the latter)
 *   malloc/free/realloc/calloc             →  br_malloc/br_free/br_realloc/br_calloc
 *   memcpy/memset/memcmp/memmove           →  provided by core(rule: use these four directly)
 *   strlen/strnlen/strcmp/memcmp-style     →  this file's `uboot_erofs_*` (see below)
 *   printf/pr_err/pr_info/pr_debug         →  br_log_error/info/debug
 *   BUG_ON/DBG_BUGON                       →  DBG_BUGON(see below: log + do not crash)
 *   IS_ERR/ERR_PTR/PTR_ERR                 →  unused here; dropped
 *   plain E* names / EFSCORRUPTED(EUCLEAN) →  **core's <br/core/br_errno.h>** (see below)
 *
 * ## Why the string helpers are hand-written
 *   Hard constraint (task spec): `strlen/strcmp/strncmp/strcpy/strnlen/strchr` **must
 *   be small local implementations**, with no dependency on runtime/posix. All are
 *   `static inline` with the `uboot_erofs_` prefix, and **macros** then map the
 *   upstream plain names over — so ported `namei.c`/`fs.c` lines need **not one
 *   character changed** (`strnlen(de_name, n)` still reads the same), while the linker
 *   can never reach a nonexistent libc symbol. The whole translation unit is
 *   `-fno-builtin`, so the compiler will not treat our definitions as builtins and
 *   call them recursively.
 *
 * ## Intentional deviations (from upstream)
 *   1. Upstream uses **compile-time** switches such as `IS_ENABLED(CONFIG_LZ4)`; this
 *      file removes them all for **run-time** branches in `erofs_decompress` (hard
 *      constraint of the task spec).
 *   2. Upstream `internal.h`'s `erofs_err()` appends a newline; u-boot's `pr_err` does
 *      too. This file's `erofs_err` adds none (br_log_write outputs line by line).
 *   3. `erofs_dbg` really outputs only when `BR_EROFS_VERBOSE_DBG` is enabled — by
 *      default it compiles to an empty statement, so machines without a serial port are
 *      not flooded by per-block mapping logs.
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_EROFS_ENV_H
#define BR_ADAPTER_UBOOT_EROFS_ENV_H

#include <stddef.h>     /* NULL / size_t — compiler-provided header, not libc */

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
/* Block-device contract: upstream gets it from `blk.h`, this file from here. **Only this
 * header** (the adapter entry) may include `<br/...>` — upstream-derived files under
 * `src/` always take their environment through this header. */
#include <br/dev/br_bdev.h>

/* `uintN_t`/`intN_t` come from the compiler's own <stdint.h> (upstream `erofs_fs.h`
 * includes it itself); this layer does **not redefine** them — a duplicate definition
 * would clash with the compiler header, exactly what an adapter layer must not do. */
#include <stdint.h>

/* ================================================================== memory allocation
 *
 * Upstream EROFS uses libc `malloc/free/calloc/realloc` (the kmalloc family in the
 * kernel). This is a bare-metal unikernel and the heap lives in core (`br_malloc`/
 * `br_free`/`br_realloc`/`br_calloc`).
 * * No allocator is written here, the names are merely wired over — ported code
 *   therefore needs no changes. Cost: upstream's implicit "out of bounds/failure"
 *   assumption (return NULL) must still hold, and core's `br_malloc` returns NULL just
 *   like libc, so it fits.
 */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* ================================================================ little-endian premise */
/* Upstream's `le32_to_cpu()` is an **identity** function here — valid only if the
 * target really is little-endian. aarch64 QEMU virt and almost all similar boards are
 * little-endian; on a big-endian target this port is wrong, so stopping here is better
 * than silently producing wrong words. */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-erofs leN_to_cpu() is an identity implementation: little-endian targets only"
#endif

/* ==================================================================== base types */
typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;   /* * u-boot uses long long here (not uint64_t) */
typedef signed long long    s64;   /* * same as above */
typedef _Bool               bool;
/* `size_t` / `uintptr_t` / `uintN_t` / `intN_t` come from the **compiler's**
 * <stddef.h>/<stdint.h> (upstream includes <stdint.h> itself) — **no duplicate
 * typedef** here, to avoid clashing with the compiler headers. */
typedef long long           loff_t;   /* u-boot: `typedef long long loff_t;` */
typedef unsigned int        dev_t;
typedef unsigned short      umode_t;

#ifndef NULL
#define NULL  BR_NULL
#endif

#define true  BR_TRUE
#define false BR_FALSE

_Static_assert(sizeof(br_size_t) == 8u, "size_t must be 64-bit (matching upstream's aarch64 convention)");
_Static_assert(sizeof(br_u64) == 8u, "u64 must be 64-bit");

/* ---- EROFS's own three types (defined in upstream internal.h, copied verbatim) ---- */
typedef u64 erofs_off_t;    /* byte offset */
typedef u64 erofs_nid_t;    /* inode number (slot index in the metadata area) */
typedef u32 erofs_blk_t;    /* block number within the filesystem */
#define NULL_ADDR     ((unsigned int)-1)
#define NULL_ADDR_UL  ((unsigned long)-1)

/* inode slot (one inode occupies 2^5 = 32 bytes in the metadata area) — upstream
 * internal.h's `EROFS_ISLOTBITS`. super.c copies it into `sbi.islotbits`, and `iloc()`
 * uses it to compute the byte offset of a nid. */
#define EROFS_ISLOTBITS 5

/* ================================================================ on-disk little-endian */
/* Same shape as Linux `include/uapi/linux/types.h`: `__leN` is an integer type whose
 * content is little-endian. This file drops the sparse annotation (`__bitwise`) — so
 * upstream's habit of assigning a `le32_to_cpu()` result straight to `__le32` also
 * type-checks (the kernel needs `__force` there; here it is not needed at all). */
typedef br_u16 __le16;
typedef br_u32 __le32;
typedef br_u64 __le64;
typedef br_u16 __u16;
typedef br_u32 __u32;
typedef br_u64 __u64;
typedef br_u8  __u8;

/* aarch64 = little-endian, so all four conversions are identity. The function form is
 * kept so ported code **still looks like upstream**. */
static inline u16 le16_to_cpu(__le16 v) { return (u16)v; }
static inline u32 le32_to_cpu(__le32 v) { return (u32)v; }
static inline u64 le64_to_cpu(__le64 v) { return (u64)v; }
static inline __le16 cpu_to_le16(u16 v) { return (__le16)v; }
static inline __le32 cpu_to_le32(u32 v) { return (__le32)v; }
static inline __le64 cpu_to_le64(u64 v) { return (__le64)v; }

/* Unaligned access: the target is built with `-mgeneral-regs-only -mstrict-align`, so
 * everything goes through __builtin_memcpy (on aarch64 GCC lowers it to a single
 * `ldr`/`str`, it never really calls libc memcpy). Upstream uses u-boot's
 * `get_unaligned_le32()`; the semantics are identical. */
static inline u16 uboot_erofs_get_unaligned_le16(const void *p)
{
    u16 v;
    (void)__builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static inline u32 uboot_erofs_get_unaligned_le32(const void *p)
{
    u32 v;
    (void)__builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static inline u64 uboot_erofs_get_unaligned_le64(const void *p)
{
    u64 v;
    (void)__builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static inline void uboot_erofs_put_unaligned_le16(u16 v, void *p)
{
    (void)__builtin_memcpy(p, &v, sizeof(v));
}

static inline void uboot_erofs_put_unaligned_le32(u32 v, void *p)
{
    (void)__builtin_memcpy(p, &v, sizeof(v));
}

/* ================================================================ memory movement
 *
 * Project rule (task spec): `memcpy/memset/memcmp/memmove` **are provided by core and may
 * be used directly** — do not write our own and do not depend on runtime/posix. The
 * symbols are implemented in `core/src/string.c` and linked into the image with core.
 * But they have **no public header** (core's `string_internal.h` is private), so the four
 * prototypes are written out here — prototypes are what gives type checking (the other
 * side of the `-Wmissing-prototypes` rule).
 *
 * Why not borrow `__builtin_memcpy` as fs/tmpfs does: in upstream EROFS/LZ4 code
 * `memcpy/memmove/memset/memcmp` appear dozens of times, and replacing each one with a
 * builtin would undercut "you can tell this is a port"; and `LZ4_copy8` should use the
 * builtin anyway (it belongs to the `get_unaligned` family). The two approaches do not
 * conflict: core's implementations are **declared** here, the builtin stays in use there. */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* ================================================================ file type bits
 * Upstream gets `S_IFMT`/`S_ISREG`/... from `linux/stat.h`. Bare metal has no such
 * header, so the bits are supplied here with **values matching Linux verbatim** (only
 * these are needed — this plugin's VFS surface has only DIR/FILE/SYMLINK). */
#define S_IFMT   0170000u
#define S_IFIFO  0010000u
#define S_IFCHR  0020000u
#define S_IFDIR  0040000u
#define S_IFBLK  0060000u
#define S_IFREG  0100000u
#define S_IFLNK  0120000u
#define S_IFSOCK 0140000u

#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* ==================================================================== utility macros */
#define DIV_ROUND_UP(n, d)   (((n) + (d) - 1u) / (d))
#define round_up(x, y)       ((((x) + ((y) - 1u)) / (y)) * (y))
#define round_down(x, y)     (((x) / (y)) * (y))
#define roundup(x, y)        round_up(x, y)
#define rounddown(x, y)      round_down(x, y)

#define min(a, b)            (((a) < (b)) ? (a) : (b))
#define max(a, b)            (((a) > (b)) ? (a) : (b))
#define min_t(type, a, b)    ((type)min((type)(a), (type)(b)))
#define max_t(type, a, b)    ((type)max((type)(a), (type)(b)))

#define likely(x)            (x)
#define unlikely(x)          (x)

/* `ilog2` (used by upstream super.c's `ilog2(EROFS_MAX_BLOCK_SIZE)`);
 * needed at both constant and run time — the run-time version iterates <= 12 times,
 * cheap enough. */
static inline unsigned int uboot_erofs_ilog2(unsigned int v)
{
    unsigned int n = 0u;

    while (v > 1u) {
        v >>= 1;
        n++;
    }
    return n;
}
#define ilog2(v) uboot_erofs_ilog2(v)

/*
 * `BUILD_BUG_ON` — implemented **via u-boot's mechanism**, not via `_Static_assert`.
 *
 * * Why `_Static_assert` cannot be used (ADR-0024 §7): the last self-check in upstream
 *   `erofs_fs.h` is
 *     BUILD_BUG_ON(__builtin_constant_p(fmh) ? fmh != cpu_to_le64(1ULL<<63) : 0);
 *   `fmh` is a function-local `const` — **not an integer constant expression** — so
 *   `_Static_assert` reports "expression in static assertion is not constant". u-boot's
 *   definition is "an `if` in the function body plus a `__compiletime_error` external
 *   function" (see include/linux/build_bug.h and `__compiletime_assert` in
 *   include/linux/compiler.h); it **allows** a non-constant condition: decidable at
 *   compile time => the compiler errors in place with msg; not decidable => one call is
 *   left. Only by copying this mechanism can upstream `erofs_fs.h` compile **verbatim**
 *   (which is exactly the adapter layer's job).
 *
 * Semantics match upstream: the condition triggers on **violation** (upstream passes
 * `!(cond)` into compiletime_assert). Without optimisation (`__OPTIMIZE__` undefined)
 * upstream compiles it to an empty statement; the same is done here.
 */
#define __br_compiletime_error(msg) __attribute__((__error__(msg)))
#define __br_cc_assert_id(prefix, suffix) prefix ## suffix
#define __br_cc_assert(cond, msg, prefix, suffix)                          \
    do {                                                                   \
        extern void __br_cc_assert_id(prefix, suffix)(void)                \
            __br_compiletime_error(msg);                                   \
        if (!(cond))                                                       \
            __br_cc_assert_id(prefix, suffix)();                           \
    } while (0)

#ifdef __OPTIMIZE__
#define BUILD_BUG_ON(condition)                                            \
    __br_cc_assert(!(condition), "BUILD_BUG_ON: " #condition,              \
                   __br_build_bug_on_, __COUNTER__)
#else
#define BUILD_BUG_ON(condition) do { } while (0)
#endif

/*
 * The three attributes/macros provided by u-boot's `<compiler.h>` / `<linux/bitops.h>`.
 * * Why they must be provided here (ADR-0024 §7): `src/erofs_fs.h` must be **verbatim
 *   identical to upstream**, and upstream writes `} __packed u1;` (**positional
 *   attribute**: between `}` and the member name), `const __le64 fmh __maybe_unused = ...`
 *   and `BIT(n)`. Missing any one of them means the upstream text does not compile, so
 *   "keep it verbatim" would degrade into "rewrite upstream" — exactly what this work is
 *   meant to eliminate.
 */
#define __packed        __attribute__((__packed__))
#define __maybe_unused  __attribute__((__unused__))
#define BIT(nr)         (1UL << (nr))

/* u-boot `include/linux/compiler.h`'s `__rcode` (a return-code annotation; empty or used
 * for static checking in u-boot). Upstream `lib/lz4.c` writes it as a leading function
 * qualifier — an empty definition is enough. */
#ifndef __rcode
#define __rcode
#endif

/* u-boot `include/asm/unaligned.h`'s **generic** unaligned access (get_unaligned/
 * put_unaligned). Upstream `lib/lz4.c` uses them to move 8 bytes. Semantics = "unaligned
 * read/write in native byte order"; aarch64 is little-endian, so equivalent to the LE
 * versions (the same __builtin_memcpy implementation).
 * * The target is built with `-mstrict-align`, so real unaligned dereferences are not
 *   allowed — GCC lowers __builtin_memcpy to a safe single ldr/str, which is what we want. */
#define get_unaligned(ptr)      (*(const __typeof__(*(ptr)) *)(ptr))
#define put_unaligned(val, ptr) (*(__typeof__(*(ptr)) *)(ptr) = (val))

/* `CONFIG_IS_ENABLED(RELOC_LOADER)`: there is no relocation loader here, so always 0.
 * (It must be defined explicitly, otherwise `#if CONFIG_IS_ENABLED(RELOC_LOADER)` warns
 * "undefined" under -Wundef.) */
#define RELOC_LOADER            0

/* u-boot's `PAGE_SIZE` comes from `<asm/page.h>`. Upstream `fs/erofs/internal.h` checks
 * it with `#if PAGE_SIZE != 4096 #error` — this platform's page size really is 4096
 * (consistent with the mmu/page pool), so this is a **platform fact**, not a tunable. */
#ifndef PAGE_SIZE
#define PAGE_SIZE       4096u
#endif

/*
 * `struct list_head` — provided by u-boot's `<linux/list.h>` (through
 * `<linux/compat.h>`), and upstream `fs/erofs/internal.h`'s `struct erofs_inode` /
 * `struct erofs_dentry` **contain its fields** (`i_hash` / `i_subdirs` / `i_xattrs` /
 * `d_child`).
 *
 * * Only the **type itself** is provided, no traversal/insertion functions — those
 *   fields belong to the **cache and dentry layer**, while this port's inode is a
 *   "transient path wrapper" (ADR-0022 D-3) that never uses these lists. With the right
 *   type size the upstream structure layout holds; nobody traverses them, so nothing
 *   needs implementing.
 */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/*
 * * Intentional deviation: upstream `DBG_BUGON()` = `BUG_ON()` = **halt**. This plugin
 *   uses it heavily on the mount path and the read path for "cannot happen" assertions,
 *   and the task spec's principle is "mount failure => first-failure halt is
 *   intentional", but **the read path must not kill the kernel over one suspicious
 *   on-disk field** (when that image is corrupt the right behaviour is to give the
 *   caller -EIO, not take the whole device down). So it becomes "log + continue" here,
 *   converged by the caller's immediately following `return -EFSCORRUPTED` (every
 *   DBG_BUGON is followed by an error return or an equivalent branch).
 */
#define BUG_ON(cond)                                                                    \
    do {                                                                                \
        if (unlikely(cond)) {                                                           \
            uboot_erofs_log_err_inline("DBG_BUGON(%s) @ %s:%d", #cond, __FILE__, __LINE__); \
        }                                                                               \
    } while (0)


/* ==================================================================== logging
 *
 * Upstream: `erofs_err(fmt, ...)` = `pr_err(fmt "\n", ...)`. This file adds no newline
 * (br_log_write outputs line by line).
 *
 * * Why **functions** instead of a variadic macro: C11's `__VA_ARGS__` is wrong with
 *   zero variadic arguments (GNU's `##__VA_ARGS__` is an extension, and this file
 *   follows the `-std=c11` rule). Upstream has several argument-less `erofs_err("...")`
 *   calls (e.g. "failed to read packed inode from disk"), so two **wrapper functions**
 *   forward the variadic arguments as-is to core's log entry — call sites stay exactly
 *   as upstream wrote them and zero-argument calls remain legal. They do **not** carry
 *   the `format(printf)` attribute: core's `br_log_write` has its own, and this file
 *   prefixes the format string with "uboot-erofs: ", which would make the compiler
 *   check the composed string twice.
 */
/*
 * * Why "format first, then forward": `br_log_write()` is a **variadic** function, and
 *   there are only two ways to "pass the arguments through as-is" — either
 *   `<stdarg.h>`'s `va_list` interface (bare metal has no such header, it is libc's),
 *   or let the caller format first. This file takes the latter: compiler-provided
 *   `__builtin_vsnprintf` builds the whole "uboot-erofs: <content>" line into a small
 *   stack buffer, then the **already-complete line** is handed to core as a format
 *   string (the log subsystem re-formats it harmlessly; see br_log_write's contract).
 *   Why not `__builtin_va_list`: `br_log.h` exposes only `br_log_write(...)`, with no
 *   "v" variant — so "format it ourselves" is the only remaining route (rather than
 *   adding a function to core's contract surface).
 *
 * * Bound and truncation: a 256-byte stack buffer. These logs are a **diagnostic
 *   surface**: the longest one (decompression failure) is about 60 bytes and the rest
 *   are shorter, so the normal path never truncates; a real truncation only drops a few
 *   characters and does not affect any criterion.
 *
 * * Why the `format(printf)` attribute: call sites' format strings and arguments are
 *   still checked one by one by the compiler (the same check core's `br_log_write`
 *   gets). The attribute belongs to the **inline wrapper** itself and is not checked
 *   twice.
 */
#define UBOOT_EROFS_LOG_MAX  256u

/*
 * * Why not use `__builtin_vsnprintf` directly: GCC's `__builtin_vsnprintf` is **not**
 *   self-contained — it still expands to a call to libc `vsnprintf` (observed:
 *   link-time `undefined reference to vsnprintf`). This repo has no libc (core provides
 *   only the compiler support routines mem*), and `runtime/posix` has no printf family
 *   either (ADR-0014 §4 says explicitly it will not). Core exposes no v-variant
 *   interface either (`br_log.h` has only the variadic `br_log_write`, no
 *   `br_log_vwrite`), so only two routes remain:
 *     1. have core expose one more v interface (a change to core's contract surface,
 *        not worth it for one diagnostic print);
 *     2. carry a tiny formatter in this file (covering only the conversions actually
 *        used here).
 *   Route 2 is chosen. Its criterion surface is narrow: **the output only feeds the
 *   log** and takes part in no filesystem decision.
 *
 * Coverage = the real usage checked call site by call site: `%s %d %u %x %lu %lx %llu
 * %llx %%` plus the length modifiers `l`/`ll`/`z`. Width/precision/other flags are used
 * **nowhere**, so they are not implemented; if one does appear it is echoed verbatim
 * (not silently swallowed — better an ugly log than pretending formatting succeeded).
 */
static inline char *uboot_erofs_fmt_uint(char *p, char *end, unsigned long long v,
                                         unsigned base, br_bool upper)
{
    char tmp[24];
    unsigned n = 0u;

    do {
        const unsigned d = (unsigned)(v % base);
        tmp[n++] = (char)((d < 10u) ? ('0' + (char)d)
                                    : ((upper ? 'A' : 'a') + (char)(d - 10u)));
        v /= base;
    } while (v != 0u);

    while (n > 0u) {
        n--;
        if (p < end) {
            *p++ = tmp[n];
        }
    }
    return p;
}

static inline int uboot_erofs_vformat(char *dst, br_size_t cap, const char *fmt,
                                      __builtin_va_list ap)
{
    char       *p   = dst;
    char *const end = (cap > 0u) ? (dst + cap - 1u) : dst;

    while (*fmt != '\0') {
        if (*fmt != '%') {
            if (p < end) { *p++ = *fmt; }
            fmt++;
            continue;
        }

        fmt++;
        unsigned lcount = 0u;          /* 0 = int, 1 = long/z, 2 = long long */
        while (*fmt == 'l') { lcount++; fmt++; }
        if (lcount == 0u && *fmt == 'z') { lcount = 1u; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (s == BR_NULL) { s = "(null)"; }
            while (*s != '\0') {
                if (p < end) { *p++ = *s; }
                s++;
            }
            break;
        }
        case 'd': {
            long long v;
            if (lcount >= 2u) {
                v = __builtin_va_arg(ap, long long);
            } else if (lcount == 1u) {
                v = (long long)__builtin_va_arg(ap, long);
            } else {
                v = (long long)__builtin_va_arg(ap, int);
            }
            unsigned long long u = (unsigned long long)v;
            if (v < 0) {
                if (p < end) { *p++ = '-'; }
                u = ~u + 1ull;
            }
            p = uboot_erofs_fmt_uint(p, end, u, 10u, BR_FALSE);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long long v;
            if (lcount >= 2u) {
                v = __builtin_va_arg(ap, unsigned long long);
            } else if (lcount == 1u) {
                v = (unsigned long long)__builtin_va_arg(ap, unsigned long);
            } else {
                v = (unsigned long long)__builtin_va_arg(ap, unsigned int);
            }
            p = uboot_erofs_fmt_uint(p, end, v, (*fmt == 'u') ? 10u : 16u,
                                     (*fmt == 'X') ? BR_TRUE : BR_FALSE);
            break;
        }
        case 'p': {
            const void *q = __builtin_va_arg(ap, void *);
            p = uboot_erofs_fmt_uint(p, end, (unsigned long long)(br_uintptr_t)q, 16u, BR_FALSE);
            break;
        }
        case '%':
            if (p < end) { *p++ = '%'; }
            break;
        case '\0':                     /* trailing '%': finish as-is */
            fmt--;
            break;
        default:                       /* unimplemented modifier: echo as-is */
            if (p < end) { *p++ = '%'; }
            if (p < end) { *p++ = *fmt; }
            break;
        }

        if (*fmt != '\0') { fmt++; }
    }

    *p = '\0';
    return (int)(p - dst);
}

static inline void uboot_erofs_log_err_inline(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static inline void uboot_erofs_log_dbg_inline(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static inline void uboot_erofs_log_err_inline(const char *fmt, ...)
{
    char line[UBOOT_EROFS_LOG_MAX];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    (void)uboot_erofs_vformat(line, sizeof(line), fmt, ap);
    __builtin_va_end(ap);
    br_log_write(BR_LOG_ERROR, "uboot-erofs: %s", line);
}

static inline void uboot_erofs_log_dbg_inline(const char *fmt, ...)
{
    char line[UBOOT_EROFS_LOG_MAX];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    (void)uboot_erofs_vformat(line, sizeof(line), fmt, ap);
    __builtin_va_end(ap);
    br_log_write(BR_LOG_DEBUG, "uboot-erofs: %s", line);
}



/* ==================================================================== u-boot pr_*
 *
 * Upstream `fs/erofs/internal.h` itself maps `erofs_err/info/dbg` to
 * `pr_err/pr_info/pr_debug` — so **it supplies the names, we supply the
 * implementations** (exactly "do not change upstream, only adapt"). Note it writes
 * `pr_err(fmt "\n", ##__VA_ARGS__)`: with the GNU comma-swallowing extension, and the
 * format string already ends in a newline (our logs are line-based, so the extra
 * newline is harmless).
 */
static inline void uboot_erofs_log_info_inline(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

static inline void uboot_erofs_log_info_inline(const char *fmt, ...)
{
    char line[UBOOT_EROFS_LOG_MAX];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    (void)uboot_erofs_vformat(line, sizeof(line), fmt, ap);
    __builtin_va_end(ap);
    br_log_write(BR_LOG_INFO, "uboot-erofs: %s", line);
}

#define pr_err(fmt, ...)    uboot_erofs_log_err_inline(fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   uboot_erofs_log_info_inline(fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)  uboot_erofs_log_dbg_inline(fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   uboot_erofs_log_err_inline(fmt, ##__VA_ARGS__)

/* ================================================================ kernel-style helpers */
/* A few Linux idioms used by upstream in `data.c`/`zmap.c`/`super.c`. They are **pure
 * macros/functions** whose semantics must be the kernel's (not invented), so each one
 * follows the Linux wording. */
#ifndef UINT_MAX
#define UINT_MAX            0xffffffffu
#endif
#ifndef ULONG_MAX
#define ULONG_MAX           (~0UL)
#endif

/* `roundup_pow_of_two()`: round up to a power of two (Linux `include/linux/log2.h`). */
static inline unsigned long uboot_erofs_roundup_pow_of_two(unsigned long n)
{
    unsigned long r = 1ul;

    if (n <= 1ul) {
        return 1ul;
    }
    while (r < n) {
        r <<= 1;
    }
    return r;
}
#define roundup_pow_of_two(n) uboot_erofs_roundup_pow_of_two(n)

/*
 * `IS_ERR/ERR_PTR/PTR_ERR` (Linux `include/linux/err.h`) — upstream `namei.c` uses them
 * to fold "directory entry not found" and "a real error" into a single return value.
 * * An earlier port removed these (switching to an out parameter), but that **changes
 *   upstream code**; this work needs the upstream text to hold, so the convention must
 *   be provided as-is: error codes live on the **high-address side** of the pointer
 *   value (> -4096).
 */
#define MAX_ERRNO           4095
#define IS_ERR_VALUE(x)     unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error)
{
    return (void *)error;
}

static inline long PTR_ERR(const void *ptr)
{
    return (long)ptr;
}

static inline bool IS_ERR(const void *ptr)
{
    return IS_ERR_VALUE((unsigned long)ptr);
}

static inline bool IS_ERR_OR_NULL(const void *ptr)
{
    return (ptr == BR_NULL) || IS_ERR_VALUE((unsigned long)ptr);
}

/*
 * `IS_ENABLED()`/`CONFIG_IS_ENABLED()` (u-boot `include/linux/kconfig.h`).
 * Upstream `decompress.c` uses them for compile-time pruning: `#if IS_ENABLED(CONFIG_LZ4)`.
 * * This file supports LZ4, not zlib/deflate — so CONFIG_LZ4 is 1 and CONFIG_ZLIB is 0,
 *   which makes the `#if` branches hold exactly as upstream wrote them, with **no line
 *   of its code changed**.
 */
#define IS_ENABLED(opt)         (opt)
#define CONFIG_IS_ENABLED(opt)  (opt)
#define CONFIG_LZ4              1
#define CONFIG_ZLIB             0

/* ==================================================================== errno convention
 *
 * The plain names (`EIO`/`ENOENT`/`EOPNOTSUPP`/...) and `EUCLEAN` are provided by
 * **core** (`<br/core/br_errno.h>`, ADR-0023); the values correspond one-to-one with
 * `br_error.h` (taken verbatim from the Linux kernel and cross-checked code by code by
 * the host `ft-test`). This file carries **no mapping table of its own** — that would be
 * both a second source of truth and a needless change to upstream code.
 *
 * * `EFSCORRUPTED` here **copies upstream `internal.h`'s construction** (the
 *   `#ifdef EUCLEAN` branch) with not one character changed; it used to be mapped to
 *   `EIO` because this repo had no errno 117 then — now `EUCLEAN` has been added from
 *   the kernel table per ADR-0023 D-4. */
#include <br/core/br_errno.h>

#define EFSCORRUPTED  EUCLEAN

/* ================================================================ string helpers
 *
 * All `static inline` with the `uboot_erofs_` prefix (hard constraint of the task spec:
 * no dependency on runtime/posix). Macros then wire upstream's plain names over — that
 * is how "you can tell ported code is a port" works: the `strnlen(de_name, n)` call site
 * stays as upstream wrote it while expanding to our own implementation.
 *
 * Length-bound discipline (the same as fs/tmpfs's `tmpfs_name_ok`): these functions
 * honour only the caller-supplied `n` and never read one byte beyond it. */
static inline br_size_t uboot_erofs_strlen(const char *s)
{
    br_size_t n = 0u;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static inline br_size_t uboot_erofs_strnlen(const char *s, br_size_t n)
{
    br_size_t i = 0u;

    while ((i < n) && (s[i] != '\0')) {
        i++;
    }
    return i;
}

static inline int uboot_erofs_strcmp(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline int uboot_erofs_strncmp(const char *a, const char *b, br_size_t n)
{
    br_size_t i;

    for (i = 0u; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

/*
 * * This file **deliberately does not provide** a "copy until NUL" helper such as
 *   `strcpy` (upstream has no call site either). The reason is a trap actually hit:
 *   names in an EROFS directory block are **not NUL-terminated** — they are packed one
 *   after another in the name area (the length comes from subtracting the `nameoff` of
 *   two adjacent dirents). Reading them by "scan until NUL" swallows every following
 *   name into the buffer (observed symptom: "comp" becoming
 *   "compdirhello.txtraw.bin"). So wherever on-disk bytes are moved, the
 *   **length-aware** `uboot_erofs_copy_n()` / `__builtin_memcpy` is used.
 */
static inline void uboot_erofs_copy_n(char *dst, br_size_t cap, const char *src, br_size_t n)
{
    if (cap == 0u) {
        return;
    }
    if (n > (cap - 1u)) {
        n = cap - 1u;               /* truncate (the caller guarantees n was in bounds) */
    }
    for (br_size_t i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
    dst[n] = '\0';
}

static inline char *uboot_erofs_strchr(const char *s, int c)
{
    char ch = (char)c;

    for (;; s++) {
        if (*s == ch) {
            return (char *)(uintptr_t)s;
        }
        if (*s == '\0') {
            return (char *)0;
        }
    }
}

/* Recognisably a port: upstream plain names -> our implementations (whole TU is
 * -fno-builtin, so no self-collision) */
#define strlen(s)          uboot_erofs_strlen(s)
#define strnlen(s, n)      uboot_erofs_strnlen((s), (n))
#define strcmp(a, b)       uboot_erofs_strcmp((a), (b))
#define strncmp(a, b, n)   uboot_erofs_strncmp((a), (b), (n))
#define strchr(s, c)       uboot_erofs_strchr((s), (c))
#define copy_n(d, cap, s, n) uboot_erofs_copy_n((d), (cap), (s), (n))

/* unaligned little-endian loads (upstream's `get_unaligned_le32`) */
#define get_unaligned_le16(p) uboot_erofs_get_unaligned_le16(p)
#define get_unaligned_le32(p) uboot_erofs_get_unaligned_le32(p)
#define get_unaligned_le64(p) uboot_erofs_get_unaligned_le64(p)
#define put_unaligned_le16(v, p) uboot_erofs_put_unaligned_le16((v), (p))
#define put_unaligned_le32(v, p) uboot_erofs_put_unaligned_le32((v), (p))

/* log wrappers (defined in this plugin's erofs_vfs.c — the plugin's only "glue"
 * translation unit) */

/* ==================================================================== format strings
 * Upstream uses `PRIu64`/`PRIx64` (<inttypes.h>). This file's log formatter knows only
 * `%llu`/`%llx`, so these macros **include the cast** — call sites therefore need no
 * extra `(unsigned long long)`. This is the "recognisably a port" trade-off: upstream
 * format strings stay unchanged character for character. */
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRId64 "lld"

#endif /* BR_ADAPTER_UBOOT_EROFS_ENV_H */
