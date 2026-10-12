/*
 * adapter/uboot-fat — **u-boot runtime environment** (adapter layer)
 *
 * ============================== what this file is ==============================
 * Upstream FAT (`fs/fat/fat.c` + `fs/fat/fat_write.c`, one translation unit) is **u-boot
 * style** C: it assumes `<blk.h>`/`<part.h>` (block device: `struct blk_desc` +
 * `disk_partition` + `blk_dread/blk_dwrite`), `<fat.h>` (on-disk structs), `<fs.h>`
 * (dirents/dir streams), `<malloc.h>`'s `malloc_cache_aligned()`, `<rand.h>`'s `rand()`,
 * libc's `malloc/free/strdup/strchr/strrchr/strcasecmp/sprintf`, `printf`/`debug`/
 * `log_err`, `assert()`. This plugin is a **bare-metal unikernel (no libc)**, so this layer
 * maps each assumption **back to this project's truth** — the upstream .c needs **not one
 * character changed** (the criterion is the hash lock in
 * `tools/check-third-party-pristine.sh`; adapting = giving upstream an environment, not
 * editing upstream).
 *
 *   u-boot/Linux side                          →  brickOS side
 *   ---------------------------------------------------------------------------------
 *   u8/u16/u32/u64/s64/loff_t/ulong/uint       →  br_u8/… / br_s64(long long)
 *   bool/true/false                            →  br_bool / BR_TRUE / BR_FALSE
 *   __u8…__le32 + le16_to_cpu/cpu_to_le32      →  identity (aarch64 = LE; see #error below)
 *   get_unaligned_le16/32                      →  __builtin_memcpy version (target -mstrict-align)
 *   malloc/calloc/realloc/free                 →  br_malloc/br_calloc/br_realloc/br_free
 *   memcpy/memset/memcmp/memmove               →  from core (project rule: use these four directly)
 *   strdup/strcpy/strlen/strchr/strrchr/…      →  this file's `uboot_fat_*` (see below)
 *   sprintf                                    →  bounded formatter declared here (no libc)
 *   rand()                                     →  this file's deterministic LCG (see below)
 *   printf/debug/log_err/log_debug             →  br_log_write (with "uboot-fat: " prefix)
 *   assert(x)                                  →  **empty statement** (as upstream `_DEBUG=0`)
 *   malloc_cache_aligned/memalign/ALLOC_CACHE_ALIGN_BUFFER → br_malloc (align ≥ 16 B)
 *   struct blk_desc + disk_partition           →  **minimal shim** (whole device = one partition:
 *                                                 start sector 0, sectors = geometry.n_sectors)
 *   blk_dread/blk_dwrite                       →  `disk_read/disk_write` → br_bdev_ops_t.read/write
 *   part_get_info/blk_enabled/dev_print/…      →  environment stubs (**only** called by upstream's
 *                                                 unreferenced `fat_register_device`/
 *                                                 `file_fat_detectfs`, --gc-sections drops them)
 *
 * ## why the strings are hand-written
 *   Hard constraint (task spec): `strlen/strcmp/strncmp/strcpy/strchr/strrchr/tolower` **must be
 *   written small here**, no dependency on runtime/posix. All are `static inline` +
 *   `uboot_fat_` prefix, then **macros** map upstream's bare names onto them — so no upstream
 *   call site changes, and the link never reaches for a missing libc symbol (the whole
 *   translation unit is `-fno-builtin`, so the compiler cannot treat our definitions as
 *   builtins and self-call).
 *
 * ## boundedness (environment facts, not "some bound we picked")
 *   · `strcpy(dst, src)`: upstream has only two call sites, both targeting **256-byte** buffers
 *     (`fat_readdir`'s `struct fs_dirent.name[256]` (via vendored `include/fs.h`) and
 *     `normalize_longname`'s `char l_filename[VFAT_MAXLEN_BYTES=256]`) ⇒ 256 is
 *     **upstream's own construction**, not our guess.
 *   · `strdup(path)`: this plugin's VFS surface caps paths at `UB_FAT_PATH_MAX` (256)
 *     (see `glue/fat_glue.h`); longer returns NULL — upstream's call site answers NULL
 *     with `-ENOMEM`.
 *   · `sprintf(buf, fmt, …)`: libc-free `sprintf` has no "how big is the target" argument, so
 *     this environment caps at `UB_FAT_SPRINTF_MAX` (256) and appends '\0'; upstream's four
 *     call sites target `char vol_label[12]`/`char *uuid_str` (contract ≥10 B)/`char buf[13]`,
 *     all far below 256 ⇒ the normal path never truncates (emitted in reverse, overflow
 *     dropped, see `glue/fat_blk.c`).
 *
 * ## intentional deviations (from upstream **behaviour**, each auditable)
 *   1. `assert(x)` is an empty statement — **matching upstream `include/log.h`**: upstream has
 *      `#define assert(x) ({ if (!(x) && _DEBUG) __assert_fail(...); })`, and neither fat .c
 *      defines `DEBUG` nor `CONFIG_LOG`/`LOG_DEBUG` ⇒ `_DEBUG == 0` ⇒ this upstream build
 *      **does no runtime assertions anyway**. This file uses `if (0) (void)(x)` to keep both
 *      properties: the expression is still type-checked, never evaluated at run time.
 *   2. `debug(...)` is **silent** by default (same as upstream `debug` = `debug_cond(_DEBUG, …)`).
 *      Defining `UBOOT_FAT_VERBOSE_DBG` routes it to BR_LOG_DEBUG; upstream's data-path
 *      diagnostics are `printf`/`log_err`, which **always** emit (they map to BR_LOG_ERROR).
 *   3. `printf` and `log_err` go to `br_log_write(BR_LOG_ERROR, ...)`: upstream's `printf` is
 *      "print and continue"; here it becomes **one error log** (text kept verbatim, and the
 *      format string plus arguments are still `-Wformat` checked). Continue-vs-return is still
 *      decided by upstream's own `return -1` — we did **not** change upstream's return contract.
 *   4. `rand()` is a **deterministic** 32-bit LCG (u-boot's `rand()` has no upstream definition,
 *      see NOTICE): only `set_name()` uses it to pick the search start for a short-name suffix,
 *      and the candidate space is deterministic anyway (try until no collision) ⇒ contract
 *      unchanged, minus one source of nondeterminism.
 *   5. The DM/RTC/partition family are **environment stubs**: `CONFIG_DM_RTC=0` /
 *      `CONFIG_EFI_LOADER=0`, so the upstream branches are dead code (still must type-check), and
 *      `part_get_info` et al. are called only by `fat_register_device` (this port mounts via
 *      `fat_set_blk_dev` + our own volume table) ⇒ `-ffunction-sections` + `-Wl,--gc-sections`
 *      keep them out of the image.
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_FAT_ENV_H
#define BR_ADAPTER_UBOOT_FAT_ENV_H

/* `NULL`/`size_t` come from the **compiler's own** <stddef.h> (not libc); uintN_t from <stdint.h> */
#include <stddef.h>
#include <stdint.h>

#include <br/core/br_error.h>
#include <br/core/br_errno.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
/* Block device contract: upstream takes it from `<blk.h>`/`<part.h>`, this file from here.
 * **Only this header** (adapter entry) includes `<br/...>`; upstream .c and glue go via it. */
#include <br/dev/br_bdev.h>

/* ==================================================================== little-endian premise */
/* Upstream's `FAT2CPU16/32` (the `#if __LITTLE_ENDIAN` in vendored `include/fat.h`) and
 * `le16_to_cpu()` are **identity** here ⇒ valid only on a true little-endian target. aarch64
 * QEMU virt and nearly all such boards are LE; a BE target makes this wrong, so stop here. */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-fat FAT2CPU16/32 and leN_to_cpu() are identity: this port supports little-endian only"
#endif
/* vendored `include/fat.h`'s `FAT2CPU16/32` takes the `#if __LITTLE_ENDIAN` branch — give it
 * a true value (u-boot's `include/linux/byteorder/little_endian.h` does the same). */
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif

/* ==================================================================== base types */
/* ★ Follow **u-boot's widths** verbatim, not "equally wide br_*":
 *   u-boot's `u64`/`s64`/`loff_t` are `unsigned long long`/`long long`, while this repo's
 *   `br_u64`/`br_s64` are `unsigned long`/`long` (LP64: same width, **different type**). Not
 *   pedantry: upstream is full of `%llu`/`%lld` bound to the argument **type**, and br_* gives
 *   `-Werror=format=` (hit by the probe; same trap as erofs, ADR-0026 §3.1). */
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

/* ---- on-disk integer family (the one in u-boot `include/linux/types.h`) ---- */
typedef unsigned char      __u8;
typedef unsigned short     __u16;
typedef unsigned int       __u32;
typedef unsigned long long __u64;
typedef __u16 __le16;
typedef __u32 __le32;
typedef __u64 __le64;

_Static_assert(sizeof(__u64) == 8u, "__u64 must be 64-bit");

/* ==================================================================== byte order (identity) */
static inline u16 uboot_fat_le16_to_cpu(u16 v) { return v; }
static inline u32 uboot_fat_le32_to_cpu(u32 v) { return v; }
static inline u16 uboot_fat_cpu_to_le16(u16 v) { return v; }
static inline u32 uboot_fat_cpu_to_le32(u32 v) { return v; }
#define le16_to_cpu(x) uboot_fat_le16_to_cpu(x)
#define le32_to_cpu(x) uboot_fat_le32_to_cpu(x)
#define cpu_to_le16(x) uboot_fat_cpu_to_le16(x)
#define cpu_to_le32(x) uboot_fat_cpu_to_le32(x)

/* Unaligned little-endian loads (upstream `get_unaligned_le16/32`): the target uses
 * `-mstrict-align`, so always via __builtin_memcpy (GCC lowers it to one `ldr`/`str` on aarch64). */
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

/* ==================================================================== memory */
/* Allocator: names forwarded (br_* is core's truth; no libc). Why macros instead of declaring
 * `void *malloc(size_t)`: that would become a **link-time dependency on the runtime**
 * (`runtime/posix` currently provides malloc, but this plugin is a media FS and must not depend
 * on the POSIX surface). Macros have no such ambiguity.
 * `malloc_cache_aligned`/`memalign`: core's heap aligns ≥ 16 B (BR_MALLOC_ALIGN), covering all
 * of this plugin's alignment needs (max natural alignment 8 B; upstream's "FAT buffer must be
 * 32-bit aligned" holds likewise) ⇒ no alignment object needed, just br_malloc. */
#define malloc(n)             br_malloc(n)
#define calloc(n, s)          br_calloc((n), (s))
#define realloc(p, n)         br_realloc((p), (n))
#define free(p)               br_free(p)
#define memalign(a, n)        ((void)(a), br_malloc(n))
#define malloc_cache_aligned(n) br_malloc(n)

/* `memcpy/memset/memcmp/memmove` are **provided by core** (core/src/string.c, linked in with
 * core; project rule: use these four directly, do not rewrite and do not depend on
 * runtime/posix). core's string_internal.h is core-private, so prototypes go here — prototypes
 * are what enables type checking (the flip side of `-Wmissing-prototypes`). */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* u-boot's DMA alignment macros (`include/asm/cache.h` + `include/memalign.h`). Here there is
 * no DMA constraint (the block layer memcpys into a device buffer; the driver owns DMA) ⇒ use
 * 16 (same as BR_MALLOC_ALIGN), and `ALLOC_CACHE_ALIGN_BUFFER` yields a **truly 16 B aligned**
 * on-stack pointer, same name and meaning as upstream. */
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

/* The attribute macros from u-boot `include/linux/compiler.h` / `include/linux/compiler_types.h`
 * that this file **actually uses** (`fat.c`'s `static void __maybe_unused fat2rtc(...)`). No
 * other compile-time attribute macros — the environment supplies only the names upstream needs,
 * not a whole approximate Linux header (see `compat/linux/compiler.h`). */
#define __maybe_unused  __attribute__((__unused__))
#define __packed        __attribute__((__packed__))
#define __aligned(x)    __attribute__((__aligned__(x)))
#define __always_inline inline __attribute__((__always_inline__))
#ifndef __force
#define __force
#endif

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

/* `fls(i)` (upstream `set_name` uses it for the "random short-name suffix" bit width): position
 * of the highest set bit, 1-based, fls(0)=0. */
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

/* `is_power_of_2()` (used by upstream `is_bootsector_valid`; the meaning in u-boot
 * `include/linux/log2.h`) */
static inline bool is_power_of_2(unsigned long n)
{
    return (bool)((n != 0ul) && ((n & (n - 1ul)) == 0ul));
}

/* `ilog2()` (u-boot `include/linux/log2.h`; unused by this file's fat source, but kept at the
 * same name and meaning as upstream, so a later upstream use needs no "approximate" filler). */
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

/* `do_div`/`lldiv`/`div_u64_rem` (upstream `include/div64.h` + `include/linux/math64.h`).
 * Target BITS_PER_LONG == 64 ⇒ verbatim the same as u-boot's 64-bit branch. */
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

/* `BUILD_BUG_ON` — implemented with u-boot's mechanism (an `if` in a function body +
 * `__compiletime_error`), not `_Static_assert`: upstream `include/linux/build_bug.h` allows a
 * condition that is not an integer constant expression. This file's fat source has no call
 * site, but vendored headers may reach it indirectly (a complete environment is safer than a
 * minimal one). */
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

/* `assert()`: see "intentional deviations (1)" in the file header — `_DEBUG == 0` for this
 * upstream build. The expression is still type-checked (`(void)(x)` inside `if (0)`), never
 * evaluated at run time. */
#define assert(x) do { if (0) { (void)(x); } } while (0)

/* ==================================================================== compile-time config */
/* u-boot uses Kconfig `CONFIG_*` switches to cut code. Here they get **explicit true values**,
 * so upstream's `CONFIG_IS_ENABLED(...)` branches hold verbatim (we do not delete branches):
 *   FAT_WRITE=1  ⇒ vendored fat.c does not compile the read-only flush_dirty_fat_buffer stub
 *                  (the real one is in upstream fat_write.c — same translation unit, one copy)
 *   EFI_LOADER=0 ⇒ fat_readdir fills no timestamps (this port has no RTC in v1)
 *   DM_RTC=0     ⇒ the write path takes no RTC time, using upstream's own 2000-01-01 fallback
 *   FS_EXFAT=0   ⇒ dirent name field = 256 bytes (see vendored include/fs.h)
 *   FS_FAT_MAX_CLUSTSIZE: u-boot's default (32 KiB), used by vendored fat.h's MAX_CLUSTSIZE */
#define CONFIG_FAT_WRITE           1
#define CONFIG_EFI_LOADER          0
#define CONFIG_DM_RTC              0
#define CONFIG_FS_EXFAT            0
#define CONFIG_FS_FAT_MAX_CLUSTSIZE 32768
#define CONFIG_LOGLEVEL            8
#define CONFIG_LOG                 0
#define CONFIG_LOGF_FUNC           0

/* `CONFIG_IS_ENABLED(opt, [case1], [case2])` / `IS_ENABLED(opt)`: the same variadic choice as
 * u-boot (1 arg ⇒ 0/1 constant; 3 args ⇒ pick one). Upstream's four uses here are all in scope:
 * `#if !CONFIG_IS_ENABLED(FAT_WRITE)`, `if (CONFIG_IS_ENABLED(EFI_LOADER))`,
 * `if (CONFIG_IS_ENABLED(DM_RTC))`, `CONFIG_IS_ENABLED(FS_EXFAT, (1024), (256))`. */
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

/* u-boot log categories/levels: both upstream .c files define `#define LOG_CATEGORY LOGC_FS`,
 * while `log_err`/`log_debug` map to br_log_write here (no category) ⇒ all this needs is for that
 * macro to have **a defined name** (otherwise `-Wundef` fires when it is expanded). */
#define LOGC_FS 0

/* ==================================================================== logging */
/* See "intentional deviations (2)(3)" in the header: printf/log_err always emit (ERROR level),
 * debug is silent by default.
 * ★ Why concatenate the string instead of "format then forward": upstream's format strings are
 *   **all string literals** (checked call site by call site), so `"uboot-fat: " fmt` is a
 *   compile-time concat, and argument type checking still comes from br_log_write's
 *   `format(printf, …)` attribute. Zero variadic args relies on GNU `##__VA_ARGS__` (the same
 *   trick as adapter/uboot-erofs's pr_err). */
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

/* ==================================================================== strings (hand-rolled) */
/* All `static inline` + `uboot_fat_` prefix (hard constraint: no dependency on runtime/posix),
 * then macros forward upstream's bare names. Length discipline: always bounded by the caller's
 * `n` (or an upper bound that is **upstream's own construction**). */
#define UB_FAT_STRCPY_CAP   256u   /* upstream's two strcpy targets are 256 B buffers (see header) */
/* `strdup`'s bound = this plugin's VFS path bound (`UB_FAT_PATH_MAX` = 256, see glue/fat_glue.h)
 * + a leading '/' and a trailing '\0' from the upstream call site ⇒ 258. Longer returns NULL
 * (upstream answers NULL with -ENOMEM), so "path too long" and "out of memory" are the same
 * failure upstream, never a silent truncation. */
#define UB_FAT_STRDUP_CAP   (256u + 2u)
#define UB_FAT_SPRINTF_MAX  256u   /* environment-side cap for libc-free sprintf (see header) */

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

/* Bounded copy (cap includes the trailing '\0'; see header: upstream's two targets are 256 B) */
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

/* ASCII lowercase (upstream `tolower`; handles 'A'..'Z' only, other bytes returned as-is —
 * upstream likewise assumes an 8-bit codepage) */
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

/* The port is visible here: upstream's bare names → our implementations (the whole translation
 * unit is -fno-builtin, so no self-collision) */
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

/* `strdup`/`sprintf`/`rand`: implemented in `glue/fat_blk.c` (bounded/deterministic; see header).
 * Declarations + macros rather than in-header definitions: they are real cross-module symbols, so
 * prototypes give the other side of -Wmissing-prototypes, and there is exactly one truth here. */
char *uboot_fat_strdup(const char *s);
int   uboot_fat_sprintf(char *dst, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
int   uboot_fat_rand(void);

#define strdup(s)   uboot_fat_strdup(s)
#define sprintf     uboot_fat_sprintf
#define rand()      uboot_fat_rand()

/* ==================================================================== block device shim */
/* In u-boot, `disk_read/disk_write` (static inside upstream fat.c) ultimately call
 * `blk_dread/blk_dwrite(desc, start, blkcnt, buf)`. This file provides a **minimal same-shape
 * shim**:
 *   · `struct blk_desc`/`disk_partition` carry only the fields upstream fat.c really touches;
 *   · "whole device = one partition" (start sector 0) ⇒ upstream's `cur_part_info.start + block`
 *     is always just `block`;
 *   · `blk_dread/blk_dwrite` live in `glue/fat_blk.c` (split by geometry.max_rw, reject
 *     out-of-range).
 * Upstream's return contract: success = sectors transferred, failure < 0 (so upstream's
 * `ret != nr_blocks` test holds). */
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

/* Environment stubs called only by upstream functions that **nobody references**
 * (`fat_register_device`/`file_fat_detectfs`): declared here, defined `static inline` ⇒ not even a
 * link dependency. `-ffunction-sections` + `--gc-sections` drop those upstream functions whole
 * (this port mounts via `fat_set_blk_dev` + the volume table). */
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

/* ==================================================================== DM/RTC stubs */
/* With `CONFIG_DM_RTC=0` upstream's RTC branch is dead code but must still type-check:
 * `struct udevice`/`struct rtc_time` (the latter from vendored include/fs.h → rtc_def.h). */
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

/* `tm` is deliberately `void *` (not `struct rtc_time *`): this header may be expanded before
 * vendored `include/fs.h`, and a struct first seen in a prototype is scoped to that prototype
 * only — which would make upstream `dm_rtc_get(dev, &tm)` (tm is fs.h's struct rtc_time) report
 * an incompatible pointer type. `void *` has no such ambiguity, and this branch is dead code
 * (see the header). */
static inline int dm_rtc_get(struct udevice *dev, void *tm)
{
    (void)dev; (void)tm;
    return -1;
}

/* `rtc_calc_weekday()` (u-boot `include/rtc.h`, used by upstream `fat2rtc()`): v1 has no RTC,
 * and `fat2rtc` is called only from `fat_readdir`'s `CONFIG_EFI_LOADER` branch (dead code, cut by
 * `--gc-sections`) ⇒ this is an **environment stub**, present only to type-check that upstream
 * text. */
static inline void rtc_calc_weekday(void *tm)
{
    (void)tm;
}

#endif /* BR_ADAPTER_UBOOT_FAT_ENV_H */
