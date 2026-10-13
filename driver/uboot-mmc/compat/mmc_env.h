/*
 * u-boot runtime environment for the upstream `drivers/mmc/` sources: every `compat/<u-boot
 * header>` shim lands here, so not one line of upstream .c changes. `udelay()`/`get_timer()` are
 * "sleep until next ms or a device IRQ", which turns upstream's polling loops into IRQ-driven
 * waits; `uboot_mmc_env_reg_touched()` reports the MMIO address just accessed so glue can map it
 * back to the waiting card. `glue/` implements the three declared hooks; `compat/` never includes
 * `glue/`. This file declares no upstream data type (`compat/mmc.h`/`sdhci.h` forward those).
 */
#ifndef BR_ADAPTER_UBOOT_MMC_ENV_H
#define BR_ADAPTER_UBOOT_MMC_ENV_H

#include <stddef.h>     /* NULL / size_t / offsetof -- compiler-provided, not libc */
#include <stdbool.h>    /* bool/true/false -- u-boot's linux/types.h does the same */
#include <stdint.h>     /* uint8_t/uint32_t/uint64_t (blk.h's lbaint_t uses these) */

#include <br/core/br_error.h>   /* errno names and values (ADR-0027: names = POSIX originals) */
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* Config matrix, same role as `generated/autoconf.h`; must precede any header using
 * CONFIG_IS_ENABLED(). */
#include <linux/kconfig.h>

/* Compile-time premises this adapter relies on. `#ifdef` tests definedness, so the two shape
 * switches must be !defined, not `== 0`. */
#ifdef CONFIG_DM_MMC
#error "CONFIG_DM_MMC must **not be defined**: it makes upstream sdhci.c take the DM/udevice shape"
#endif
#ifdef CONFIG_BLK
#error "CONFIG_BLK must **not be defined**: it makes upstream blk_desc/mmc_bread take the uclass shape"
#endif
#if !defined(CONFIG_MMC_SDHCI_ADMA) || !defined(CONFIG_MMC_SDHCI_ADMA_HELPERS)
#error "this adapter's sources include sdhci-adma.c ⇒ ADMA and ADMA_HELPERS must be enabled"
#endif
#if !defined(CONFIG_MMC_WRITE)
#error "this adapter's sources include mmc_write.c ⇒ CONFIG_MMC_WRITE must be enabled"
#endif

/* u64/s64 are `unsigned/signed long long`, not `uint64_t` (same width, different type on LP64);
 * upstream formatting/assignments assume `long long`. */
typedef br_u8   u8;
typedef br_s8   s8;
typedef br_u16  u16;
typedef br_s16  s16;
typedef br_u32  u32;
typedef br_s32  s32;
typedef unsigned long long u64;
typedef signed long long   s64;

typedef unsigned char  uchar;
typedef unsigned short ushort;
typedef unsigned int   uint;
typedef unsigned long  ulong;

/* Both 64-bit on aarch64; identity map => they are the virtual/physical addresses themselves. */
typedef unsigned long phys_addr_t;
typedef unsigned long dma_addr_t;

/* Byte order: `__leN` is identity on this little-endian target, `__beN` is `__swabN`, and
 * `be32_to_cpu`/`le32_to_cpu` are lowercase aliases. Upstream uses `__be32_to_cpu` (SCR) and
 * `be32_to_cpu` (SSR).
 */
static inline u16 uboot_mmc_swab16(u16 x) { return __builtin_bswap16(x); }
static inline u32 uboot_mmc_swab32(u32 x) { return __builtin_bswap32(x); }
static inline u64 uboot_mmc_swab64(u64 x) { return __builtin_bswap64(x); }
#define __swab16(x) ((u16)uboot_mmc_swab16((u16)(x)))
#define __swab32(x) ((u32)uboot_mmc_swab32((u32)(x)))
#define __swab64(x) ((u64)uboot_mmc_swab64((u64)(x)))

#define __cpu_to_le16(x) ((__le16)(u16)(x))
#define __le16_to_cpu(x) ((u16)(__le16)(x))
#define __cpu_to_le32(x) ((__le32)(u32)(x))
#define __le32_to_cpu(x) ((u32)(__le32)(x))
#define __cpu_to_le64(x) ((__le64)(u64)(x))
#define __le64_to_cpu(x) ((u64)(__le64)(x))
#define __cpu_to_be16(x) ((__be16)__swab16((u16)(x)))
#define __be16_to_cpu(x) ((u16)__swab16((u16)(x)))
#define __cpu_to_be32(x) ((__be32)__swab32((u32)(x)))
#define __be32_to_cpu(x) ((u32)__swab32((u32)(x)))
#define __cpu_to_be64(x) ((__be64)__swab64((u64)(x)))
#define __be64_to_cpu(x) ((u64)__swab64((u64)(x)))

#define cpu_to_le16 __cpu_to_le16
#define le16_to_cpu __le16_to_cpu
#define cpu_to_le32 __cpu_to_le32
#define le32_to_cpu __le32_to_cpu
#define cpu_to_le64 __cpu_to_le64
#define le64_to_cpu __le64_to_cpu
#define cpu_to_be16 __cpu_to_be16
#define be16_to_cpu __be16_to_cpu
#define cpu_to_be32 __cpu_to_be32
#define be32_to_cpu __be32_to_cpu
#define cpu_to_be64 __cpu_to_be64
#define be64_to_cpu __be64_to_cpu

/* u-boot's `__uN`/`__leN`/`__beN` families; sparse `__bitwise` annotations dropped (static
 * checks only on aarch64). */
typedef br_u8  __u8;   typedef br_u16 __u16;  typedef br_u32 __u32;  typedef br_u64 __u64;
typedef br_s8  __s8;   typedef br_s16 __s16;  typedef br_s32 __s32;  typedef br_s64 __s64;
typedef br_u16 __le16; typedef br_u32 __le32; typedef br_u64 __le64;
typedef br_u16 __be16; typedef br_u32 __be32; typedef br_u64 __be64;

/* Present in u-boot's `linux/types.h`; used indirectly by `mmc.h`. */
typedef long long      loff_t;
typedef unsigned int   dev_t;
typedef unsigned short umode_t;

/* width self-check */
_Static_assert(sizeof(u8)  == 1u, "u8 must be 8 bits");
_Static_assert(sizeof(u16) == 2u, "u16 must be 16 bits");
_Static_assert(sizeof(u32) == 4u, "u32 must be 32 bits");
_Static_assert(sizeof(u64) == 8u, "u64 must be 64 bits");

/* The identity `__leN` / swapping `__beN` above hold only on a little-endian target; a
 * big-endian target would be silently wrong, so stop here. */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-mmc implements leN_to_cpu()/beN_to_cpu() for little-endian targets only"
#endif

/* u-boot's `linux/compiler*.h` family is not vendored (it needs `__KERNEL__` and build-time
 * probes); only the attributes upstream actually uses are given. */
#define __weak           __attribute__((weak))
#define __maybe_unused   __attribute__((unused))
#define __packed         __attribute__((__packed__))
#define __aligned(x)     __attribute__((aligned(x)))
#define __iomem
#define __force
#define __user
#define __must_check
#define __visible        __attribute__((visibility("default")))
#define __always_inline  inline __attribute__((always_inline))

/* ---- bit ops/utilities ---- */
#define BITS_PER_LONG        64
#define BITS_PER_LONG_LONG   64
#define BITS_PER_BYTE        8

#define BIT(nr)              (1UL << (nr))
#define BIT_ULL(nr)          (1ULL << (nr))
#define BIT_MASK(nr)         (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)         ((nr) / BITS_PER_LONG)
#define GENMASK(h, l)        (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l)    (((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))

/* fls/ffs from u-boot `<linux/bitops.h>` (same semantics) */
static inline int uboot_mmc_ffs(int x)
{
    int r = 1;

    if (x == 0) {
        return 0;
    }
    if ((x & 0xffff) == 0) { x >>= 16; r += 16; }
    if ((x & 0xff) == 0)   { x >>= 8;  r += 8; }
    if ((x & 0xf) == 0)    { x >>= 4;  r += 4; }
    if ((x & 3) == 0)      { x >>= 2;  r += 2; }
    if ((x & 1) == 0)      { x >>= 1;  r += 1; }
    return r;
}

static inline int uboot_mmc_fls(int x)
{
    int r = 32;

    if (x == 0) {
        return 0;
    }
    if ((x & (int)0xffff0000u) == 0) { x <<= 16; r -= 16; }
    if ((x & (int)0xff000000u) == 0) { x <<= 8;  r -= 8; }
    if ((x & (int)0xf0000000u) == 0) { x <<= 4;  r -= 4; }
    if ((x & (int)0xc0000000u) == 0) { x <<= 2;  r -= 2; }
    if ((x & (int)0x80000000u) == 0) { x <<= 1;  r -= 1; }
    return r;
}
#define ffs(x) uboot_mmc_ffs(x)
#define fls(x) uboot_mmc_fls(x)

#define ARRAY_SIZE(x)     (sizeof(x) / sizeof((x)[0]))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1u) / (d))
#define ROUND(a, b)       (((a) + (b) - 1u) & ~((b) - 1u))
#define ALIGN(x, a)       (((x) + ((a) - 1u)) & ~((a) - 1u))
#define ALIGN_DOWN(x, a)  ((x) & ~((a) - 1u))
#define IS_ALIGNED(x, a)  (((x) & ((typeof(x))(a) - 1u)) == 0u)
#define __ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))

/* Each argument is evaluated once and types are checked: upstream has side-effecting
 * `max(a, b())` style arguments, which a plain ternary would evaluate twice. */
#define min(x, y) ({                             \
        typeof(x) _min1 = (x);                   \
        typeof(y) _min2 = (y);                   \
        (void)(&_min1 == &_min2);                \
        _min1 < _min2 ? _min1 : _min2; })
#define max(x, y) ({                             \
        typeof(x) _max1 = (x);                   \
        typeof(y) _max2 = (y);                   \
        (void)(&_max1 == &_max2);                \
        _max1 > _max2 ? _max1 : _max2; })
#define min_t(type, x, y) ({                     \
        type _min1_t = (type)(x);                \
        type _min2_t = (type)(y);                \
        _min1_t < _min2_t ? _min1_t : _min2_t; })
#define max_t(type, x, y) ({                     \
        type _max1_t = (type)(x);                \
        type _max2_t = (type)(y);                \
        _max1_t > _max2_t ? _max1_t : _max2_t; })
#define clamp(v, lo, hi) min((typeof(v))max((v), (lo)), (hi))

#define likely(x)         __builtin_expect(!!(x), 1)
#define unlikely(x)       __builtin_expect(!!(x), 0)

/* Verbatim from upstream: the ADMA2 descriptor `addr_lo` and `SDHCI_ADMA_ADDRESS` both rely on
 * `lower_32_bits()`. */
#define lower_32_bits(n)  ((u32)((n) & 0xffffffff))
#define upper_32_bits(n)  ((u32)(((n) >> 16) >> 16))
#define lower_16_bits(n)  ((u16)(n))

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Subset of u-boot `<linux/sizes.h>` (upstream mmc.c's SD SSR table); values match upstream. */
#define SZ_1    0x00000001
#define SZ_2    0x00000002
#define SZ_4    0x00000004
#define SZ_8    0x00000008
#define SZ_16   0x00000010
#define SZ_32   0x00000020
#define SZ_64   0x00000040
#define SZ_128  0x00000080
#define SZ_256  0x00000100
#define SZ_512  0x00000200
#define SZ_1K   0x00000400
#define SZ_2K   0x00000800
#define SZ_4K   0x00001000
#define SZ_8K   0x00002000
#define SZ_16K  0x00004000
#define SZ_32K  0x00008000
#define SZ_64K  0x00010000
#define SZ_128K 0x00020000
#define SZ_256K 0x00040000
#define SZ_512K 0x00080000
#define SZ_1M   0x00100000
#define SZ_2M   0x00200000
#define SZ_4M   0x00400000
#define SZ_8M   0x00800000
#define SZ_16M  0x01000000
#define SZ_32M  0x02000000
#define SZ_64M  0x04000000

/* 64-bit division helpers (u-boot `div64.h` + the BITS_PER_LONG==64 branch of `linux/math64.h`):
 * `do_div(n, base)` works **in place** and returns the remainder (call sites rely on that);
 * `div_u64_rem()` takes a remainder out-param; `lldiv()` wraps `do_div` without modifying it. */
#define do_div(n, base)                     \
    ({                                      \
        u32 __base = (u32)(base);           \
        u32 __rem  = (u32)((u64)(n) % __base); \
        (n) = (u64)(n) / __base;            \
        __rem;                              \
    })

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
    *remainder = (u32)(dividend % divisor);
    return dividend / divisor;
}

static inline u64 lldiv(u64 dividend, u32 divisor)
{
    u64 res = dividend;

    (void)do_div(res, divisor);
    return res;
}

/* Upstream uses `printf`/`puts`/`sprintf` plus `log_*`/`pr_*`; this adapter has no libc printf, so
 * a tiny formatter covers the conversions upstream actually uses (`%d %i %u %x %X %c %s %p %%`,
 * flags `#`/`0`, width, length `l`/`ll`/`z`). No `format(printf)` attribute: upstream passes `uint`
 * to `%d`, and the attribute is on the layer forwarding to core only. Output goes to core's log
 * (BR_LOG_INFO), sharing one stream with the kernel log. */
#define UBOOT_MMC_LOG_MAX 256u

/* Unsigned integer -> buffer, reversed digit table. Returns the new write pointer. */
static inline char *uboot_mmc_fmt_uint(char *p, char *end, unsigned long long v,
                                      unsigned base, bool upper)
{
    char     tmp[24];
    unsigned n = 0u;

    if (v == 0u) {
        tmp[n++] = '0';
    }
    while (v != 0u) {
        const unsigned d = (unsigned)(v % (unsigned long long)base);
        tmp[n++] = (char)((d < 10u) ? ('0' + (char)d)
                                    : ((upper ? 'A' : 'a') + (char)(d - 10u)));
        v /= (unsigned long long)base;
    }
    while (n > 0u) {
        n--;
        if (p < end) {
            *p++ = tmp[n];
        }
    }
    return p;
}

/* `cap` includes the trailing NUL; overflow truncates silently. Unimplemented modifiers are
 * emitted verbatim. */
static inline int uboot_mmc_vformat(char *dst, br_size_t cap, const char *fmt,
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
        bool alt   = false;
        bool zero  = false;
        while ((*fmt == '#') || (*fmt == '0')) {
            if (*fmt == '#') { alt = true; }
            else             { zero = true; }
            fmt++;
        }
        int width = 0;
        while ((*fmt >= '0') && (*fmt <= '9')) {
            width = (width * 10) + (int)(*fmt - '0');
            fmt++;
        }
        unsigned lcount = 0u;              /* 0 = int, 1 = long/z, 2 = long long */
        while (*fmt == 'l') { lcount++; fmt++; }
        if ((lcount == 0u) && (*fmt == 'z')) { lcount = 1u; fmt++; }

        char *const mark = p;              /* start of this conversion (needed for width padding) */

        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (s == NULL) { s = "(null)"; }
            while (*s != '\0') {
                if (p < end) { *p++ = *s; }
                s++;
            }
            break;
        }
        case 'c': {
            const int c = __builtin_va_arg(ap, int);
            if (p < end) { *p++ = (char)c; }
            break;
        }
        case 'd': case 'i': {
            long long v;
            if (lcount >= 2u)      { v = __builtin_va_arg(ap, long long); }
            else if (lcount == 1u) { v = (long long)__builtin_va_arg(ap, long); }
            else                   { v = (long long)__builtin_va_arg(ap, int); }
            unsigned long long u = (unsigned long long)v;
            if (v < 0) {
                if (p < end) { *p++ = '-'; }
                u = ~u + 1ull;
            }
            if (width > 0) {
                unsigned digits = 0u;
                unsigned long long t = u;
                do { digits++; t /= 10ull; } while (t != 0ull);
                unsigned used = digits + ((v < 0) ? 1u : 0u);
                while (used < (unsigned)width) {
                    if (p < end) { *p++ = zero ? '0' : ' '; }
                    used++;
                }
            }
            p = uboot_mmc_fmt_uint(p, end, u, 10u, false);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long long v;
            if (lcount >= 2u)      { v = __builtin_va_arg(ap, unsigned long long); }
            else if (lcount == 1u) { v = (unsigned long long)__builtin_va_arg(ap, unsigned long); }
            else                   { v = (unsigned long long)__builtin_va_arg(ap, unsigned int); }
            const unsigned base = (*fmt == 'u') ? 10u : 16u;
            const unsigned long long pfx = (alt && (base == 16u) && (v != 0ull)) ? 2ull : 0ull;
            if (width > 0) {
                unsigned digits = 0u;
                unsigned long long t = v;
                do { digits++; t /= (unsigned long long)base; } while (t != 0ull);
                unsigned used = digits + (unsigned)pfx;
                while (used < (unsigned)width) {
                    if (p < end) { *p++ = zero ? '0' : ' '; }
                    used++;
                }
            }
            if (pfx != 0ull) {
                if (p < end) { *p++ = '0'; }
                if (p < end) { *p++ = (*fmt == 'X') ? 'X' : 'x'; }
            }
            p = uboot_mmc_fmt_uint(p, end, v, base, (*fmt == 'X'));
            break;
        }
        case 'p': {
            const void *q = __builtin_va_arg(ap, void *);
            if (p < end) { *p++ = '0'; }
            if (p < end) { *p++ = 'x'; }
            p = uboot_mmc_fmt_uint(p, end, (unsigned long long)(br_uintptr_t)q, 16u, false);
            break;
        }
        case '%':
            if (p < end) { *p++ = '%'; }
            break;
        case '\0':                          /* trailing '%': finish as-is */
            fmt--;
            break;
        default:                            /* unimplemented modifier: emit verbatim */
            if (p < end) { *p++ = '%'; }
            if (p < end) { *p++ = *fmt; }
            break;
        }
        (void)mark;

        if (*fmt != '\0') { fmt++; }
    }

    *p = '\0';
    return (int)(br_size_t)(p - dst);
}

/* Not `__builtin_vsnprintf`: it still calls libc `vsnprintf` at link time. */
static inline int printf(const char *fmt, ...)
{
    char line[UBOOT_MMC_LOG_MAX];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    (void)uboot_mmc_vformat(line, sizeof(line), fmt, ap);
    __builtin_va_end(ap);
    /* upstream format strings carry their own '\n'; br_log_write is per-line, so one extra */
    br_log_write(BR_LOG_INFO, "%s", line);
    return 0;
}

static inline int sprintf(char *dst, const char *fmt, ...)
{
    __builtin_va_list ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = uboot_mmc_vformat(dst, 256u, fmt, ap);   /* upstream call-site buffers are ≥ 9 B; 256 is a safe upper bound */
    __builtin_va_end(ap);
    return n;
}

static inline int puts(const char *s)
{
    br_log_write(BR_LOG_INFO, "%s", (s != NULL) ? s : "");
    return 0;
}

/* ---- u-boot's log_* / pr_* / debug ----
 * Both families are upstream's, so this file supplies them, but they must differ: `log_*`
 * (include/log.h) do no compile-time filtering, so forward straight to `br_log_write` (core's
 * runtime level filter). `pr_*` (linux/printk.h) filter by `CONFIG_LOGLEVEL` with the upstream
 * thresholds >0/>1/.../>7; with `CONFIG_LOGLEVEL` = 4, `pr_err` is kept and `pr_warn` cut.
 * The compile-time cut is required: `mmc.c:175` compiles out `mmc_mode_name()` under the same
 * condition and all its call sites are inside `pr_debug(...)` arguments, so forwarding
 * unconditionally would leave an undefined reference to `mmc_mode_name`. */
#define UBOOT_MMC_LOG_GATED(cond, level, ...) \
    ({ (cond) ? br_log_write((level), __VA_ARGS__) : 0; })

/* runtime filtering (no compile-time cut) */
#define UBOOT_MMC_LOGF(level, ...) br_log_write((level), __VA_ARGS__)
#define log_emerg(...)    UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_alert(...)    UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_crit(...)     UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_err(...)      UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_warning(...)  UBOOT_MMC_LOGF(BR_LOG_WARN,  __VA_ARGS__)
#define log_notice(...)   UBOOT_MMC_LOGF(BR_LOG_INFO,  __VA_ARGS__)
#define log_info(...)     UBOOT_MMC_LOGF(BR_LOG_INFO,  __VA_ARGS__)
#define log_debug(...)    UBOOT_MMC_LOGF(BR_LOG_DEBUG, __VA_ARGS__)

/* compile-time filtering (thresholds as upstream printk.h) */
#define pr_emerg(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 0, BR_LOG_ERROR, __VA_ARGS__)
#define pr_alert(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 1, BR_LOG_ERROR, __VA_ARGS__)
#define pr_crit(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 2, BR_LOG_ERROR, __VA_ARGS__)
#define pr_err(...)       UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 3, BR_LOG_ERROR, __VA_ARGS__)
#define pr_warn(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 4, BR_LOG_WARN,  __VA_ARGS__)
#define pr_notice(...)    UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 5, BR_LOG_INFO,  __VA_ARGS__)
#define pr_info(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 6, BR_LOG_INFO,  __VA_ARGS__)
#define pr_debug(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 7, BR_LOG_DEBUG, __VA_ARGS__)
#define pr_devel(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 7, BR_LOG_DEBUG, __VA_ARGS__)

/* `_DEBUG` is 0 here; upstream's `debug_cond` relies on -O2 DCE, this folds at any -O level. */
#define _DEBUG            0
#define debug(...)        UBOOT_MMC_LOG_GATED(_DEBUG != 0, BR_LOG_DEBUG, __VA_ARGS__)

/* Same values as upstream `enum log_level_t`. They must be macros, not an enum: upstream
 * `mmc.c:175` uses `LOGL_DEBUG` inside an `#if`, where only macros exist (and `-Wundef -Werror`
 * demands it). */
#define LOGL_EMERG          0
#define LOGL_ALERT          1
#define LOGL_CRIT           2
#define LOGL_ERR            3
#define LOGL_WARNING        4
#define LOGL_NOTICE         5
#define LOGL_INFO           6
#define LOGL_DEBUG          7
#define LOGL_DEBUG_CONTENT  8
#define LOGL_DEBUG_IO       9
#define LOGL_COUNT          10
#define LOGL_NONE           11
#define LOGL_LEVEL_MASK     0xf
#define LOGL_FORCE_DEBUG    0x10
#define LOGL_FIRST          LOGL_EMERG
#define LOGL_MAX            LOGL_DEBUG_IO
#define LOGL_CONT           (-1)

/* memcpy/memset/memcmp/memmove come from core and are used directly; core's `string_internal.h` is
 * private, so the prototypes are repeated here for type checking. */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* u-boot `malloc.h` wired to the core heap. Macros, not functions, so products that do not select
 * runtime/posix still link. */
#define malloc(n)        br_malloc((br_size_t)(n))
#define calloc(n, s)     br_calloc((br_size_t)(n), (br_size_t)(s))
#define realloc(p, n)    br_realloc((p), (br_size_t)(n))
#define free(p)          br_free(p)

/* u-boot `memalign.h`: the ADMA2 descriptor table is read by the device, so glue supplies it from
 * the DMA pool (uncached/contiguous), not br_malloc. */
void *uboot_mmc_env_memalign(br_size_t align, br_size_t size);
#define memalign(align, size) uboot_mmc_env_memalign((align), (size))

/* Upstream's bare `-EIO`/`-ETIMEDOUT`/... come from `br_error.h`, which carries the POSIX-original
 * errno names, so those sources need no change. */

/* ---- errno names upstream uses that `br_error.h` does not yet carry ----
 * Five names used by the upstream `drivers/mmc/` originals are absent from core, so they are added
 * here, values verbatim from u-boot `include/linux/errno.h`. They belong back in `br_error.h`
 * eventually; `tools/check-errno.sh` only enforces the names already there. */
#define ECOMM        70    /* Communication error on send */
#define EBADMSG      74    /* Not a data message */
#define ENOMEDIUM   123    /* No medium found */
#define EMEDIUMTYPE 124    /* Wrong medium type */
#define ENOTSUPP    524    /* Operation is not supported (kernel-internal code, not uapi) */

/* `udelay()`/`get_timer()` are a sleep wait that a device IRQ can wake early, so upstream's
 * polling loops become IRQ-driven. Glue provides three hooks: `uboot_mmc_env_wait(max_us)` yields
 * for at most max_us or until the current device's IRQ (re-enabling `SIGNAL_ENABLE` first, which
 * the ISR masks); `uboot_mmc_env_spin(us)` busy-waits without yielding, because upstream uses
 * `udelay()` only for "register stable/reset complete" (10/100/1000/2000 us) and one tick of
 * sleep would inflate those to 5 ms; `uboot_mmc_env_reg_touched()` records the MMIO address just
 * accessed for the device lookup (upstream's wait loops have no device argument). */
void uboot_mmc_env_wait(br_u32 max_us);
void uboot_mmc_env_spin(br_u32 us);
void uboot_mmc_env_reg_touched(const volatile void *reg);

/* Upper bound on a single yield window (1 ms); only the wait-for-device-event path uses it
 * fully. */
#define UBOOT_MMC_WAIT_MAX_US   1000u

/* One "yield the CPU" window: at most max_us microseconds, or a device IRQ. */
static inline void uboot_mmc_yield_once(br_u32 max_us)
{
    uboot_mmc_env_wait(max_us);
}

/* `udelay(us)`: u-boot semantics are a busy wait, and upstream uses it only for 10/100/1000/2000 us
 * register-stable/reset waits -- do not yield, else a us spin becomes one 5 ms tick. */
static inline void udelay(unsigned long usec)
{
    uboot_mmc_env_spin((br_u32)usec);
}

/* `mdelay(ms)`: upstream waits for the card (`mmc_send_op_cond`/`mmc_complete_op_cond`, 5/10 ms),
 * a device event => yield the CPU in 1 ms slices, woken early by an IRQ. */
static inline void mdelay(unsigned long msec)
{
    const br_time_t deadline = br_deadline_from_now((br_time_t)msec * (br_time_t)1000u);

    for (;;) {
        const br_time_t now  = br_clock_now();
        const br_time_t left = (deadline > now) ? (deadline - now) : 0;

        if (left == 0) {
            break;
        }
        uboot_mmc_yield_once((left > (br_time_t)UBOOT_MMC_WAIT_MAX_US)
                                 ? UBOOT_MMC_WAIT_MAX_US
                                 : (br_u32)left);
    }
}

/* `get_timer(base)`: milliseconds since base. Every call yields the CPU first (until the next ms or
 * a device IRQ), so upstream's polling metronome becomes IRQ-driven. */
static inline ulong get_timer(ulong base)
{
    uboot_mmc_yield_once(UBOOT_MMC_WAIT_MAX_US);
    return (ulong)(br_clock_now() / (br_time_t)BR_US_PER_MS) - base;
}

/* u-boot's microsecond read (`include/time.h`); `time_after()` is a wrap-safe unsigned compare. */
static inline ulong timer_get_us(void)
{
    return (ulong)br_clock_now();
}
#define time_after(a, b) ((long)((b) - (a)) < 0)

/* Upstream `sdhci.h`'s `sdhci_readl/writel/...` (`MMC_SDHCI_IO_ACCESSORS` off) land on
 * `readl(host->ioaddr + reg)`. Every access reports the address to
 * `uboot_mmc_env_reg_touched()`, which is how a wait finds its card; `-mstrict-align` and
 * Device-nGnRnE memory force access by width. */
static inline u32 uboot_mmc_readl(const volatile void *addr)
{
    uboot_mmc_env_reg_touched(addr);
    return *(const volatile u32 *)addr;
}
static inline u16 uboot_mmc_readw(const volatile void *addr)
{
    uboot_mmc_env_reg_touched(addr);
    return *(const volatile u16 *)addr;
}
static inline u8 uboot_mmc_readb(const volatile void *addr)
{
    uboot_mmc_env_reg_touched(addr);
    return *(const volatile u8 *)addr;
}
static inline void uboot_mmc_writel(u32 v, volatile void *addr)
{
    uboot_mmc_env_reg_touched(addr);
    *(volatile u32 *)addr = v;
}
static inline void uboot_mmc_writew(u16 v, volatile void *addr)
{
    uboot_mmc_env_reg_touched(addr);
    *(volatile u16 *)addr = v;
}
static inline void uboot_mmc_writeb(u8 v, volatile void *addr)
{
    uboot_mmc_env_reg_touched(addr);
    *(volatile u8 *)addr = v;
}
#define readl(a)  uboot_mmc_readl(a)
#define readw(a)  uboot_mmc_readw(a)
#define readb(a)  uboot_mmc_readb(a)
#define writel(v, a) uboot_mmc_writel((v), (a))
#define writew(v, a) uboot_mmc_writew((v), (a))
#define writeb(v, a) uboot_mmc_writeb((v), (a))

/* u-boot `cpu_func.h`/`asm/cache.h`/`linux/dma-mapping.h`: `ARCH_DMA_MINALIGN` = 64 B matches the
 * Cortex-A53 L1 D-cache line (QEMU virt); `flush_cache`/`flush_dcache_range`/
 * `invalidate_dcache_range` map to core's `br_mm_cache_*` (no-op on an uncached region);
 * `dma_map_single`/`dma_unmap_single` do per-direction cache maintenance and identity-map. */
#define ARCH_DMA_MINALIGN  64ul

static inline void flush_dcache_range(unsigned long start, unsigned long stop)
{
    (void)br_mm_cache_flush((void *)(br_uintptr_t)start, (br_size_t)(stop - start));
}
static inline void invalidate_dcache_range(unsigned long start, unsigned long stop)
{
    (void)br_mm_cache_invalidate((void *)(br_uintptr_t)start, (br_size_t)(stop - start));
}
static inline void flush_cache(phys_addr_t addr, unsigned long size)
{
    (void)br_mm_cache_flush((void *)(br_uintptr_t)addr, (br_size_t)size);
}
static inline void invalidate_cache(phys_addr_t addr, unsigned long size)
{
    (void)br_mm_cache_invalidate((void *)(br_uintptr_t)addr, (br_size_t)size);
}

/* `linux/dma-direction.h` (matching upstream) */
enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE     = 1,
    DMA_FROM_DEVICE   = 2,
    DMA_NONE          = 3,
};

static inline dma_addr_t dma_map_single(void *vaddr, size_t len,
                                       enum dma_data_direction dir)
{
    const unsigned long addr = (unsigned long)(br_uintptr_t)vaddr;
    const unsigned long n    = (unsigned long)ALIGN((unsigned long)len, ARCH_DMA_MINALIGN);

    if (dir == DMA_FROM_DEVICE) {
        invalidate_dcache_range(addr, addr + n);
    } else {
        flush_dcache_range(addr, addr + n);
    }
    return (dma_addr_t)addr;
}

static inline void dma_unmap_single(dma_addr_t addr, size_t len,
                                    enum dma_data_direction dir)
{
    const unsigned long n = (unsigned long)ALIGN((unsigned long)len, ARCH_DMA_MINALIGN);

    if (dir != DMA_TO_DEVICE) {
        invalidate_dcache_range((unsigned long)addr, (unsigned long)addr + n);
    }
}

#define dma_mapping_error(x, y) 0

/* `phys2bus.h`: the `!CONFIG_PHYS_TO_BUS` and `!DM` branches both degenerate to identity */
#define dev_phys_to_bus(_dev, addr) ((dma_addr_t)(addr))
#define dev_bus_to_phys(_dev, addr) ((phys_addr_t)(addr))
static inline unsigned long phys_to_bus(unsigned long phys) { return phys; }
static inline unsigned long bus_to_phys(unsigned long bus)  { return bus; }

/* u-boot's `virt_to_phys()`: identity here (`sdhci_setup_cfg` uses it for the ADMA table
 * address). */
#define virt_to_phys(addr) ((phys_addr_t)(br_uintptr_t)(addr))

/* `struct sdhci_host` embeds two `struct gpio_desc` by value, so the type must exist; neither
 * DM_GPIO nor DM_MMC is enabled, so nothing uses them. Matches upstream field by field. */
struct udevice;                 /* forward declaration: no DM here */

struct gpio_desc {
    struct udevice *dev;        /* Device, NULL for invalid GPIO */
    unsigned long   flags;
#define GPIOD_IS_OUT        BIT(1)
#define GPIOD_IS_IN         BIT(2)
#define GPIOD_ACTIVE_LOW    BIT(3)
#define GPIOD_IS_OUT_ACTIVE BIT(4)
#define GPIOD_OPEN_DRAIN    BIT(5)
#define GPIOD_OPEN_SOURCE   BIT(6)
#define GPIOD_PULL_UP       BIT(7)
#define GPIOD_PULL_DOWN     BIT(8)
#define GPIOD_IS_AF         BIT(9)
    uint offset;                /* GPIO offset within the device */
};

/* Upstream `mmc.h` declares `struct bd_info;` itself; `mmc_initialize()`'s signature needs it. */
struct bd_info;

#endif /* BR_ADAPTER_UBOOT_MMC_ENV_H */
