/*
 * adapter/uboot-mmc -- **u-boot runtime environment** (adapter layer; ADR-0026 D-3)
 *
 * ============================== WHAT THIS FILE IS ==============================
 * Upstream `drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c` are **u-boot style** C:
 * they assume `linux/types.h` `u8/u32/bool`, `malloc.h`, `log.h` `log_err/pr_debug`,
 * `cpu_func.h` `flush_dcache_range`, `udelay()`/`get_timer()`, `asm/io.h`
 * `readl/writel`... This plugin is a **bare-metal unikernel (no libc)**, so none of it exists.
 *
 * This file is the **single landing point** for that environment on brickOS: every thin shim in
 * `compat/<u-boot header name>` points here, and **not one line of upstream .c changes** to
 * compile. This is "give upstream the environment it depends on", not
 * "change upstream to fit us" (the ADR-0026 D-3 criterion).
 *
 *   u-boot side                        →  brickOS side
 *   ---------------------------------------------------------------------------
 *   u8/u16/u32/u64/bool                →  br_u8/... / `<stdbool.h>`
 *   malloc/calloc/free                 →  br_malloc/br_calloc/br_free
 *   memalign(alignment + DMA safe)     →  br_dma_alloc(BR_DMA_F_ALIGN4K, ...) (from glue)
 *   memcpy/memset/memcmp/memmove       →  core/src/string.c (discipline: use directly)
 *   printf/sprintf/puts                →  tiny formatter here → br_log_write / buffer
 *   log_err/log_warning/log_info/...   →  br_log_error/warn/info/debug
 *   pr_err/pr_info/pr_debug            →  same as above
 *   bare E* names (-EIO/-ETIMEDOUT/...)→  **core `<br/core/br_error.h>`** (ADR-0027:
 *                                          names are the POSIX originals, so upstream holds)
 *   udelay/mdelay/get_timer            →  ★ **sleep wait woken early by device IRQ** (below)
 *   readl/writel/readw/...             →  direct MMIO + record "window just touched" (below)
 *   flush_dcache_range/flush_cache     →  br_mm_cache_flush / br_mm_cache_invalidate
 *   dma_map_single/dma_unmap_single    →  identity map + per-direction cache maintenance
 *   dev_phys_to_bus(dev = NULL)        →  identity (v1 identity map ⇒ dma_addr == vaddr)
 *
 * ============ ★ CORE TRICK: upstream **polling waits** become **IRQ-driven sleep waits** ============
 * The two wait paths in upstream `sdhci.c` (task spec §2.4):
 *
 *   - command completion: `start = get_timer(0); do { stat = sdhci_readl(host, SDHCI_INT_STATUS);
 *     ...; if (get_timer(start) >= SDHCI_READ_STATUS_TIMEOUT) ... } while ((stat & mask) != mask);`
 *   - data transfer: `do { stat = sdhci_readl(host, SDHCI_INT_STATUS); ...;
 *     if (timeout-- > 0) udelay(10); ... } while (!(stat & SDHCI_INT_DATA_END));`
 *
 * Upstream yields the CPU **only** via `get_timer()` / `udelay()`. So this environment layer
 * implements those two as "**yield the CPU until the next ms or a device IRQ**" -- upstream's
 * polling loop then automatically becomes an **IRQ-driven** wait: "sleep → device IRQ → wake →
 * read INT_STATUS again", with **not one line of upstream code changed**. That is the core trick.
 *
 * The "device IRQ" is provided by glue (`glue/emmc_bdev.c` `uboot_mmc_env_wait()`):
 *   { ISR: read INT_STATUS + write SIGNAL_ENABLE 0 to deassert the level line + br_work_submit() to bh }
 *   { bh:  br_sem_give() }
 *   { this file's wait primitive: **re-enable SIGNAL_ENABLE** + sleep (so each round can be woken) }
 * This is ADR-0021 D-4 realized under "upstream compiles unchanged".
 *
 * ★ How the environment layer knows which card is being waited on: upstream's wait loop has no
 *   device argument. So the MMIO accessors report **the address just touched** to glue
 *   (`uboot_mmc_env_reg_touched()`), which maps it back window → instance. Concurrent waits on
 *   several cards thus each attach to their own IRQ channel (not a "global singleton").
 *
 * ============================== DEPENDENCY DIRECTION ==============================
 * `compat/` is the environment, `glue/` is the implementation. ⇒ This file **only declares** the
 * three hooks glue must provide (`uboot_mmc_env_wait` / `uboot_mmc_env_reg_touched` /
 * `uboot_mmc_env_memalign`) and includes nothing under `glue/`. The reverse direction
 * (glue → compat) is the only allowed one.
 *
 * ★ This file does **not** declare any upstream data type (that is `compat/mmc.h`/`sdhci.h`
 *   forwarding the vendor originals); it only covers the "runtime environment".
 * ==========================================================================
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

/* Config matrix: same role as `generated/autoconf.h` in a real u-boot build.
 * ★ Must come before any upstream header using `CONFIG_IS_ENABLED()` (see compat/config.h). */
#include <linux/kconfig.h>

/* ==================================================================== config matrix self-check
 *
 * The matrix's traps are documented one by one in `compat/generated/autoconf.h`. Here the two
 * **premises this adapter relies on** become compile-time assertions -- so that nobody later
 * "defines DM_MMC as 0" or "turns on BLK" unnoticed:
 * `#ifdef` tests definedness, so these two **must** be `!defined`, not `== 0`. */
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

/* ==================================================================== basic types
 *
 * ★ `u64`/`s64` are `unsigned/signed long long` -- **not** `uint64_t`. u-boot's
 *   `asm-generic/int-ll64.h` says `typedef unsigned long long __u64; typedef __u64 u64;`,
 *   while on LP64 `uint64_t` is `unsigned long`: same width, **different type**. Upstream
 *   formatting/assignments everywhere assume `long long`; changing the type breaks `-Wformat`
 *   and friends (the same trap was hit in the erofs adapter). */
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

/* u-boot's `phys_addr_t`/`dma_addr_t`: both 64-bit on aarch64. This adapter does **not** enable
 * `CONFIG_DMA_ADDR_T_64BIT` (u-boot's switch for 32-bit DMA controllers); it simply defines both
 * as `unsigned long` -- under an identity map they are the virtual/physical addresses themselves. */
typedef unsigned long phys_addr_t;
typedef unsigned long dma_addr_t;

/*
 * Byte order (`asm/byteorder.h` → `linux/byteorder/little_endian.h` on aarch64).
 *   - the `__leN` family is identity on a **little-endian target** (u-boot does the same);
 *   - the `__beN` family is `__swabN` (target is little-endian ⇒ must really swap);
 *   - the bare names `be32_to_cpu`/`le32_to_cpu` etc. are lowercase aliases (the
 *     `#define be32_to_cpu __be32_to_cpu` block at the end of u-boot's `little_endian.h`).
 * Upstream uses `__be32_to_cpu` (SCR) and `be32_to_cpu` (SSR); both families are given here so
 * the next use of `be16`/`le32` does not require touching the environment layer.
 *
 * ★ Like `erofs_env.h`, this file guards the "little-endian premise" with an `#error` at the top
 * (this port is wrong on a big-endian target).
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

/* u-boot `linux/types.h`'s `__uN`/`__sN` and `__leN`/`__beN` families (matching upstream).
 * Upstream uses `__be32` (the SCR buffer in `sd_get_capabilities`). The sparse annotations
 * (`__bitwise`) are dropped -- same treatment as `erofs_env.h`: on aarch64 those are static
 * checks only and do not affect semantics. */
typedef br_u8  __u8;   typedef br_u16 __u16;  typedef br_u32 __u32;  typedef br_u64 __u64;
typedef br_s8  __s8;   typedef br_s16 __s16;  typedef br_s32 __s32;  typedef br_s64 __s64;
typedef br_u16 __le16; typedef br_u32 __le32; typedef br_u64 __le64;
typedef br_u16 __be16; typedef br_u32 __be32; typedef br_u64 __be64;

/* `loff_t`/`dev_t`/`umode_t`: present in u-boot's `linux/types.h`, used indirectly by `mmc.h`. */
typedef long long      loff_t;
typedef unsigned int   dev_t;
typedef unsigned short umode_t;

/* compile-time self-check: wrong type width breaks everything */
_Static_assert(sizeof(u8)  == 1u, "u8 must be 8 bits");
_Static_assert(sizeof(u16) == 2u, "u16 must be 16 bits");
_Static_assert(sizeof(u32) == 4u, "u32 must be 32 bits");
_Static_assert(sizeof(u64) == 8u, "u64 must be 64 bits");

/* ==================================================================== little-endian premise
 * Above, `__leN` is identity and `__beN` is __swabN -- true only if the **target really is
 * little-endian**. aarch64's QEMU virt and nearly all similar boards are little-endian; on a
 * big-endian target this port is wrong, so stop here rather than silently emit wrong words
 * (same guard as `erofs_env.h`). */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-mmc implements leN_to_cpu()/beN_to_cpu() for little-endian targets only"
#endif

/* ==================================================================== attributes/annotations
 * u-boot's `include/linux/compiler*.h` family provides these. This adapter does not vendor that
 * family (it needs `__KERNEL__` plus build-time probes like `COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW`)
 * and gives only the few upstream actually uses -- each with a clear use upstream. */
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

/* ==================================================================== bit ops/utilities */
#define BITS_PER_LONG        64
#define BITS_PER_LONG_LONG   64
#define BITS_PER_BYTE        8

#define BIT(nr)              (1UL << (nr))
#define BIT_ULL(nr)          (1ULL << (nr))
#define BIT_MASK(nr)         (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)         ((nr) / BITS_PER_LONG)
#define GENMASK(h, l)        (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l)    (((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))

/* fls/ffs from u-boot `<linux/bitops.h>` (generic implementation, same semantics) */
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

/* Same shape as upstream `include/linux/kernel.h` (lines 156-231): **each argument is evaluated
 * once** and types are checked. ★ A plain ternary will not do: upstream has several `max(a, b())`
 * style / side-effecting arguments, and a plain ternary would evaluate one of them twice. */
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

/* ★ `lower_32_bits`/`upper_32_bits` are copied verbatim from upstream `include/linux/kernel.h`
 *   (lines 108/114). **Not decoration**: the ADMA2 descriptor `addr_lo` and `SDHCI_ADMA_ADDRESS`
 *   both rely on `lower_32_bits()`.
 *   (Measured lesson: this layer's first version mis-copied `lower_32_bits` as the
 *    `upper_32_bits` expansion, so every descriptor's data address became `addr >> 32 == 0`
 *    ⇒ QEMU's ADMA engine fetched address 0 ⇒ `SDHCI_INT_ADMA_ERROR` (`st=0x02008001`,
 *    `adma_err=0x1`) ⇒ `mmc_init` returned `-ECOMM`.
 *    That was an **adapter-layer bug**, not upstream's, and not "upstream cannot run on QEMU's
 *    emmc model".) */
#define lower_32_bits(n)  ((u32)((n) & 0xffffffff))
#define upper_32_bits(n)  ((u32)(((n) >> 16) >> 16))
#define lower_16_bits(n)  ((u16)(n))

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Subset of u-boot `<linux/sizes.h>` (upstream mmc.c's SD SSR table uses these).
 * These are **platform/interface constants**, not tunables; values match upstream one by one. */
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

/*
 * 64-bit division helpers (u-boot `include/div64.h` + the BITS_PER_LONG==64 branch of
 * `include/linux/math64.h`). This adapter does not vendor those two headers (their other half is
 * 32-bit-platform log2/bit-alignment tricks); it gives only the three upstream uses -- semantics
 * must match the kernel set:
 *   - `do_div(n, base)`: **in place**, sets `n` to the quotient and returns the remainder
 *     (call sites rely on that side effect);
 *   - `div_u64_rem`: non-negative 64/32 division with a remainder out-parameter;
 *   - `lldiv`: a "does not modify its argument" wrapper over `do_div` (upstream `mmc.c` uses it
 *     to compute the LBA).
 */
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

/* ==================================================================== logging/printing
 *
 * Upstream uses `printf`/`puts`/`sprintf` plus `log_err/log_warning/log_info/log_debug/debug`
 * and `pr_err/pr_info/pr_debug`. This adapter has **no libc printf** (core's `br_log_write` is
 * line output only, and `runtime/posix` provides no printf family either -- ADR-0014 §4).
 *
 * ⇒ This file carries a **tiny formatter** covering the conversions upstream **actually uses**
 * (every call site checked):
 *   `%d %i %u %x %X %c %s %p %%`, flags `#` and `0`, width (e.g. `%06x`/`%08x`/`%03d`),
 *   length modifiers `l`/`ll`/`z`.
 *   ★ Why **no** `format(printf)` attribute: upstream passes `uint` to `%d` (u-boot's own
 *     printf does not validate strictly either); the attribute would immediately turn vendor code
 *     red under `-Werror` -- and "change upstream to pass warnings" is forbidden. The attribute
 *     is added only on the layer forwarding to core (those call sites are ours).
 *
 * ★ Deliberate deviation: upstream's `printf` goes to the u-boot console; this adapter goes to
 *   core's log (BR_LOG_INFO), so "what the driver says" and the rest of the kernel log share one
 *   stream (the gate greps those lines).
 */
#define UBOOT_MMC_LOG_MAX 256u

/* Unsigned integer → buffer (little-endian digit table emitted in reverse). Returns new write ptr. */
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

/*
 * This plugin's formatter. `cap` includes the trailing NUL; overflow truncates silently
 * (logs/device strings are diagnostics, not a gate surface). Coverage is in the file header;
 * unimplemented modifiers are **emitted verbatim** (an ugly log beats silently dropping it).
 */
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

/*
 * ★ Why not just use `__builtin_vsnprintf`: GCC's `__builtin_vsnprintf` is **not self-contained**
 *   -- after expansion it still calls libc `vsnprintf` (link-time `undefined reference`). This
 *   repo has no libc (`runtime/posix` has no printf family either) ⇒ we must carry our own
 *   (same treatment as `erofs_env.h`).
 */
static inline int printf(const char *fmt, ...)
{
    char line[UBOOT_MMC_LOG_MAX];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    (void)uboot_mmc_vformat(line, sizeof(line), fmt, ap);
    __builtin_va_end(ap);
    /* upstream format strings carry their own '\n'; br_log_write also terminates by line ⇒ one
     * extra newline is harmless (and the gate greps by line). */
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
 * Upstream writes `log_err(fmt, ...)` and **we supply the implementation** -- exactly what
 * "adaptation only" looks like. Note upstream format strings carry their own '\n'; this layer
 * adds none (consistent with core's per-line output).
 *
 * ★★ There is a **required** distinction here (not a style question: without it the link fails):
 *    u-boot builds these two families as **different** filtering layers --
 *
 *      ➀ `log_err/log_warning/...` (upstream `include/log.h`) do **no compile-time filtering**:
 *         in u-boot they expand to `log(cat, level, ...)` and are filtered only **at runtime** by
 *         log level. ⇒ this layer does the same: forward straight to
 *         `br_log_write(BR_LOG_*, ...)` (core's level filter is that runtime gate).
 *      ➁ `pr_err/pr_info/pr_debug/...` (upstream `include/linux/printk.h`) do **compile-time
 *         filtering by `CONFIG_LOGLEVEL`**, shaped as a "constant-condition ternary":
 *             #define pr_debug(fmt, ...) ({ CONFIG_LOGLEVEL > 7 ? log_debug(fmt, ##__VA_ARGS__) : 0; })
 *
 *    Why ➁'s filtering is **required**: upstream `mmc.c:175` uses the **same condition** to also
 *    cut out the definition of `mmc_mode_name()` --
 *         #if CONFIG_IS_ENABLED(MMC_VERBOSE) || defined(DEBUG) || CONFIG_VAL(LOGLEVEL) >= LOGL_DEBUG
 *         const char *mmc_mode_name(enum bus_mode mode) { ... }
 *         #endif
 *    and its **call sites are all inside `pr_debug(...)` arguments** (mmc.c lines
 *    232/1748/1884/2228). If `pr_debug` did not filter by `CONFIG_LOGLEVEL` (e.g. forwarded
 *    unconditionally like `log_*`), those arguments would be evaluated ⇒ **definition cut,
 *    reference kept** ⇒ link-time `undefined reference to mmc_mode_name`. That is exactly what
 *    happened (hit by the first version of this layer; see the delivery report). ⇒ ➁ must use
 *    **the same thresholds** as upstream, one by one.
 *
 *    Thresholds copied from upstream `include/linux/printk.h` (not invented): `>0/>1/>2/>3/>4/>5/>6/>7`.
 *    This adapter's `CONFIG_LOGLEVEL = 4` ⇒ `pr_err` kept, `pr_warn` and below cut -- matching
 *    u-boot's default; and `log_warning()` (e.g. sdhci.c's reset warning) is **unaffected** (family ➀).
 */
#define UBOOT_MMC_LOG_GATED(cond, level, ...) \
    ({ (cond) ? br_log_write((level), __VA_ARGS__) : 0; })

/* ➀ runtime filtering (no compile-time cut) */
#define UBOOT_MMC_LOGF(level, ...) br_log_write((level), __VA_ARGS__)
#define log_emerg(...)    UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_alert(...)    UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_crit(...)     UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_err(...)      UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_warning(...)  UBOOT_MMC_LOGF(BR_LOG_WARN,  __VA_ARGS__)
#define log_notice(...)   UBOOT_MMC_LOGF(BR_LOG_INFO,  __VA_ARGS__)
#define log_info(...)     UBOOT_MMC_LOGF(BR_LOG_INFO,  __VA_ARGS__)
#define log_debug(...)    UBOOT_MMC_LOGF(BR_LOG_DEBUG, __VA_ARGS__)

/* ➁ compile-time filtering (thresholds as upstream printk.h; see "required distinction" above) */
#define pr_emerg(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 0, BR_LOG_ERROR, __VA_ARGS__)
#define pr_alert(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 1, BR_LOG_ERROR, __VA_ARGS__)
#define pr_crit(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 2, BR_LOG_ERROR, __VA_ARGS__)
#define pr_err(...)       UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 3, BR_LOG_ERROR, __VA_ARGS__)
#define pr_warn(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 4, BR_LOG_WARN,  __VA_ARGS__)
#define pr_notice(...)    UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 5, BR_LOG_INFO,  __VA_ARGS__)
#define pr_info(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 6, BR_LOG_INFO,  __VA_ARGS__)
#define pr_debug(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 7, BR_LOG_DEBUG, __VA_ARGS__)
#define pr_devel(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 7, BR_LOG_DEBUG, __VA_ARGS__)

/* `debug()`: upstream is `debug_cond(_DEBUG, ...)` with `_DEBUG = (DEBUG ? 1 : 0)`; this adapter
 * does not define DEBUG ⇒ always 0. Upstream writes `if (cond) printf(...)` and relies on -O2 DCE
 * to drop the arguments; a constant ternary here is more robust (constant folding, any -O level). */
#define _DEBUG            0
#define debug(...)        UBOOT_MMC_LOG_GATED(_DEBUG != 0, BR_LOG_DEBUG, __VA_ARGS__)

/*
 * The u-boot log level constants (a **same-value restatement** of `enum log_level_t` in
 * u-boot `include/log.h`).
 *
 * ★ Why these must be **macros**, not an enum: upstream `mmc.c:175` uses `LOGL_DEBUG` inside
 *   an **`#if`**
 *
 *       #if CONFIG_IS_ENABLED(MMC_VERBOSE) || defined(DEBUG) || CONFIG_VAL(LOGLEVEL) >= LOGL_DEBUG
 *
 *   The preprocessor only knows macros ⇒ an enum name there is "undefined" (true of u-boot's own
 *   line too). Under `-Wundef -Werror` this adapter must supply **macros** to both compile and be
 *   semantically right: with `CONFIG_LOGLEVEL` = 4 (= WARNING) that condition is false and the
 *   "per-command trace printing" is not compiled (consistent with "do not spam").
 */
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

/* ==================================================================== memory
 *
 * Project discipline: `memcpy/memset/memcmp/memmove` come from core and are used directly (do not
 * write our own, do not depend on runtime/posix). core's `string_internal.h` is private, so the
 * prototypes are repeated here (prototypes bring type checking -- the `-Wmissing-prototypes` half
 * of the discipline). */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* u-boot's `malloc.h`: wired to the core heap. ★ Macros rather than "declare functions for
 * runtime/posix to provide": the latter would make this plugin fail to link in products that do
 * **not** select runtime/posix, while the heap is a core capability. */
#define malloc(n)        br_malloc((br_size_t)(n))
#define calloc(n, s)     br_calloc((br_size_t)(n), (br_size_t)(s))
#define realloc(p, n)    br_realloc((p), (br_size_t)(n))
#define free(p)          br_free(p)

/* u-boot's `memalign.h`: upstream `sdhci_adma_init()` uses it to get a DMA-safe, aligned
 * descriptor table (`memalign(ARCH_DMA_MINALIGN, ADMA_TABLE_SZ)`).
 * ★ Why the environment layer cannot just wire it to br_malloc: the ADMA2 descriptor table **is
 *   read by the device**, so it must come from the DMA pool (uncached/physically contiguous, see
 *   ADR-0003's region table). ⇒ glue provides it; this is only the declaration. */
void *uboot_mmc_env_memalign(br_size_t align, br_size_t size);
#define memalign(align, size) uboot_mmc_env_memalign((align), (size))

/* ==================================================================== errno
 * Upstream is full of bare `-EIO`/`-ETIMEDOUT`/`-ENODEV`/`-ENOSYS`/`-EINVAL`/`-EBUSY`/`-ECOMM`/
 * `-ENOMEM`/`-EOPNOTSUPP`/`-EMEDIUMTYPE`. ADR-0027 renamed errno names to the POSIX originals and
 * co-located them with their values in `br_error.h` ⇒ those sources need **not one character
 * changed** (that is the point of the rename). */

/*
 * ---- errno names upstream uses that core's `br_error.h` does **not yet** carry ----------------
 *
 * ★ This is a **known debt**, and the only place this adapter "adds errno names":
 *   `core/include/br/core/br_error.h` per ADR-0027 only carries the second-range names (from 35)
 *   that "this repo uses", and this adapter is a **new consumer**: the upstream `drivers/mmc/`
 *   originals use the five below (`-ECOMM` for command timeout/error return codes, `-EMEDIUMTYPE`
 *   when a hardware partition is unsupported, `-ENOTSUPP` as the fallback for "modes this adapter
 *   does not implement", the other two on the SD path). They are absent from core ⇒ "upstream
 *   unchanged" means they must be added here.
 *
 * ★ The values are taken **verbatim from u-boot `include/linux/errno.h`** (which took them from
 *   the Linux kernel), so moving these five into `br_error.h` later needs **no value changes**;
 *   criterion ➁ of `tools/check-errno.sh` ("no errno names defined outside br_error.h") currently
 *   only enforces the 53 names already in br_error.h, so this file does **not** trip it -- but the
 *   truth belongs back in core (see the debt list in the delivery report §6).
 *   This file does **not** modify core (hard constraint: do not touch the core directory).
 */
#define ECOMM        70    /* Communication error on send */
#define EBADMSG      74    /* Not a data message */
#define ENOMEDIUM   123    /* No medium found */
#define EMEDIUMTYPE 124    /* Wrong medium type */
#define ENOTSUPP    524    /* Operation is not supported (kernel-internal code, not uapi) */

/* ==================================================================== time
 *
 * ★ This adapter's core trick (see file header): `udelay()`/`get_timer()` are a "**sleep wait
 *   that a device IRQ can wake early**", so upstream's polling loops become IRQ-driven waits.
 *
 * Two hooks, **one per kind of u-boot wait** (provided by glue):
 *   - `uboot_mmc_env_wait(max_us)`: "yield the CPU **for at most max_us, or until the current
 *     device's device IRQ**" -- the **wait-for-device-event** path (command completion); before
 *     sleeping it re-enables this card's `SIGNAL_ENABLE` (the ISR deasserts the level line by
 *     writing SIGNAL_ENABLE 0), so normally a **device IRQ** wakes us; with no waitable device
 *     channel (or no scheduler yet) it falls back to a short yield.
 *   - `uboot_mmc_env_spin(us)`: **busy wait** (does not yield). Upstream uses `udelay()` to spin
 *     waiting for "register stable/reset complete" (measured values are only 10/100/1000/2000 µs,
 *     see §"why not sleep"); those loops have no device event to wait for, and the scheduler's
 *     sleep granularity is **one tick** (1/HZ = 5 ms) ⇒ routing them to the sleep path would
 *     "sleep 100 µs but really 5 ms" (measured: four-card bring-up 0.037 s → 0.35 s, pushing the
 *     whole self-test past `irq-test`'s 6 s gate limit; see ADR-0029 §4).
 * `uboot_mmc_env_reg_touched()` is provided by glue: it records "the MMIO address just accessed"
 *   for the "current device" lookup above (upstream's wait loops have no device argument).
 */
void uboot_mmc_env_wait(br_u32 max_us);
void uboot_mmc_env_spin(br_u32 us);
void uboot_mmc_env_reg_touched(const volatile void *reg);

/*
 * **Upper bound** on a single yield window (= the "next ms" of task spec §2.4): only the
 * "wait-for-device-event" path uses it fully.
 */
#define UBOOT_MMC_WAIT_MAX_US   1000u

/* One "yield the CPU" window: at most max_us microseconds, or a device IRQ. */
static inline void uboot_mmc_yield_once(br_u32 max_us)
{
    uboot_mmc_env_wait(max_us);
}

/*
 * `udelay(us)` -- u-boot's semantics are exactly a **busy wait**; upstream only uses it to wait
 * for "register stable/reset complete" (values 10/100/1000/2000 µs), so busy-wait like u-boot
 * here: no CPU yield, so a µs-level spin is not inflated to "one tick" (5 ms). No upper bound is
 * promised (upstream only guarantees "not early").
 */
static inline void udelay(unsigned long usec)
{
    uboot_mmc_env_spin((br_u32)usec);
}

/*
 * `mdelay(ms)`: u-boot uses it to wait for the **card** to become ready (the polling interval of
 * `mmc_send_op_cond`/`mmc_complete_op_cond`, values 5/10 ms). That is a "wait-for-device-event"
 * ⇒ it takes the opposite path from `udelay`: **yield the CPU**, at most 1 ms per slice, and a
 * device IRQ can wake it early.
 */
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

/*
 * `get_timer(base)`: returns "milliseconds since base". ★ Every call **yields the CPU first**
 * (until the next ms or a device IRQ) -- upstream uses it as the polling metronome, so its loops
 * become IRQ-driven.
 */
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

/* ==================================================================== MMIO
 *
 * Upstream `sdhci.h`'s `sdhci_readl/writel/...` (with `MMC_SDHCI_IO_ACCESSORS` off) land directly
 * on `readl(host->ioaddr + reg)`. That environment is what this section provides.
 *
 * ★ Every access calls `uboot_mmc_env_reg_touched(addr)`: that is the entire secret of "how the
 *   environment layer knows which card is being waited on" (see file header). It is just a store
 *   plus a lookup (when waiting) and does not affect correctness.
 * ★ Alignment: the PCIe BAR sits in Device-nGnRnE memory and this target uses `-mstrict-align`
 *   ⇒ accesses must be aligned. Upstream `sdhci_readl/writel` use aligned offsets, so dereference
 *   by width here.
 */
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

/* ==================================================================== cache / DMA
 *
 * u-boot's `cpu_func.h`/`asm/cache.h`/`linux/dma-mapping.h`:
 *   - `ARCH_DMA_MINALIGN`: matches the platform -- Cortex-A53 L1 D-cache line is 64 B (QEMU virt);
 *   - `flush_cache(addr, size)` (upstream `sdhci-adma.c` calls it after filling the descriptor
 *     table) / `flush_dcache_range` / `invalidate_dcache_range` → core's `br_mm_cache_*` (hitting
 *     an uncached region ⇒ no-op);
 *   - `dma_map_single`/`dma_unmap_single`: u-boot's semantics are exactly "per-direction cache
 *     maintenance + return the identity-mapped dma_addr" (see upstream `linux/dma-mapping.h`);
 *   - `dev_phys_to_bus(dev, addr)`: here `!DM_MMC` ⇒ `mmc_to_dev()` is NULL; identity map
 *     ⇒ return as-is (`phys2bus.h`'s `#define dev_phys_to_bus(_, _addr) _addr`).
 */
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

/* `linux/dma-direction.h` (matching upstream one by one) */
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

/* `phys2bus.h` (both the `!CONFIG_PHYS_TO_BUS` and `!DM` branches degenerate to identity) */
#define dev_phys_to_bus(_dev, addr) ((dma_addr_t)(addr))
#define dev_bus_to_phys(_dev, addr) ((phys_addr_t)(addr))
static inline unsigned long phys_to_bus(unsigned long phys) { return phys; }
static inline unsigned long bus_to_phys(unsigned long bus)  { return bus; }

/* u-boot's `virt_to_phys()`: identity here (identity map) (`sdhci_setup_cfg` uses it for the ADMA table address). */
#define virt_to_phys(addr) ((phys_addr_t)(br_uintptr_t)(addr))

/* ==================================================================== GPIO
 *
 * `struct sdhci_host` embeds two `struct gpio_desc` **by value** (pwr_gpio/cd_gpio), so the type
 * must be present. This adapter enables neither DM_GPIO nor DM_MMC ⇒ nothing initializes or uses
 * them. The definition matches upstream `asm-generic/gpio.h` field by field (three fields).
 */
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

/* ==================================================================== other forward declarations
 * Upstream `mmc.h` declares `struct bd_info;` itself; the `mmc_initialize()` signature needs it. */
struct bd_info;

#endif /* BR_ADAPTER_UBOOT_MMC_ENV_H */
