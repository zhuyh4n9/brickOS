/*
 * adapter/uboot-fat — **u-boot 运行环境**(适配层)
 *
 * ============================== 本文件是什么 ==============================
 * 上游 FAT(`fs/fat/fat.c` + `fs/fat/fat_write.c`, 单编译单元)是**u-boot 风格**的 C:
 * 它假设自己有 `<blk.h>`/`<part.h>`(块设备: `struct blk_desc` + `disk_partition` +
 * `blk_dread/blk_dwrite`)、`<fat.h>`(盘上结构)、`<fs.h>`(目录项/目录流)、`<malloc.h>`
 * 的 `malloc_cache_aligned()`、`<rand.h>` 的 `rand()`、libc 的
 * `malloc/free/strdup/strchr/strrchr/strcasecmp/sprintf`、`printf`/`debug`/`log_err`、
 * `assert()`。本插件是**裸机 unikernel(无 libc)**, 所以这一层把那些假设**逐条落回本项目
 * 的真值** —— 上游 .c **一个字都不用改**(判据 = `tools/check-third-party-pristine.sh`
 * 的哈希锁; 适配 = 给上游环境, 不是改上游)。
 *
 *   u-boot/Linux 侧                            →  brickOS 侧
 *   ---------------------------------------------------------------------------------
 *   u8/u16/u32/u64/s64/loff_t/ulong/uint       →  br_u8/… / br_s64(long long)
 *   bool/true/false                            →  br_bool / BR_TRUE / BR_FALSE
 *   __u8…__le32 + le16_to_cpu/cpu_to_le32      →  恒等函数(aarch64 = 小端; 见下方 #error)
 *   get_unaligned_le16/32                      →  __builtin_memcpy 版本(本目标 -mstrict-align)
 *   malloc/calloc/realloc/free                 →  br_malloc/br_calloc/br_realloc/br_free
 *   memcpy/memset/memcmp/memmove               →  core 提供(项目纪律: 这四个直接用)
 *   strdup/strcpy/strlen/strchr/strrchr/…      →  本文件的 `uboot_fat_*`(见下)
 *   sprintf                                    →  本文件声明的有界格式化器(无 libc)
 *   rand()                                     →  本文件的确定性 LCG(见下)
 *   printf/debug/log_err/log_debug             →  br_log_write(带 "uboot-fat: " 前缀)
 *   assert(x)                                  →  **空语句**(与上游 `_DEBUG=0` 的展开一致)
 *   malloc_cache_aligned/memalign/ALLOC_CACHE_ALIGN_BUFFER → br_malloc(对齐 ≥ 16 B)
 *   struct blk_desc + disk_partition           →  **极小的 shim**(整设备一个分区:
 *                                                 起始扇区 0, 扇区数 = geometry.n_sectors)
 *   blk_dread/blk_dwrite                       →  `disk_read/disk_write` → br_bdev_ops_t.read/write
 *   part_get_info/blk_enabled/dev_print/…      →  环境桩(**只**被上游那些没人引用的函数
 *                                                 `fat_register_device`/`file_fat_detectfs`
 *                                                 调用, --gc-sections 会把它们整段裁掉)
 *
 * ## 为什么字符串要自己写
 *   硬约束(任务书): `strlen/strcmp/strncmp/strcpy/strchr/strrchr/tolower` **必须自己写小的**,
 *   不许依赖 runtime/posix。这里全是 `static inline` + `uboot_fat_` 前缀, 再用**宏**把上游的
 *   裸名字映射过来 —— 于是上游的调用点一个字都不用改, 而链接期绝不会去够一个不存在的
 *   libc 符号(整个编译单元是 `-fno-builtin`, 编译器不会把我们的定义当成内建再自调用)。
 *
 * ## 有界性(环境事实, 不是"随便给个上界")
 *   · `strcpy(dst, src)`: 上游只有两个调用点, 目标都是 **256 字节**的缓冲
 *     (`fat_readdir` 的 `struct fs_dirent.name[256]`(经 vendored `include/fs.h`)与
 *     `normalize_longname` 的 `char l_filename[VFAT_MAXLEN_BYTES=256]`)⇒ 上界 256 是
 *     **上游自己的构造**, 不是我们拍的。
 *   · `strdup(path)`: 本插件的 VFS 面把路径限在 `UB_FAT_PATH_MAX`(256)内
 *     (见 `glue/fat_glue.h`), 超长直接返回 NULL —— 上游调用点对 NULL 的处理是 `-ENOMEM`。
 *   · `sprintf(buf, fmt, …)`: 无 libc 的 `sprintf` 没有"目标多大"这个参数, 本环境按
 *     `UB_FAT_SPRINTF_MAX`(256)封顶并在结尾补 '\0'; 上游四个调用点的目标分别是
 *     `char vol_label[12]`/`char *uuid_str`(契约 ≥10 B)/`char buf[13]`, 格式化结果
 *     都远小于 256 ⇒ 正常路径永不截断(逆序生成、超界丢弃, 见 `glue/fat_blk.c`)。
 *
 * ## 有意偏离(相对上游的**行为**, 逐条可查)
 *   1. `assert(x)` 是空语句 —— **与上游 `include/log.h` 的展开一致**: 上游
 *      `#define assert(x) ({ if (!(x) && _DEBUG) __assert_fail(...); })`, 而两个 fat .c
 *      都没有 `#define DEBUG`、也没有 `CONFIG_LOG`/`LOG_DEBUG` ⇒ `_DEBUG == 0` ⇒
 *      上游这一次编译**本来就不做运行期断言**。本文件用 `if (0) (void)(x)` 保持"表达式
 *      仍被类型检查、运行期不求值"这两条性质。
 *   2. `debug(...)` 缺省**不输出**(同上: 上游 `debug` = `debug_cond(_DEBUG, …)`)。打开
 *      `UBOOT_FAT_VERBOSE_DBG` 可以让它落到 BR_LOG_DEBUG; 上游数据路径的诊断面是 `printf`/
 *      `log_err`, 那两条**始终**输出(它们映射到 BR_LOG_ERROR)。
 *   3. `printf` 与 `log_err` 落到 `br_log_write(BR_LOG_ERROR, ...)`: 上游的 `printf` 是
 *      "打印并继续", 本环境把它变成**一条错误日志**(内容逐字保留, 且格式串与实参仍然被
 *      编译器 `-Wformat` 校验)。控制流的"继续/返回"仍由上游自己的 `return -1` 决定 ——
 *      我们**没有**改上游的返回口径。
 *   4. `rand()` 是**确定性**的 32 位 LCG(u-boot 的 `rand()` 找不到上游定义, 见 NOTICE):
 *      它只被 `set_name()` 用来挑短名后缀的搜索起点, 而候选空间本来就是确定的(逐个试到
 *      不冲突为止)⇒ 契约不变, 顺带去掉一处不确定性。
 *   5. DM/RTC/partition 那一族是**环境桩**: `CONFIG_DM_RTC=0`/`CONFIG_EFI_LOADER=0`, 于是
 *      上游对应分支是死代码(编译期仍要类型正确), 而 `part_get_info` 等只被
 *      `fat_register_device`(本件的挂载走 `fat_set_blk_dev` + 我们自己的卷表)调用 ⇒
 *      `-ffunction-sections` + `-Wl,--gc-sections` 不会把它们放进镜像。
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_FAT_ENV_H
#define BR_ADAPTER_UBOOT_FAT_ENV_H

/* `NULL`/`size_t` 来自**编译器自带的** <stddef.h>(不是 libc); uintN_t 由 <stdint.h> */
#include <stddef.h>
#include <stdint.h>

#include <br/core/br_error.h>
#include <br/core/br_errno.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
/* 块设备契约: 上游从 `<blk.h>`/`<part.h>` 拿, 本件从这里拿。**只有本头**(适配层入口)
 * 可以 include `<br/...>` —— 上游 .c 与 glue 都经本头取运行环境。 */
#include <br/dev/br_bdev.h>

/* ==================================================================== 小端前提 */
/* 上游的 `FAT2CPU16/32`(vendored `include/fat.h` 的 `#if __LITTLE_ENDIAN`)与
 * `le16_to_cpu()` 在本件里是**恒等** ⇒ 只有当目标真的小端时才成立。aarch64 的 QEMU virt
 * 与绝大多数同类板子都是小端; 大端目标下这份环境是错的, 所以宁可在这里停住。 */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-fat 的 FAT2CPU16/32 与 leN_to_cpu() 是恒等实现: 本移植只支持小端目标"
#endif
/* vendored `include/fat.h` 的 `FAT2CPU16/32` 走 `#if __LITTLE_ENDIAN` 这条分支 —— 给它
 * 一个真值(u-boot 的 `include/linux/byteorder/little_endian.h` 也是这么定义的)。 */
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif

/* ==================================================================== 基础类型 */
/* ★ 逐字按 **u-boot 的宽度**给, 不是"同宽的 br_*":
 *   u-boot 的 `u64`/`s64`/`loff_t` 都是 `unsigned long long`/`long long`, 而本仓库的
 *   `br_u64`/`br_s64` 是 `unsigned long`/`long`(LP64 同宽**不同型**)。这不是洁癖:
 *   上游满地的 `%llu`/`%lld` 与实参的**类型**绑定, 用 br_* 会得到 `-Werror=format=`
 *   (探针实测踩到, 与 ADR-0026 §3.1 记的 erofs 同一条坑)。 */
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

_Static_assert(sizeof(u64) == 8u, "u64 必须是 64 位");
_Static_assert(sizeof(u8) == 1u, "u8 必须是 8 位");
_Static_assert(sizeof(u64) == sizeof(unsigned long long), "u64 = u-boot 的 long long");
_Static_assert(sizeof(loff_t) == 8u, "loff_t 必须是 64 位(上游 aarch64 口径)");
_Static_assert(sizeof(u16) == 2u, "u16 必须是 16 位");
_Static_assert(sizeof(u32) == 4u, "u32 必须是 32 位");

/* ---- on-disk 整数族(u-boot `include/linux/types.h` 的那一族) ---- */
typedef unsigned char      __u8;
typedef unsigned short     __u16;
typedef unsigned int       __u32;
typedef unsigned long long __u64;
typedef __u16 __le16;
typedef __u32 __le32;
typedef __u64 __le64;

_Static_assert(sizeof(__u64) == 8u, "__u64 必须是 64 位");

/* ==================================================================== 字节序(恒等) */
static inline u16 uboot_fat_le16_to_cpu(u16 v) { return v; }
static inline u32 uboot_fat_le32_to_cpu(u32 v) { return v; }
static inline u16 uboot_fat_cpu_to_le16(u16 v) { return v; }
static inline u32 uboot_fat_cpu_to_le32(u32 v) { return v; }
#define le16_to_cpu(x) uboot_fat_le16_to_cpu(x)
#define le32_to_cpu(x) uboot_fat_le32_to_cpu(x)
#define cpu_to_le16(x) uboot_fat_cpu_to_le16(x)
#define cpu_to_le32(x) uboot_fat_cpu_to_le32(x)

/* 非对齐取小端(上游的 `get_unaligned_le16/32`): 目标开了 `-mstrict-align`, 所以一律走
 * __builtin_memcpy(GCC 在 aarch64 上把它降成单条 `ldr`/`str`, 不会真去调 libc 的 memcpy)。 */
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

/* ==================================================================== 内存 */
/* 分配器: 名字接过去(br_* 是 core 的真值; 没有 libc)。为什么用宏而不是声明一个
 * `void *malloc(size_t)`: 那样会变成**对运行期的链接依赖**(`runtime/posix` 目前提供
 * malloc, 而本插件是介质 FS, 不该依赖 POSIX 面)。宏没有这个歧义。
 * `malloc_cache_aligned`/`memalign`: core 堆的对齐 ≥ 16 B(BR_MALLOC_ALIGN), 覆盖本插件的
 * 全部对齐需求(最大自然对齐 8 B; 上游那句"FAT buffer 要 32 位对齐"同理成立) ⇒
 * 对齐这一层没有对象, 直接给 br_malloc。 */
#define malloc(n)             br_malloc(n)
#define calloc(n, s)          br_calloc((n), (s))
#define realloc(p, n)         br_realloc((p), (n))
#define free(p)               br_free(p)
#define memalign(a, n)        ((void)(a), br_malloc(n))
#define malloc_cache_aligned(n) br_malloc(n)

/* `memcpy/memset/memcmp/memmove` **由 core 提供**(core/src/string.c, 随 core 链进镜像;
 * 项目纪律: 这四个直接用, 不自己写、也不依赖 runtime/posix)。core 的 string_internal.h 是
 * core 私有, 所以这里写下原型 —— 有原型才有类型检查(`-Wmissing-prototypes` 的另一面)。 */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* u-boot 的 DMA 对齐宏(`include/asm/cache.h` + `include/memalign.h`)。本件没有 DMA 约束
 * (块层是裸 memcpy 到设备缓冲, 驱动自己有 DMA) ⇒ 值取 16(与 BR_MALLOC_ALIGN 同),
 * `ALLOC_CACHE_ALIGN_BUFFER` 给出**真实 16 B 对齐**的栈上指针形态(与上游同名同义)。 */
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

/* u-boot `include/linux/compiler.h` / `include/linux/compiler_types.h` 里被本件**真正用到**
 * 的属性宏(`fat.c` 的 `static void __maybe_unused fat2rtc(...)`)。其余的编译期属性宏不提供
 * —— 环境只给上游用得上的名字, 不给一整套近似的 Linux 头(见 `compat/linux/compiler.h`)。 */
#define __maybe_unused  __attribute__((__unused__))
#define __packed        __attribute__((__packed__))
#define __aligned(x)    __attribute__((__aligned__(x)))
#define __always_inline inline __attribute__((__always_inline__))
#ifndef __force
#define __force
#endif

/* ==================================================================== 工具宏 */
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

/* `fls(i)`(上游 `set_name` 用它做"随机短名后缀"的位宽): 最高置位的位置, 1 起算, fls(0)=0。 */
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

/* `is_power_of_2()`(上游 `is_bootsector_valid` 用; u-boot `include/linux/log2.h` 的原义) */
static inline bool is_power_of_2(unsigned long n)
{
    return (bool)((n != 0ul) && ((n & (n - 1ul)) == 0ul));
}

/* `ilog2()`(u-boot `include/linux/log2.h`; 本件的 fat 源码没用到, 但环境头与上游同名同义,
 * 免得将来上游用到时又要"补一个近似的")。 */
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

/* `do_div`/`lldiv`/`div_u64_rem`(上游 `include/div64.h` + `include/linux/math64.h`)。
 * 目标 BITS_PER_LONG == 64 ⇒ 与 u-boot 的 64 位分支逐字同义。 */
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

/* `BUILD_BUG_ON` —— 按 u-boot 的机制(函数体内 `if` + `__compiletime_error`)兑现, 不用
 * `_Static_assert`: 上游 `include/linux/build_bug.h` 允许条件不是整型常量表达式。本件的
 * fat 源码没有调用点, 但 vendored 头转发进来时可能间接用到(环境完整比"恰好够用"更稳)。 */
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

/* `assert()`: 见文件头"有意偏离(1)"—— 上游这一次编译的 `_DEBUG == 0`。表达式仍然被类型
 * 检查(`(void)(x)` 在 `if (0)` 里), 运行期不求值。 */
#define assert(x) do { if (0) { (void)(x); } } while (0)

/* ==================================================================== 编译期配置 */
/* u-boot 用 Kconfig 的 `CONFIG_*` 开关裁代码。本件给它们**显式真值**, 于是上游的
 * `CONFIG_IS_ENABLED(...)` 分支逐字成立(不是我们把分支删掉):
 *   FAT_WRITE=1  ⇒ vendored fat.c 不编那份"只读 flush_dirty_fat_buffer 桩"
 *                  (真身在上游 fat_write.c 里 —— 同一个编译单元, 只有一份)
 *   EFI_LOADER=0 ⇒ fat_readdir 不填时间戳(本件 v1 无 RTC)
 *   DM_RTC=0     ⇒ 写路径不取 RTC 时间, 用上游自己的 2000-01-01 兜底常量
 *   FS_EXFAT=0   ⇒ 目录项名字段 = 256 字节(见 vendored include/fs.h)
 *   FS_FAT_MAX_CLUSTSIZE: u-boot 的缺省值(32 KiB), vendored fat.h 的 MAX_CLUSTSIZE 用它 */
#define CONFIG_FAT_WRITE           1
#define CONFIG_EFI_LOADER          0
#define CONFIG_DM_RTC              0
#define CONFIG_FS_EXFAT            0
#define CONFIG_FS_FAT_MAX_CLUSTSIZE 32768
#define CONFIG_LOGLEVEL            8
#define CONFIG_LOG                 0
#define CONFIG_LOGF_FUNC           0

/* `CONFIG_IS_ENABLED(opt, [case1], [case2])` / `IS_ENABLED(opt)`: 与 u-boot 同形的
 * 变参选择(1 参 ⇒ 0/1 常量; 3 参 ⇒ 选一支)。上游在本件里的四个用法都在作用域内:
 * `#if !CONFIG_IS_ENABLED(FAT_WRITE)`、`if (CONFIG_IS_ENABLED(EFI_LOADER))`、
 * `if (CONFIG_IS_ENABLED(DM_RTC))`、`CONFIG_IS_ENABLED(FS_EXFAT, (1024), (256))`。 */
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

/* u-boot 的日志分类/等级: 上游两个 .c 都写 `#define LOG_CATEGORY LOGC_FS`, 而
 * `log_err`/`log_debug` 由本文件映射到 br_log_write(不用分类)⇒ 这里只需要让那个宏**有个
 * 被定义的名字**(否则 `-Wundef` 在它被展开时报未定义)。 */
#define LOGC_FS 0

/* ==================================================================== 日志 */
/* 见文件头"有意偏离(2)(3)": printf/log_err 始终输出(ERROR 级), debug 缺省不输出。
 * ★ 为什么直接拼串而不是"先格式化再转发": 上游的格式串**全是字符串字面量**(逐个调用点
 *   查过), 于是 `"uboot-fat: " fmt` 是编译期拼接, 实参的类型检查仍由 br_log_write 的
 *   `format(printf, …)` 属性给出。零个可变参数时靠 GNU 的 `##__VA_ARGS__`(与
 *   adapter/uboot-erofs 的 pr_err 同一手法)。 */
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

/* ==================================================================== 字符串(自研) */
/* 全部 `static inline` + `uboot_fat_` 前缀(硬约束: 不依赖 runtime/posix), 再用宏把上游的
 * 裸名字接过来。长度纪律: 一律只按调用方给的 `n`(或本文件给出的**上游构造**上界)走。 */
#define UB_FAT_STRCPY_CAP   256u   /* 上游两个 strcpy 目标都是 256 B 的缓冲(见文件头) */
/* `strdup` 的上界 = 本插件 VFS 面的路径上界(`UB_FAT_PATH_MAX` = 256, 见 glue/fat_glue.h)
 * + 上游调用点还会自带一个前导 '/' 与结尾 '\0' ⇒ 258。超长返回 NULL(上游对 NULL 的答复
 * 是 -ENOMEM), 于是"路径过长"与"没内存"在上游眼里是同一类失败, 不会静默截断。 */
#define UB_FAT_STRDUP_CAP   (256u + 2u)
#define UB_FAT_SPRINTF_MAX  256u   /* 无 libc 的 sprintf 在环境侧的封顶(见文件头) */

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

/* 有界复制(cap 含结尾 '\0'; 上界见文件头: 上游两个目标都是 256 B 的缓冲) */
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

/* ASCII 小写(上游 `tolower`, 只处理 'A'..'Z'; 非 ASCII 原样返回 —— 上游同样假设 8 位代码页) */
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

/* 认得出是移植: 上游的裸名字 → 我们的实现(整编译单元 -fno-builtin, 不会自撞) */
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

/* `strdup`/`sprintf`/`rand`: 由 `glue/fat_blk.c` 实现(有界/确定性; 见文件头)。
 * 声明 + 宏而不是"实现放头里": 它们是真正的跨模块符号, 有原型才有 -Wmissing-prototypes 的
 * 另一面, 也只在本插件里有一份真值。 */
char *uboot_fat_strdup(const char *s);
int   uboot_fat_sprintf(char *dst, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
int   uboot_fat_rand(void);

#define strdup(s)   uboot_fat_strdup(s)
#define sprintf     uboot_fat_sprintf
#define rand()      uboot_fat_rand()

/* ==================================================================== 块设备 shim */
/* u-boot 里 `disk_read/disk_write`(上游 fat.c 内的 static)最终调
 * `blk_dread/blk_dwrite(desc, start, blkcnt, buf)`。本件给出一组**同形状的极小 shim**:
 *   · `struct blk_desc`/`disk_partition` 只有上游 fat.c 真正碰到的字段;
 *   · "整设备一个分区"(起始扇区 0) ⇒ 上游的 `cur_part_info.start + block` 恒等于 `block`;
 *   · `blk_dread/blk_dwrite` 的实现在 `glue/fat_blk.c`(按 geometry.max_rw 切块、越界拒绝)。
 * 返回值的上游口径: 成功 = 传输的扇区数, 失败 < 0(于是上游的 `ret != nr_blocks` 判据成立)。 */
typedef br_u64 lbaint_t;

struct blk_desc {
    int           uclass_id;   /* u-boot: enum uclass_id */
    int           devnum;      /* 设备号(本件恒 0) */
    lbaint_t      lba;         /* 扇区总数 */
    unsigned long blksz;       /* 扇区字节数 */
    int           log2blksz;
    char          vendor[9];   /* 只被 file_fat_detectfs → dev_print 用(死代码) */
};

#define PART_NAME_LEN 36
#define PART_TYPE_LEN 36

struct disk_partition {
    lbaint_t start;                  /* 分区首扇区(本件恒 0) */
    lbaint_t size;                   /* 分区扇区数 = 设备扇区数 */
    ulong    blksz;                  /* 扇区字节数 */
    uchar    name[PART_NAME_LEN];
    uchar    type[PART_TYPE_LEN];
    int      bootable;
};

unsigned long blk_dread (struct blk_desc *desc, lbaint_t start, lbaint_t blkcnt, void *buf);
unsigned long blk_dwrite(struct blk_desc *desc, lbaint_t start, lbaint_t blkcnt, const void *buf);

/* 只被上游**没人引用**的函数调用的环境桩(`fat_register_device`/`file_fat_detectfs`):
 * 声明在这里, 定义成 `static inline` ⇒ 连链接依赖都没有。`-ffunction-sections` +
 * `--gc-sections` 会把那些上游函数整段裁掉(本件的挂载走 `fat_set_blk_dev` + 卷表)。 */
static inline int part_get_info(struct blk_desc *desc, int part, struct disk_partition *info)
{
    (void)desc; (void)part; (void)info;
    return -1;                    /* 本件没有分区表(整设备一个分区) */
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

/* ==================================================================== DM/RTC 环境桩 */
/* `CONFIG_DM_RTC=0` 时上游的 RTC 分支是死代码, 但仍要类型正确:
 * `struct udevice`/`struct rtc_time`(后者来自 vendored include/fs.h → rtc_def.h)。 */
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

/* `tm` 故意写 `void *`(而不是 `struct rtc_time *`): 本头可能先于 vendored `include/fs.h`
 * 被展开, 而函数原型里的 struct 若在该处首次出现, 作用域只到原型为止 —— 那会让上游
 * `dm_rtc_get(dev, &tm)`(tm 是 fs.h 的 struct rtc_time)报"指针类型不兼容"。`void *` 没有
 * 这个歧义, 且这条分支是死代码(见文件头)。 */
static inline int dm_rtc_get(struct udevice *dev, void *tm)
{
    (void)dev; (void)tm;
    return -1;
}

/* `rtc_calc_weekday()`(u-boot `include/rtc.h`, 上游 `fat2rtc()` 用): 本件 v1 没有 RTC,
 * 而 `fat2rtc` 只被 `fat_readdir` 的 `CONFIG_EFI_LOADER` 分支调用(死代码, 会被
 * `--gc-sections` 裁掉)⇒ 这里是**环境桩**, 只为让那段上游原文类型正确。 */
static inline void rtc_calc_weekday(void *tm)
{
    (void)tm;
}

#endif /* BR_ADAPTER_UBOOT_FAT_ENV_H */
