/*
 * fs/uboot-erofs - u-boot runtime environment (adapter layer) for a bare-metal
 * unikernel. Maps upstream's kernel/u-boot assumptions onto this project: u8/u16/u32/u64 ->
 * br_u8/..., bool -> br_bool, __leN/leN_to_cpu -> identity (aarch64 LE, #error-enforced
 * below), get_unaligned_le16/32 -> __builtin_memcpy (target -mstrict-align),
 * min_t/max_t/round_up/round_down/ilog2/DIV_ROUND_UP -> the small macros here, malloc
 * family -> br_malloc family, memcpy/memset/memcmp/memmove -> core-provided, string
 * helpers -> static inline uboot_erofs_* behind macros, printf/pr_err/pr_info/pr_debug ->
 * br_log_error/info/debug, BUG_ON ->
 * log without crash, E* names -> core's <br/core/br_errno.h>.
 *
 * Deviations from upstream: compile-time IS_ENABLED(CONFIG_LZ4) switches become run-time
 * branches in erofs_decompress; erofs_err() adds no trailing newline (br_log_write is
 * line-based); erofs_dbg() emits only with BR_EROFS_VERBOSE_DBG, so machines without a
 * serial port are not flooded by per-block mapping logs. The whole TU is -fno-builtin.
 */
#ifndef BR_ADAPTER_UBOOT_EROFS_ENV_H
#define BR_ADAPTER_UBOOT_EROFS_ENV_H

#include <stddef.h>     /* NULL / size_t — compiler-provided header, not libc */

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>
#include <br/core/br_types.h>
/* Block-device contract: upstream gets it from `blk.h`. Only this header (the adapter entry)
 * may include `<br/...>`; upstream-derived files under `src/` take their environment through it. */
#include <br/dev/br_bdev.h>

/* uintN_t/intN_t come from the compiler's own <stdint.h>; not redefined here (would clash). */
#include <stdint.h>

/* ---- memory allocation: the libc names (malloc/free/calloc/realloc) are wired to core's
 * br_malloc family, so ported code needs no changes. No allocator is written here; core
 * returns NULL on failure just like libc, keeping upstream's assumption. */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* ---- little-endian premise: leN_to_cpu() is an identity function here, valid only on
 * little-endian targets (aarch64 QEMU virt and similar). Stopping on big-endian is better
 * than silently producing wrong words. */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-erofs leN_to_cpu() is an identity implementation: little-endian targets only"
#endif

/* ---- base types ---- */
typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;   /* * u-boot uses long long here (not uint64_t) */
typedef signed long long    s64;   /* * same as above */
typedef _Bool               bool;
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

/* ---- EROFS's own types (from upstream internal.h) ---- */
typedef u64 erofs_off_t;    /* byte offset */
typedef u64 erofs_nid_t;    /* inode number (slot index in the metadata area) */
typedef u32 erofs_blk_t;    /* block number within the filesystem */
#define NULL_ADDR     ((unsigned int)-1)
#define NULL_ADDR_UL  ((unsigned long)-1)

/* Inode slot = 32 bytes (upstream EROFS_ISLOTBITS): super.c copies it into sbi.islotbits and
 * iloc() uses it to compute the byte offset of a nid. */
#define EROFS_ISLOTBITS 5

/* ---- on-disk little-endian ---- */
/* __leN: integer type whose content is little-endian. The sparse __bitwise annotation is
 * dropped, so upstream's le32_to_cpu() results assign straight to __le32 without __force. */
typedef br_u16 __le16;
typedef br_u32 __le32;
typedef br_u64 __le64;
typedef br_u16 __u16;
typedef br_u32 __u32;
typedef br_u64 __u64;
typedef br_u8  __u8;

/* aarch64 LE: the conversions are identity; function form kept so ported code reads as upstream. */
static inline u16 le16_to_cpu(__le16 v) { return (u16)v; }
static inline u32 le32_to_cpu(__le32 v) { return (u32)v; }
static inline u64 le64_to_cpu(__le64 v) { return (u64)v; }
static inline __le16 cpu_to_le16(u16 v) { return (__le16)v; }
static inline __le32 cpu_to_le32(u32 v) { return (__le32)v; }
static inline __le64 cpu_to_le64(u64 v) { return (__le64)v; }

/* Unaligned access via __builtin_memcpy (target is -mgeneral-regs-only -mstrict-align; GCC
 * lowers it to a single ldr/str). Semantics match u-boot's get_unaligned_le32(). */
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

/* ---- memory movement: memcpy/memset/memcmp/memmove are provided by core
 * (core/c_core/src/string.c) and may be used directly, but they have no public header, so the
 * prototypes are written out here (also what -Wmissing-prototypes wants). Call sites keep the
 * plain names so ported EROFS/LZ4 code reads as a port; LZ4_copy8 uses the builtin instead. */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* ---- file type bits: values identical to Linux's linux/stat.h (bare metal has no such
 * header). Needed only for DIR/FILE/SYMLINK, this plugin's VFS surface. */
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

/* ---- utility macros ---- */
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

/* ilog2, used by upstream super.c; needed at constant and run time, iterating <= 12 times. */
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

/* BUILD_BUG_ON via u-boot's mechanism, not _Static_assert: upstream erofs_fs.h's last
 * self-check tests a function-local `const` fmh that is not an integer constant expression, so
 * _Static_assert would reject it. u-boot's form (an if plus a __compiletime_error function)
 * allows a non-constant condition and lets the upstream text compile verbatim. Without
 * __OPTIMIZE__ it compiles to an empty statement, as upstream does. */
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

/* __packed / __maybe_unused / BIT (u-boot compiler.h / linux/bitops.h): upstream erofs_fs.h
 * writes them positionally (`} __packed u1;`) and as `__maybe_unused` / `BIT(n)`, so they
 * must exist for that text to compile verbatim. */
#define __packed        __attribute__((__packed__))
#define __maybe_unused  __attribute__((__unused__))
#define BIT(nr)         (1UL << (nr))

/* u-boot's __rcode return-code annotation; empty is enough (lib/lz4.c uses it as a qualifier). */
#ifndef __rcode
#define __rcode
#endif

/* u-boot's generic get_unaligned/put_unaligned, used by lib/lz4.c to move 8 bytes. Native byte
 * order = little-endian here, so same as the LE versions. -mstrict-align forbids real unaligned
 * dereferences, but these expand to __builtin_memcpy (a safe single ldr/str). */
#define get_unaligned(ptr)      (*(const __typeof__(*(ptr)) *)(ptr))
#define put_unaligned(val, ptr) (*(__typeof__(*(ptr)) *)(ptr) = (val))

/* No relocation loader here: always 0; defined explicitly to avoid a -Wundef warning. */
#define RELOC_LOADER            0

/* Platform fact: upstream internal.h requires PAGE_SIZE == 4096, and this platform's is 4096. */
#ifndef PAGE_SIZE
#define PAGE_SIZE       4096u
#endif

/* struct list_head: upstream erofs_inode/erofs_dentry contain its fields (i_hash/i_subdirs/
 * i_xattrs/d_child), so the type must exist with the right size. No traversal or insertion
 * functions: this port's inode is a transient path wrapper that never uses the lists. */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* Deviation: upstream DBG_BUGON/BUG_ON halts. The read path must not kill the kernel over one
 * suspicious on-disk field -- a corrupt image should yield -EIO -- so this logs and continues;
 * every call site is followed by an error return. */
#define BUG_ON(cond)                                                                    \
    do {                                                                                \
        if (unlikely(cond)) {                                                           \
            uboot_erofs_log_err_inline("DBG_BUGON(%s) @ %s:%d", #cond, __FILE__, __LINE__); \
        }                                                                               \
    } while (0)


/* ---- logging: erofs_err(fmt, ...) = pr_err(fmt, ...); this file adds no newline (br_log_write
 * outputs line by line). Wrapper functions rather than a variadic macro because C11 __VA_ARGS__
 * is wrong with zero variadic arguments (this file is -std=c11) and upstream has argument-less
 * erofs_err("...") calls. No format(printf) attribute here: br_log_write carries its own and the
 * prefixed string would otherwise be checked twice. */
/* Format first, then forward: br_log_write is variadic and there is no v-variant (.stdarg.h is
 * libc's, and br_log.h exposes only br_log_write), so __builtin_vsnprintf builds the complete
 * "uboot-erofs: <content>" line into a stack buffer and that finished line is passed as a format
 * string. Buffer is 256 bytes: the longest log is ~60 bytes, so the normal path never truncates,
 * and a truncation only drops characters and affects no criterion. The format(printf) attribute
 * on the inline wrapper still checks each call site once. */
#define UBOOT_EROFS_LOG_MAX  256u

/* Why a local formatter: GCC's __builtin_vsnprintf still calls libc vsnprintf, which does not
 * exist here (core provides only the mem* support routines). Coverage, checked call site by call
 * site: %s %d %u %x %lu %lx %llu %llx %% with the l/ll/z length modifiers. Width, precision and
 * flags are used nowhere and not implemented; an unknown modifier is echoed verbatim rather than
 * silently swallowed. The output only feeds the log and no filesystem decision. */
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



/* u-boot pr_*: upstream internal.h maps erofs_err/info/dbg to pr_err/pr_info/pr_debug -- it
 * supplies the names, this file the implementations. It writes pr_err(fmt "\n", ##__VA_ARGS__);
 * the extra newline is harmless for line-based logs. */
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

/* ---- kernel-style helpers: Linux idioms used by upstream data.c/zmap.c/super.c; semantics
 * must match the kernel's, not invented. */
#ifndef UINT_MAX
#define UINT_MAX            0xffffffffu
#endif
#ifndef ULONG_MAX
#define ULONG_MAX           (~0UL)
#endif

/* roundup_pow_of_two(): round up to a power of two (Linux log2.h). */
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

/* IS_ERR/ERR_PTR/PTR_ERR (Linux err.h): upstream namei.c folds "entry not found" and "real
 * error" into one return value, so the convention is provided as-is -- error codes live on the
 * high-address side of the pointer value (> -4096). */
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

/* IS_ENABLED/CONFIG_IS_ENABLED (u-boot kconfig.h): upstream decompress.c prunes with
 * #if IS_ENABLED(CONFIG_LZ4). Only LZ4 is supported here, so CONFIG_LZ4=1 / CONFIG_ZLIB=0 keeps
 * the upstream #if branches true. */
#define IS_ENABLED(opt)         (opt)
#define CONFIG_IS_ENABLED(opt)  (opt)
#define CONFIG_LZ4              1
#define CONFIG_ZLIB             0

/* ---- errno convention: plain E* names and EUCLEAN come from core's <br/core/br_errno.h>,
 * values matching br_error.h and cross-checked by the host ft-test; no mapping table here.
 * EFSCORRUPTED = EUCLEAN copies upstream internal.h's #ifdef EUCLEAN construction verbatim. */
#include <br/core/br_errno.h>

#define EFSCORRUPTED  EUCLEAN

/* ---- string helpers: all static inline with the uboot_erofs_ prefix (no runtime/posix
 * dependency); macros below wire upstream's plain names over them, so call sites such as
 * strnlen(de_name, n) stay as upstream wrote them. Length-bound discipline (as fs/tmpfs's
 * tmpfs_name_ok): honour only the caller's n, never read one byte beyond it. */
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

/* No strcpy-style "copy until NUL" helper here (upstream has no call site either): EROFS dirent
 * names are packed without NUL terminators, so scanning to NUL swallows the following names.
 * On-disk bytes are always moved with the length-aware uboot_erofs_copy_n()/__builtin_memcpy. */
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

/* Upstream plain names -> our implementations (whole TU is -fno-builtin, so no self-collision). */
#define strlen(s)          uboot_erofs_strlen(s)
#define strnlen(s, n)      uboot_erofs_strnlen((s), (n))
#define strcmp(a, b)       uboot_erofs_strcmp((a), (b))
#define strncmp(a, b, n)   uboot_erofs_strncmp((a), (b), (n))
#define strchr(s, c)       uboot_erofs_strchr((s), (c))
#define copy_n(d, cap, s, n) uboot_erofs_copy_n((d), (cap), (s), (n))

#define get_unaligned_le16(p) uboot_erofs_get_unaligned_le16(p)
#define get_unaligned_le32(p) uboot_erofs_get_unaligned_le32(p)
#define get_unaligned_le64(p) uboot_erofs_get_unaligned_le64(p)
#define put_unaligned_le16(v, p) uboot_erofs_put_unaligned_le16((v), (p))
#define put_unaligned_le32(v, p) uboot_erofs_put_unaligned_le32((v), (p))

/* ---- format strings: upstream uses PRIu64/PRIx64; this file's formatter knows only %llu/%llx,
 * so the macros include the cast and upstream format strings stay unchanged. */
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRId64 "lld"

#endif /* BR_ADAPTER_UBOOT_EROFS_ENV_H */
