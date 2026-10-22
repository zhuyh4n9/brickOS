/*
 * fs/uboot-fat - u-boot runtime environment for the vendored FAT sources (fs/fat/fat.c +
 * fs/fat/fat_write.c, one translation unit). Upstream assumes u-boot's types, libc, blk/part/fs
 * headers and logging; this bare-metal unikernel has no libc, so this layer supplies them and
 * the upstream .c needs not one character changed (hash lock:
 * tools/check-third-party-pristine.sh). Mapping, bounds and behavioural deviations are
 * documented at their definitions below.
 */
#ifndef BR_ADAPTER_UBOOT_FAT_ENV_H
#define BR_ADAPTER_UBOOT_FAT_ENV_H

/* NULL/size_t come from the compiler's own <stddef.h> (not libc); uintN_t from <stdint.h>. */
#include <stddef.h>
#include <stdint.h>

#include <br/core/br_error.h>
#include <br/core/br_errno.h>
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>
#include <br/core/br_types.h>
/* Block device contract (upstream gets it from <blk.h>/<part.h>). Only this header includes
 * <br/...>; upstream .c and glue reach core through it. */
#include <br/dev/br_bdev.h>

/* ---- little-endian premise ---- */
/* FAT2CPU16/32 (vendored fat.h) and le16_to_cpu() are identity here => little-endian targets
 * only: on a big-endian target every on-disk field would be misread, so fail at compile time. */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-fat FAT2CPU16/32 and leN_to_cpu() are identity: this port supports little-endian only"
#endif
/* Vendored fat.h selects the `#if __LITTLE_ENDIAN` branch; give that macro a true value. */
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif

/* ---- base types ---- */
/* u-boot's widths verbatim: u64/s64/loff_t are unsigned long long/long long, while br_u64/
 * br_s64 are unsigned long/long -- same width, different type (-Werror=format= on %llu/%lld). */
typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;
typedef signed long long    s64;
typedef long long           loff_t;   /* u-boot: `typedef long long loff_t;` */
typedef unsigned long  ulong;   /* u-boot `include/linux/types.h` */
typedef unsigned int   uint;
typedef unsigned char  uchar;
typedef br_bool        bool;

#define true  BR_TRUE
#define false BR_FALSE

#ifndef NULL
#define NULL BR_NULL
#endif

_Static_assert(sizeof(u64) == 8u, "u64 must be 64-bit");
_Static_assert(sizeof(u8) == 1u, "u8 must be 8-bit");
_Static_assert(sizeof(u64) == sizeof(unsigned long long), "u64 = u-boot's long long");
_Static_assert(sizeof(loff_t) == 8u, "loff_t must be 64-bit (upstream aarch64)");
_Static_assert(sizeof(u16) == 2u, "u16 must be 16-bit");
_Static_assert(sizeof(u32) == 4u, "u32 must be 32-bit");

/* ---- on-disk integer family (u-boot's linux/types.h) ---- */
typedef unsigned char      __u8;
typedef unsigned short     __u16;
typedef unsigned int       __u32;
typedef unsigned long long __u64;
typedef __u16 __le16;
typedef __u32 __le32;
typedef __u64 __le64;

_Static_assert(sizeof(__u64) == 8u, "__u64 must be 64-bit");

/* ---- byte order (identity) ---- */
static inline u16 uboot_fat_le16_to_cpu(u16 v) { return v; }
static inline u32 uboot_fat_le32_to_cpu(u32 v) { return v; }
static inline u16 uboot_fat_cpu_to_le16(u16 v) { return v; }
static inline u32 uboot_fat_cpu_to_le32(u32 v) { return v; }
#define le16_to_cpu(x) uboot_fat_le16_to_cpu(x)
#define le32_to_cpu(x) uboot_fat_le32_to_cpu(x)
#define cpu_to_le16(x) uboot_fat_cpu_to_le16(x)
#define cpu_to_le32(x) uboot_fat_cpu_to_le32(x)

/* Unaligned LE loads: -mstrict-align forbids a direct dereference, so go through
 * __builtin_memcpy (GCC lowers it to a single ldr/str on aarch64). */
static inline u16 uboot_fat_get_unaligned_le16(const void *p)
{
    u16 v;
    (void)__builtin_memcpy(&v, p, sizeof(v));
    return v;
}

static inline u32 uboot_fat_get_unaligned_le32(const void *p)
{
    u32 v;
    (void)__builtin_memcpy(&v, p, sizeof(v));
    return v;
}

#define get_unaligned_le16(p) uboot_fat_get_unaligned_le16(p)
#define get_unaligned_le32(p) uboot_fat_get_unaligned_le32(p)

/* ---- memory ---- */
/* Names forwarded to core's allocator; macros rather than a prototype, which would make this
 * media FS link against the provider/posix surface. br_malloc aligns to >= 16 B (BR_MALLOC_ALIGN),
 * above the 8-byte max natural alignment, so the cache-aligned/memalign variants add nothing. */
#define malloc(n)             br_malloc(n)
#define calloc(n, s)          br_calloc((n), (s))
#define realloc(p, n)         br_realloc((p), (n))
#define free(p)               br_free(p)
#define memalign(a, n)        ((void)(a), br_malloc(n))
#define malloc_cache_aligned(n) br_malloc(n)

/* Provided by core (core/c_core/src/string.c); use these four directly, do not rewrite. Core-private
 * string_internal.h leaves the prototypes to here (the other side of -Wmissing-prototypes). */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* u-boot's DMA alignment macros. No DMA constraint here (the driver owns DMA), so 16 matches
 * BR_MALLOC_ALIGN; ALLOC_CACHE_ALIGN_BUFFER still yields a truly 16 B aligned on-stack ptr. */
#ifndef ARCH_DMA_MINALIGN
#define ARCH_DMA_MINALIGN 16u
#endif

#define UBOOT_FAT_ALIGN_UP(x, a) \
    ((void *)((((br_uintptr_t)(x)) + ((br_uintptr_t)(a) - 1u)) & ~((br_uintptr_t)(a) - 1u)))

#define ALLOC_CACHE_ALIGN_BUFFER(type, name, size)                              \
    unsigned char __##name##_raw[(size) * sizeof(type) + ARCH_DMA_MINALIGN];     \
    type *name = (type *)UBOOT_FAT_ALIGN_UP(__##name##_raw, ARCH_DMA_MINALIGN)

#define ALLOC_ALIGN_BUFFER(type, name, size, align)                             \
    unsigned char __##name##_raw[(size) * sizeof(type) + (align)];               \
    type *name = (type *)UBOOT_FAT_ALIGN_UP(__##name##_raw, align)

/* Only the attribute macros upstream uses (fat.c's __maybe_unused fat2rtc): supply the names
 * upstream needs, not an approximate Linux header. */
#define __maybe_unused  __attribute__((__unused__))
#define __packed        __attribute__((__packed__))
#define __aligned(x)    __attribute__((__aligned__(x)))
#define __always_inline inline __attribute__((__always_inline__))
#ifndef __force
#define __force
#endif

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

/* Position of the highest set bit, 1-based, fls(0)=0 (upstream set_name sizes the short-name
 * suffix search with it). */
static inline int uboot_fat_fls(unsigned int v)
{
    int n = 0;

    while (v != 0u) {
        v >>= 1;
        n++;
    }
    return n;
}
#define fls(v) uboot_fat_fls(v)

/* is_power_of_2(): non-zero and no bits below the top one (upstream is_bootsector_valid). */
static inline bool is_power_of_2(unsigned long n)
{
    return (bool)((n != 0ul) && ((n & (n - 1ul)) == 0ul));
}

/* ilog2(): floor(log2(v)); unused here but kept at upstream's name and meaning. */
static inline unsigned int uboot_fat_ilog2(unsigned long v)
{
    unsigned int n = 0u;

    while (v > 1ul) {
        v >>= 1;
        n++;
    }
    return n;
}
#define ilog2(v) uboot_fat_ilog2(v)

/* do_div/lldiv/div_u64_rem: BITS_PER_LONG == 64 here, so these are u-boot's 64-bit branch
 * verbatim. do_div divides `n` in place and evaluates to the remainder. */
#define do_div(n, base) ({                          \
        u32 __base = (u32)(base);                   \
        u32 __rem  = (u32)(((u64)(n)) % __base);     \
        (n) = (u64)((u64)(n) / __base);             \
        __rem;                                      \
    })

static inline u64 uboot_fat_lldiv(u64 dividend, u32 divisor)
{
    return dividend / (u64)divisor;
}
#define lldiv(d, div) uboot_fat_lldiv((d), (div))

static inline u64 uboot_fat_div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
    *remainder = (u32)(dividend % (u64)divisor);
    return dividend / (u64)divisor;
}
#define div_u64_rem(d, div, rem) uboot_fat_div_u64_rem((d), (div), (rem))

/* BUILD_BUG_ON via u-boot's mechanism (an `if` plus __compiletime_error), not _Static_assert,
 * which upstream cannot use for non-constant conditions. No call site; vendored headers may. */
#define UBOOT_FAT_CC_ERROR(msg) __attribute__((__error__(msg)))
#define UBOOT_FAT_CC_ID(prefix, suffix) prefix ## suffix
#define UBOOT_FAT_CC_ASSERT(cond, msg, prefix, suffix)                      \
    do {                                                                    \
        extern void UBOOT_FAT_CC_ID(prefix, suffix)(void)                    \
            UBOOT_FAT_CC_ERROR(msg);                                        \
        if (!(cond))                                                        \
            UBOOT_FAT_CC_ID(prefix, suffix)();                              \
    } while (0)

#ifdef __OPTIMIZE__
#define BUILD_BUG_ON(condition)                                             \
    UBOOT_FAT_CC_ASSERT(!(condition), "BUILD_BUG_ON: " #condition,           \
                        __br_fat_build_bug_on_, __COUNTER__)
#else
#define BUILD_BUG_ON(condition) do { } while (0)
#endif

/* assert(): no-op; upstream fat .c defines neither DEBUG nor CONFIG_LOG, so its own _DEBUG == 0.
 * The expression stays type-checked but is never evaluated at run time. */
#define assert(x) do { if (0) { (void)(x); } } while (0)

/* ---- compile-time config ---- */
/* Explicit values for upstream's Kconfig switches, so its CONFIG_IS_ENABLED branches compile
 * verbatim (no branch deleted): FAT_WRITE=1 leaves fat.c's read-only flush_dirty_fat_buffer stub
 * out (the real one is in fat_write.c); EFI_LOADER=0 leaves fat_readdir timestamps unfilled (no
 * RTC in v1); DM_RTC=0 makes the write path use upstream's 2000-01-01 fallback; FS_EXFAT=0 gives
 * a 256-byte dirent name; FS_FAT_MAX_CLUSTSIZE is u-boot's 32 KiB default for fat.h. */
#define CONFIG_FAT_WRITE           1
#define CONFIG_EFI_LOADER          0
#define CONFIG_DM_RTC              0
#define CONFIG_FS_EXFAT            0
#define CONFIG_FS_FAT_MAX_CLUSTSIZE 32768
#define CONFIG_LOGLEVEL            8
#define CONFIG_LOG                 0
#define CONFIG_LOGF_FUNC           0

/* CONFIG_IS_ENABLED(opt[, case1[, case2]]) / IS_ENABLED(opt): u-boot's variadic choice -- one
 * argument expands to the 0/1 constant, three arguments pick one. */
#define UBOOT_FAT_CONF_(opt)        CONFIG_ ## opt
#define UBOOT_FAT_CONF(opt)         UBOOT_FAT_CONF_(opt)
#define UBOOT_FAT_PICK(_1, _2, _3, NAME, ...) NAME
#define UBOOT_FAT_CE_1(opt)         (UBOOT_FAT_CONF(opt))
#define UBOOT_FAT_CE_2(opt, a)      ((UBOOT_FAT_CONF(opt)) ? (a) : 0)
#define UBOOT_FAT_CE_3(opt, a, b)   ((UBOOT_FAT_CONF(opt)) ? (a) : (b))

#define CONFIG_IS_ENABLED(opt, ...)                                          \
    UBOOT_FAT_PICK(opt, ##__VA_ARGS__, UBOOT_FAT_CE_3, UBOOT_FAT_CE_2,        \
                   UBOOT_FAT_CE_1)(opt, ##__VA_ARGS__)
#define IS_ENABLED(opt)             (opt)

/* Both upstream .c files do `#define LOG_CATEGORY LOGC_FS`; br_log_write takes no category, so
 * LOGC_FS only needs a defined value to keep -Wundef quiet. */
#define LOGC_FS 0

/* ---- logging ---- */
/* printf/log_err always emit at ERROR level, while upstream's own `return -1` still decides
 * continue-vs-return; debug is silent unless UBOOT_FAT_VERBOSE_DBG. "uboot-fat: " fmt is a
 * compile-time concat because upstream's format strings are all literals, and br_log_write's
 * format(printf, ...) attribute still type-checks arguments. Zero variadic args uses GNU
 * ##__VA_ARGS__. */
#ifndef UBOOT_FAT_VERBOSE_DBG
#define UBOOT_FAT_VERBOSE_DBG 0
#endif

#define uboot_fat_log(level, fmt, ...)  br_log_write((level), "uboot-fat: " fmt, ##__VA_ARGS__)

#define debug(fmt, ...)     do { if (UBOOT_FAT_VERBOSE_DBG) {                 \
        br_log_write(BR_LOG_DEBUG, "uboot-fat: " fmt, ##__VA_ARGS__); } } while (0)
#define log_debug(fmt, ...) debug(fmt, ##__VA_ARGS__)
#define log_err(fmt, ...)   br_log_write(BR_LOG_ERROR, "uboot-fat: " fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  br_log_write(BR_LOG_INFO,  "uboot-fat: " fmt, ##__VA_ARGS__)
#define log_warning(fmt, ...) br_log_write(BR_LOG_WARN, "uboot-fat: " fmt, ##__VA_ARGS__)
#define printf(fmt, ...)    br_log_write(BR_LOG_ERROR, "uboot-fat: " fmt, ##__VA_ARGS__)

/* ---- strings (hand-rolled) ---- */
/* static inline with a uboot_fat_ prefix (no dependency on provider/posix); the macros below
 * forward upstream's bare names. Each is bounded by the caller's n, except strcpy (upstream's
 * own 256-byte targets, see below). */
#define UB_FAT_STRCPY_CAP   256u   /* upstream's two strcpy targets are 256 B buffers */
/* strdup cap = UB_FAT_PATH_MAX (256) plus the upstream call site's '/' and '\0' => 258. Longer
 * returns NULL, which upstream turns into -ENOMEM: "path too long" and "out of memory" are the
 * same failure upstream, never a silent truncation. */
#define UB_FAT_STRDUP_CAP   (256u + 2u)
#define UB_FAT_SPRINTF_MAX  256u   /* environment-side cap for the libc-free sprintf below */

static inline size_t uboot_fat_strlen(const char *s)
{
    size_t n = 0u;

    if (s == NULL) {
        return 0u;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static inline size_t uboot_fat_strnlen(const char *s, size_t n)
{
    size_t i = 0u;

    while ((i < n) && (s[i] != '\0')) {
        i++;
    }
    return i;
}

static inline int uboot_fat_strcmp(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline int uboot_fat_strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

/* Bounded copy; the cap includes the trailing '\0'. 256 is upstream's own number: both strcpy
 * call sites target 256-byte buffers. */
static inline char *uboot_fat_strcpy(char *dst, const char *src)
{
    size_t i = 0u;

    while ((i + 1u) < UB_FAT_STRCPY_CAP) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return dst;
        }
        i++;
    }
    dst[UB_FAT_STRCPY_CAP - 1u] = '\0';
    return dst;
}

static inline char *uboot_fat_strchr(const char *s, int c)
{
    const char ch = (char)c;

    for (;; s++) {
        if (*s == ch) {
            return (char *)(br_uintptr_t)s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}

static inline char *uboot_fat_strrchr(const char *s, int c)
{
    const char *last = NULL;
    const char ch = (char)c;

    for (;; s++) {
        if (*s == ch) {
            last = s;
        }
        if (*s == '\0') {
            return (char *)(br_uintptr_t)last;
        }
    }
}

/* ASCII lowercase: 'A'..'Z' only, other bytes returned as-is (upstream assumes an 8-bit
 * codepage). */
static inline int uboot_fat_tolower(int c)
{
    if ((c >= 'A') && (c <= 'Z')) {
        return c + ('a' - 'A');
    }
    return c;
}

static inline int uboot_fat_strcasecmp(const char *a, const char *b)
{
    while (*a != '\0') {
        const int d = uboot_fat_tolower((unsigned char)*a) -
                      uboot_fat_tolower((unsigned char)*b);

        if (d != 0) {
            return d;
        }
        if (*b == '\0') {
            break;
        }
        a++;
        b++;
    }
    return uboot_fat_tolower((unsigned char)*a) - uboot_fat_tolower((unsigned char)*b);
}

static inline int uboot_fat_strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        const int d = uboot_fat_tolower((unsigned char)a[i]) -
                      uboot_fat_tolower((unsigned char)b[i]);

        if (d != 0) {
            return d;
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

/* Upstream's bare names -> our implementations. -fno-builtin, so no self-calls. */
#define strlen(s)            uboot_fat_strlen(s)
#define strnlen(s, n)        uboot_fat_strnlen((s), (n))
#define strcmp(a, b)         uboot_fat_strcmp((a), (b))
#define strncmp(a, b, n)     uboot_fat_strncmp((a), (b), (n))
#define strcpy(d, s)         uboot_fat_strcpy((d), (s))
#define strchr(s, c)         uboot_fat_strchr((s), (c))
#define strrchr(s, c)        uboot_fat_strrchr((s), (c))
#define strcasecmp(a, b)     uboot_fat_strcasecmp((a), (b))
#define strncasecmp(a, b, n) uboot_fat_strncasecmp((a), (b), (n))
#define tolower(c)           uboot_fat_tolower(c)

/* strdup/sprintf/rand live in glue/fat_blk.c: libc-free sprintf caps at UB_FAT_SPRINTF_MAX
 * (upstream's call sites target at most char[13]), and rand is a deterministic 32-bit LCG used
 * only by set_name. Real cross-module symbols, so prototypes here rather than definitions. */
char *uboot_fat_strdup(const char *s);
int   uboot_fat_sprintf(char *dst, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
int   uboot_fat_rand(void);

#define strdup(s)   uboot_fat_strdup(s)
#define sprintf     uboot_fat_sprintf
#define rand()      uboot_fat_rand()

/* ---- block device shim ---- */
/* Same shape as u-boot's blk_dread/blk_dwrite(desc, start, blkcnt, buf): the structs carry only
 * the fields upstream fat.c touches, and the whole device is one partition (start sector 0), so
 * cur_part_info.start + block is always just block. Both live in glue/fat_blk.c, which splits
 * by geometry.max_rw and rejects out-of-range. Return contract: sectors transferred on success,
 * < 0 on failure (so upstream's `ret != nr_blocks` test holds). */
typedef br_u64 lbaint_t;

struct blk_desc {
    int           uclass_id;   /* u-boot: enum uclass_id */
    int           devnum;      /* device number (always 0 here) */
    lbaint_t      lba;         /* total sectors */
    unsigned long blksz;       /* bytes per sector */
    int           log2blksz;
    char          vendor[9];   /* only used by file_fat_detectfs → dev_print (dead code) */
};

#define PART_NAME_LEN 36
#define PART_TYPE_LEN 36

struct disk_partition {
    lbaint_t start;                  /* first sector of the partition (always 0 here) */
    lbaint_t size;                   /* partition sectors = device sectors */
    ulong    blksz;                  /* bytes per sector */
    uchar    name[PART_NAME_LEN];
    uchar    type[PART_TYPE_LEN];
    int      bootable;
};

unsigned long blk_dread (struct blk_desc *desc, lbaint_t start, lbaint_t blkcnt, void *buf);
unsigned long blk_dwrite(struct blk_desc *desc, lbaint_t start, lbaint_t blkcnt, const void *buf);

/* Stubs for calls made only by unreferenced upstream functions (fat_register_device,
 * file_fat_detectfs); static inline, so -ffunction-sections + --gc-sections drop them whole. */
static inline int part_get_info(struct blk_desc *desc, int part, struct disk_partition *info)
{
    (void)desc; (void)part; (void)info;
    return -1;                    /* no partition table here (whole device = one partition) */
}

static inline void disk_partition_clr_uuid(struct disk_partition *info)
{
    (void)info;
}

static inline int blk_enabled(void)
{
    return 0;
}

static inline const char *blk_get_uclass_name(int uclass_id)
{
    (void)uclass_id;
    return "bdev";
}

static inline void dev_print(struct blk_desc *desc)
{
    (void)desc;
}

/* ---- DM/RTC stubs ---- */
/* CONFIG_DM_RTC=0 makes upstream's RTC branch dead code, but it must still type-check. struct
 * rtc_time comes from vendored include/fs.h -> rtc_def.h. */
struct udevice;

#define UCLASS_RTC 0

static inline int uclass_first_device(int uclass_id, struct udevice **devp)
{
    (void)uclass_id;
    if (devp != NULL) {
        *devp = NULL;
    }
    return 0;
}

/* `tm` is void *, not struct rtc_time *: a struct first seen in a prototype is scoped to it, so
 * expanding this header before vendored fs.h would give an incompatible pointer type. */
static inline int dm_rtc_get(struct udevice *dev, void *tm)
{
    (void)dev; (void)tm;
    return -1;
}

/* Stub for upstream fat2rtc(); with CONFIG_EFI_LOADER=0 it is reachable only from dead code cut
 * by --gc-sections, so it exists purely to type-check that upstream text. */
static inline void rtc_calc_weekday(void *tm)
{
    (void)tm;
}

#endif /* BR_ADAPTER_UBOOT_FAT_ENV_H */
