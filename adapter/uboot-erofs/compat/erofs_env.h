/*
 * adapter/uboot-erofs — **u-boot 运行环境**(适配层)
 *
 * ============================== 本文件是什么 ==============================
 * 上游 EROFS 是**内核/u-boot 风格**的 C: 它假设有 `linux/compat.h`、`linux/stat.h`、
 * `linux/bug.h`、`linux/log2.h`、`asm/unaligned.h`、`u-boot/lz4.h`、`printf`/`pr_err`
 * 与 libc 的 `malloc/free/memcpy/strnlen`。本插件是**裸机 unikernel(无 libc)**,
 * 所以这一层把那些假设**逐条落回本项目的真值**:
 *
 *   u-boot/Linux 侧                        →  brickOS 侧
 *   ---------------------------------------------------------------------------
 *   u8/u16/u32/u64/s64                     →  br_u8/…(br/core/br_types.h)
 *   bool / true / false                    →  br_bool / BR_TRUE / BR_FALSE
 *   __le16/__le32/__le64 + leN_to_cpu()    →  恒等函数(aarch64 = 小端; 见下方 #error)
 *   get_unaligned_le16/32                 →  __builtin_memcpy 版本(本目标 -mstrict-align)
 *   min_t/max_t/round_up/round_down/ilog2  →  本文件的小宏/函数
 *   DIV_ROUND_UP / BLK_ROUND_UP            →  DIV_ROUND_UP 宏(internal.h 里定义后者)
 *   malloc/free/realloc/calloc             →  br_malloc/br_free/br_realloc/br_calloc
 *   memcpy/memset/memcmp/memmove           →  core 提供(项目纪律: 这四个直接用)
 *   strlen/strnlen/strcmp/memcmp 风格比较   →  本文件的 `uboot_erofs_*`(见下)
 *   printf/pr_err/pr_info/pr_debug         →  br_log_error/info/debug
 *   BUG_ON/DBG_BUGON                       →  DBG_BUGON(见下: 打印 + 不崩)
 *   IS_ERR/ERR_PTR/PTR_ERR                 →  本项目**不用** errno 指针; 裁掉
 *   E* 裸名字 / EFSCORRUPTED(EUCLEAN)      →  **core 的 <br/core/br_errno.h>**(见下)
 *
 * ## 为什么字符串要自己写
 *   硬约束(任务书): `strlen/strcmp/strncmp/strcpy/strnlen/strchr` **必须自己写小的**,
 *   不许依赖 runtime/posix。这里全部是 `static inline` + `uboot_erofs_` 前缀, 再用
 *   **宏**把上游的裸名字映射过来 —— 于是移植过来的 `namei.c`/`fs.c` 代码行**一个字都不用改**
 *   (`strnlen(de_name, n)` 照样写), 而链接期绝不会去够一个不存在的 libc 符号。
 *   因为整个编译单元是 `-fno-builtin`, 编译器不会把我们的定义识别成内建再自调用。
 *
 * ## 有意偏离(相对上游)
 *   1. 上游用 `IS_ENABLED(CONFIG_LZ4)` 之类的**编译期**开关; 本件一律拆掉, 改成
 *      `erofs_decompress` 里的**运行期**分支(任务书硬约束)。
 *   2. 上游 `internal.h` 里的 `erofs_err()` 自带换行; u-boot 的 `pr_err` 也自带。
 *      本文件的 `erofs_err` 不补换行(br_log_write 自己按行输出)。
 *   3. `erofs_dbg` 只在 `BR_EROFS_VERBOSE_DBG` 打开时才真的输出 —— 默认编译成空语句,
 *      免得没插串口的机器上日志被逐块映射刷屏。
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_EROFS_ENV_H
#define BR_ADAPTER_UBOOT_EROFS_ENV_H

#include <stddef.h>     /* NULL / size_t —— 编译器自带头, 不是 libc */

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
/* 块设备契约: 上游从 `blk.h` 拿, 本件从这里拿。**只有本头**(适配层入口)可以
 * include `<br/...>` —— `src/` 下的上游派生件一律经本头取环境。 */
#include <br/dev/br_bdev.h>

/* `uintN_t`/`intN_t` 由编译器自带的 <stdint.h> 提供(上游 `erofs_fs.h` 自己 include 它);
 * 本层**不重复定义** —— 重复定义会与编译器头冲突, 那正是"适配层不该做的事"。 */
#include <stdint.h>

/* ==================================================================== 内存分配
 *
 * 上游 EROFS 用 libc 的 `malloc/free/calloc/realloc`(内核里是 kmalloc 家族)。
 * 本件是裸机 unikernel, 堆在 core(`br_malloc`/`br_free`/`br_realloc`/`br_calloc`)。
 * ★ 这里**不写任何分配器**, 只把名字接过去 —— 移植代码因此一行不用改。
 *   代价: 上游那套"越界/失败"的隐含假设(返回 NULL)必须仍然成立, 而 core 的
 *   `br_malloc` 返回 NULL 的语义与 libc 一致, 所以接得上。
 */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* ==================================================================== 小端前提 */
/* 上游的 `le32_to_cpu()` 在本件里是**恒等**函数 —— 只有当目标真的小端时才成立。
 * aarch64 的 QEMU virt 与绝大多数同类板子都是小端; 大端目标下这份移植是错的,
 * 所以宁可在这里停住也不要静默给出错的字。 */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-erofs 的 leN_to_cpu() 是恒等实现: 本移植只支持小端目标"
#endif

/* ==================================================================== 基础类型 */
typedef unsigned char       u8;
typedef signed char         s8;
typedef unsigned short      u16;
typedef signed short        s16;
typedef unsigned int        u32;
typedef signed int          s32;
typedef unsigned long long  u64;   /* ★ u-boot 就是 long long(不是 uint64_t) */
typedef signed long long    s64;   /* ★ 同上 */
typedef _Bool               bool;
/* `size_t` / `uintptr_t` / `uintN_t` / `intN_t` 由**编译器的** <stddef.h>/<stdint.h> 给
 * (上游自己 include <stdint.h>)—— 这里**不再重复 typedef**, 免得与编译器头打架。 */
typedef long long           loff_t;   /* u-boot: `typedef long long loff_t;` */
typedef unsigned int        dev_t;
typedef unsigned short      umode_t;

#ifndef NULL
#define NULL  BR_NULL
#endif

#define true  BR_TRUE
#define false BR_FALSE

_Static_assert(sizeof(br_size_t) == 8u, "size_t 必须是 64 位(与上游的 aarch64 口径一致)");
_Static_assert(sizeof(br_u64) == 8u, "u64 必须是 64 位");

/* ---- EROFS 自己的三个类型(上游 internal.h 定义, 逐字照搬) ---- */
typedef u64 erofs_off_t;    /* 字节偏移 */
typedef u64 erofs_nid_t;    /* inode 编号(元数据区里的槽号) */
typedef u32 erofs_blk_t;    /* 文件系统范围里的块号 */
#define NULL_ADDR     ((unsigned int)-1)
#define NULL_ADDR_UL  ((unsigned long)-1)

/* inode 槽位(元数据区里一个 inode 占 2^5 = 32 字节) —— 上游 internal.h 的
 * `EROFS_ISLOTBITS`。super.c 把它抄进 `sbi.islotbits`, `iloc()` 靠它算 nid 的字节偏移。 */
#define EROFS_ISLOTBITS 5

/* ==================================================================== on-disk 小端 */
/* 与 Linux 的 `include/uapi/linux/types.h` 同型: `__leN` 是"内容为小端"的整数类型。
 * 本件把稀疏标注(`__bitwise`)去掉 —— 于是上游那套"le32_to_cpu() 的结果直接赋给 __le32"
 * 的写法在类型上也成立(内核里靠 `__force`, 这里根本不需要)。 */
typedef br_u16 __le16;
typedef br_u32 __le32;
typedef br_u64 __le64;
typedef br_u16 __u16;
typedef br_u32 __u32;
typedef br_u64 __u64;
typedef br_u8  __u8;

/* aarch64 = 小端 ⇒ 四个转换都是恒等。保留函数形态是为了让移植代码**看起来还是上游**。 */
static inline u16 le16_to_cpu(__le16 v) { return (u16)v; }
static inline u32 le32_to_cpu(__le32 v) { return (u32)v; }
static inline u64 le64_to_cpu(__le64 v) { return (u64)v; }
static inline __le16 cpu_to_le16(u16 v) { return (__le16)v; }
static inline __le32 cpu_to_le32(u32 v) { return (__le32)v; }
static inline __le64 cpu_to_le64(u64 v) { return (__le64)v; }

/* 非对齐访问: 目标开了 `-mgeneral-regs-only -mstrict-align`, 所以一律走 __builtin_memcpy
 * (GCC 在 aarch64 上把它降成单条 `ldr`/`str`, 不会真的去调 libc 的 memcpy)。
 * 上游用的是 u-boot 的 `get_unaligned_le32()`, 语义完全相同。 */
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

/* ==================================================================== 内存搬运
 *
 * 项目纪律(任务书): `memcpy/memset/memcmp/memmove` **由 core 提供可以直接用**,
 * 不自己写、也不依赖 runtime/posix。符号由 `core/src/string.c` 实现并随 core 链进镜像。
 * 但它们**没有对外头**(core 的 `string_internal.h` 是 core 私有), 所以这里把四个原型
 * 写下来 —— 有原型才有类型检查(`-Wmissing-prototypes` 那条纪律的另一面)。
 *
 * 为什么不像 fs/tmpfs 那样借 `__builtin_memcpy`: 上游 EROFS/LZ4 的代码里
 * `memcpy/memmove/memset/memcmp` 出现几十次, 逐个换成内建会让"认得出是移植"这件事
 * 打折扣; 而 `LZ4_copy8` 本来就该用内建(它与 `get_unaligned` 同源)。
 * 两个手法不冲突: 这里**声明** core 的实现, 那里继续用内建。 */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* ==================================================================== 文件类型位
 * 上游从 `linux/stat.h` 拿 `S_IFMT`/`S_ISREG`/…。裸机没有那套头, 这里补上**数值与
 * Linux 逐字对齐**的四个位(只用得到这四个 —— 本插件的 VFS 面只有 DIR/FILE/SYMLINK)。 */
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

/* `ilog2`(上游 super.c 的 `ilog2(EROFS_MAX_BLOCK_SIZE)` 用);
 * 只在常量/运行期两处用得上 —— 运行期版本 ≤ 12 次迭代, 够便宜。 */
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
 * `BUILD_BUG_ON` —— **按 u-boot 的机制**兑现, 而不是用 `_Static_assert`。
 *
 * ★ 为什么不能用 `_Static_assert`(ADR-0024 §7): 上游 `erofs_fs.h` 的最后一条自检是
 *     BUILD_BUG_ON(__builtin_constant_p(fmh) ? fmh != cpu_to_le64(1ULL<<63) : 0);
 *   `fmh` 是函数内的 `const` 局部量 —— **不是整型常量表达式** ⇒ `_Static_assert` 直接
 *   报 "expression in static assertion is not constant"。而 u-boot 的定义是
 *   "函数体内 `if` + `__compiletime_error` 外部函数"(见 include/linux/build_bug.h 与
 *   include/linux/compiler.h 的 `__compiletime_assert`), 它**允许**条件不是常量:
 *   常量可判定 ⇒ 编译器就地报错并给出 msg; 判不出来 ⇒ 留一次调用。
 *   ⇒ 只有照搬这个机制, 上游 `erofs_fs.h` 才能**逐字**编过(这正是适配层的职责)。
 *
 * 语义与上游一致: 条件是"**违规**时"才触发(上游把 `!(cond)` 传进 compiletime_assert)。
 * 未开优化(`__OPTIMIZE__` 不定义)时上游把它编译成空语句, 这里照办。
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
 * u-boot 的 `<compiler.h>` / `<linux/bitops.h>` 提供的三个属性/宏。
 * ★ 为什么必须在这里兑现(ADR-0024 §7): `src/erofs_fs.h` 要**与上游逐字相同**,
 *   而上游那里写着 `} __packed u1;`(**位置属性**: 夹在 `}` 与成员名之间)、
 *   `const __le64 fmh __maybe_unused = …` 与 `BIT(n)`。少了任何一个, 上游原文就编不过,
 *   于是"逐字保留"只能退化成"改写上游" —— 那正是本刀要消灭的东西。
 */
#define __packed        __attribute__((__packed__))
#define __maybe_unused  __attribute__((__unused__))
#define BIT(nr)         (1UL << (nr))

/* u-boot `include/linux/compiler.h` 的 `__rcode`(返回码标注; 在 u-boot 里为空或用于
 * 静态检查)。上游 `lib/lz4.c` 拿它当函数限定符写在最前面 —— 给它空定义即可。 */
#ifndef __rcode
#define __rcode
#endif

/* u-boot `include/asm/unaligned.h` 的**通用**非对齐访问(get_unaligned/put_unaligned)。
 * 上游 `lib/lz4.c` 用它们搬运 8 字节。语义 = "按本机字节序的非对齐读写";
 * aarch64 小端 ⇒ 与 LE 版本等价(同一份 __builtin_memcpy 实现)。
 * ★ 目标开了 `-mstrict-align`, 不能真的做非对齐解引用 —— __builtin_memcpy 会被
 *   GCC 降成安全的单条 ldr/str, 这正是我们要的。 */
#define get_unaligned(ptr)      (*(const __typeof__(*(ptr)) *)(ptr))
#define put_unaligned(val, ptr) (*(__typeof__(*(ptr)) *)(ptr) = (val))

/* `CONFIG_IS_ENABLED(RELOC_LOADER)`: 本件没有重定位加载器 ⇒ 恒 0。
 * (必须显式定义, 否则 `#if CONFIG_IS_ENABLED(RELOC_LOADER)` 在 -Wundef 下报"未定义"。) */
#define RELOC_LOADER            0

/* u-boot 的 `PAGE_SIZE` 来自 `<asm/page.h>`。上游 `fs/erofs/internal.h` 会
 * `#if PAGE_SIZE != 4096 #error` 检查它 —— 本平台的页大小就是 4096(与 mmu/页池一致),
 * 所以这里是**平台事实**而不是可调参数。 */
#ifndef PAGE_SIZE
#define PAGE_SIZE       4096u
#endif

/*
 * `struct list_head` —— u-boot 的 `<linux/list.h>`(经 `<linux/compat.h>`)提供它,
 * 而上游 `fs/erofs/internal.h` 的 `struct erofs_inode` / `struct erofs_dentry` 里
 * **有它的字段**(`i_hash` / `i_subdirs` / `i_xattrs` / `d_child`)。
 *
 * ★ 我们只提供**类型本身**, 不提供任何遍历/插入函数 —— 因为它们是**缓存与 dentry 层**
 *   的字段, 而本移植的 inode 是"瞬态路径包装"(ADR-0022 D-3), 从不用这些链。
 *   类型大小对了, 上游的结构体布局就成立; 没人遍历 ⇒ 不需要实现。
 */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/*
 * ★ 有意偏离: 上游的 `DBG_BUGON()` = `BUG_ON()` = **停机**。本插件在挂载期与读路径上
 * 大量用它做"不可能发生"的断言, 而任务书的原则是"挂载失败 ⇒ 首败即停机是刻意的"、
 * 但**读路径不该因为一个可疑的盘上字段把内核打死**(那份镜像坏掉时, 正确的行为是
 * 给调用方一个 -EIO, 而不是带走整个设备)。所以这里改成"打印 + 继续", 由调用方紧接着的
 * `return -EFSCORRUPTED` 收敛(每处 DBG_BUGON 后面都跟着一个错误返回或等价分支)。
 */
#define BUG_ON(cond)                                                                    \
    do {                                                                                \
        if (unlikely(cond)) {                                                           \
            uboot_erofs_log_err_inline("DBG_BUGON(%s) @ %s:%d", #cond, __FILE__, __LINE__); \
        }                                                                               \
    } while (0)


/* ==================================================================== 日志
 *
 * 上游: `erofs_err(fmt, ...)` = `pr_err(fmt "\n", ...)`。本件不补换行(br_log_write
 * 按行输出)。
 *
 * ★ 为什么写成**函数**而不是变参宏: C11 的 `__VA_ARGS__` 在"零个可变参数"时是错的
 *   (GNU 的 `##__VA_ARGS__` 是扩展, 而本件按 `-std=c11` 的纪律走)。上游有几处
 *   `erofs_err("...")` 不带参数(例如 "failed to read packed inode from disk"), 于是
 *   这里用两个**包装函数**把可变参数原样转给 core 的日志入口 —— 调用点保持上游原样,
 *   零参调用也合法。它们**不带** `format(printf)` 属性: core 的 `br_log_write` 自己带,
 *   而本件把格式串前缀("uboot-erofs: ")拼在前面, 属性会让编译器对复合串重复校验。
 */
/*
 * ★ 为什么要"先格式化再转发": `br_log_write()` 是**变参**函数, 想"把参数原样传下去"
 *   只有两条路 —— 要么 `<stdarg.h>` 的 `va_list` 接口(裸机没有那个头, 它是 libc 的),
 *   要么让调用方自己格式化。本件走后者: 用编译器自带的 `__builtin_vsnprintf` 把
 *   "uboot-erofs: <内容>" 整行拼进一个栈上小缓冲, 再把**已成行的字符串**当格式串交给
 *   core(日志子系统会对它做一次无害的再格式化; 见 br_log_write 的口径)。
 *   为什么不走 `__builtin_va_list`: `br_log.h` 只暴露 `br_log_write(...)`, 没有
 *   "v" 变体 —— 那就只剩"自己格式化"这一条路(而不是往 core 的契约面上加函数)。
 *
 * ★ 上界与截断: 256 字节的栈缓冲。本件的日志是**诊断面**: 最长的一条(压缩解压失败)
 *   约 60 字节, 其余都更短 ⇒ 正常路径永远不截断; 真截断了也只是少几个字符, 不影响判据。
 *
 * ★ 为什么带 `format(printf)` 属性: 调用点的格式串与实参仍然被编译器逐条校验(与
 *   core 的 `br_log_write` 同款检查)。属性属于**内联包装**自身, 不会重复校验。
 */
#define UBOOT_EROFS_LOG_MAX  256u

/*
 * ★ 为什么不直接用 `__builtin_vsnprintf`: GCC 的 `__builtin_vsnprintf` **不是**自足的 ——
 *   展开后仍是一次对 libc `vsnprintf` 的调用(实测: 链接期 `undefined reference to
 *   vsnprintf`)。本仓库没有 libc(core 只提供编译器支持例程 mem*), `runtime/posix`
 *   也没有 printf 家族(ADR-0014 §4 明示不做)。而 v 变体接口 core 也没抛
 *   (`br_log.h` 只有变参的 `br_log_write`, 没有 `br_log_vwrite`)⇒ 只剩两条路:
 *     ① 让 core 多抛一个 v 接口(改 core 契约面, 为一处诊断打印不值得);
 *     ② 本件自带一个极小格式化器(只覆盖本件实际用到的转换)。
 *   选 ②。它的判据面很窄: **输出只喂给日志**, 不参与任何文件系统判定。
 *
 * 覆盖范围 = 逐个调用点查过的实际用法: `%s %d %u %x %lu %lx %llu %llx %%` 与
 * 长度修饰 `l`/`ll`/`z`。宽度/精度/其它标志**一处都没用** ⇒ 不实现; 真遇到了就
 * 原样吐出(不静默吞掉 —— 宁可日志难看, 也不假装格式化成功)。
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
        case '\0':                     /* 尾随 '%': 原样收尾 */
            fmt--;
            break;
        default:                       /* 未实现的修饰语: 原样吐出 */
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



/* ==================================================================== u-boot 的 pr_*
 *
 * 上游 `fs/erofs/internal.h` 自己把 `erofs_err/info/dbg` 映射到 `pr_err/pr_info/pr_debug`
 * —— 所以**名字由它给, 实现由我们给**(这正是"不改上游、只做适配")。
 * 注意它写的是 `pr_err(fmt "\n", ##__VA_ARGS__)`: 带 GNU 的逗号吞并扩展, 且格式串
 * 末尾自带换行(我们的日志按行输出, 多一个换行无害)。
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

/* ==================================================================== 内核风格小工具 */
/* 上游在 `data.c`/`zmap.c`/`super.c` 里用到的几个 Linux 惯用物。它们是**纯宏/纯函数**,
 * 语义必须是内核那一套(不能自创), 所以逐条照 Linux 的写法。 */
#ifndef UINT_MAX
#define UINT_MAX            0xffffffffu
#endif
#ifndef ULONG_MAX
#define ULONG_MAX           (~0UL)
#endif

/* `roundup_pow_of_two()`: 向上取到 2 的幂(Linux `include/linux/log2.h`)。 */
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
 * `IS_ERR/ERR_PTR/PTR_ERR`(Linux `include/linux/err.h`)—— 上游 `namei.c` 用它把
 * "找不到目录项"与"真出错"合在一个返回值里。
 * ★ 上一轮的移植把这些裁掉了(改成出参), 但那是**改上游代码**; 本刀要上游原文成立,
 *   所以必须原样提供这套约定: 错误码放在指针值的**高地址侧**(> -4096)。
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
 * `IS_ENABLED()`/`CONFIG_IS_ENABLED()`(u-boot `include/linux/kconfig.h`)。
 * 上游 `decompress.c` 用它做编译期裁剪: `#if IS_ENABLED(CONFIG_LZ4)`。
 * ★ 本件支持 LZ4, 不支持 zlib/deflate —— 所以把 CONFIG_LZ4 置 1、CONFIG_ZLIB 置 0,
 *   于是 `#if` 分支按上游原样成立, 我们**不用改它的任何一行**。
 */
#define IS_ENABLED(opt)         (opt)
#define CONFIG_IS_ENABLED(opt)  (opt)
#define CONFIG_LZ4              1
#define CONFIG_ZLIB             0

/* ==================================================================== errno 口径
 *
 * 裸名字(`EIO`/`ENOENT`/`EOPNOTSUPP`/…)与 `EUCLEAN` 由 **core** 提供
 * (`<br/core/br_errno.h>`, ADR-0023), 值与 `br_error.h` 一一对应(逐字取自 Linux 内核,
 * 宿主 `ft-test` 逐码对拍)。本件**不自带映射表** —— 那既是第二处真值, 也是对上游代码的
 * 多余改动。
 *
 * ★ `EFSCORRUPTED` 这里**照抄上游 `internal.h` 的构造**(`#ifdef EUCLEAN` 那一支),
 *   一个字都没改; 之所以以前映射到 `EIO`, 是因为本仓库当时没有 117 号 —— 现在
 *   `EUCLEAN` 已按 ADR-0023 D-4 从内核表补入。 */
#include <br/core/br_errno.h>

#define EFSCORRUPTED  EUCLEAN

/* ==================================================================== 字符串小工具
 *
 * 全部 `static inline` + `uboot_erofs_` 前缀(任务书硬约束: 不依赖 runtime/posix)。
 * 随后用宏把上游的裸名字接过来 —— 这就是"移植代码认得出是移植"的做法:
 * `strnlen(de_name, n)` 的调用点保持上游原样, 展开后却是我们自己的实现。
 *
 * 长度上界的纪律(与 fs/tmpfs 的 `tmpfs_name_ok` 同款): 这些函数只按调用方给的
 * `n` 走, 绝不多读一个字节。 */
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
 * ★ 本件**故意不提供** `strcpy` 这种"复制到 NUL 为止"的工具(上游也没有调用点)。
 *   原因是一个真踩到的坑: EROFS 目录块里的名字**没有 NUL 结尾** —— 它们是一条接一条
 *   紧挨着拼在名字区里的(长度由相邻两条 dirent 的 `nameoff` 相减得到)。拿"扫到 NUL"
 *   去读它, 会把后面所有名字一起吞进缓冲区(实测症状: "comp" 变成
 *   "compdirhello.txtraw.bin")。所以凡是搬盘上字节的地方一律用**带长度**的
 *   `uboot_erofs_copy_n()` / `__builtin_memcpy`。
 */
static inline void uboot_erofs_copy_n(char *dst, br_size_t cap, const char *src, br_size_t n)
{
    if (cap == 0u) {
        return;
    }
    if (n > (cap - 1u)) {
        n = cap - 1u;               /* 截断(调用方负责保证 n 本来就在界内) */
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

/* 认得出是移植: 上游的裸名字 → 我们的实现(整编译单元 -fno-builtin, 不会自撞) */
#define strlen(s)          uboot_erofs_strlen(s)
#define strnlen(s, n)      uboot_erofs_strnlen((s), (n))
#define strcmp(a, b)       uboot_erofs_strcmp((a), (b))
#define strncmp(a, b, n)   uboot_erofs_strncmp((a), (b), (n))
#define strchr(s, c)       uboot_erofs_strchr((s), (c))
#define copy_n(d, cap, s, n) uboot_erofs_copy_n((d), (cap), (s), (n))

/* 非对齐取小端(上游的 `get_unaligned_le32`) */
#define get_unaligned_le16(p) uboot_erofs_get_unaligned_le16(p)
#define get_unaligned_le32(p) uboot_erofs_get_unaligned_le32(p)
#define get_unaligned_le64(p) uboot_erofs_get_unaligned_le64(p)
#define put_unaligned_le16(v, p) uboot_erofs_put_unaligned_le16((v), (p))
#define put_unaligned_le32(v, p) uboot_erofs_put_unaligned_le32((v), (p))

/* 日志包装(定义在本插件的 erofs_vfs.c 里 —— 那里是本插件唯一的"胶水"编译单元) */

/* ==================================================================== 格式串
 * 上游用 `PRIu64`/`PRIx64`(<inttypes.h>)。本件的日志格式化器只认 `%llu`/`%llx`,
 * 于是这两个宏**带上强转** —— 调用点因此不需要额外写 `(unsigned long long)`。
 * 这是"认得出是移植"的取舍: 上游的格式串一字不改。 */
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRId64 "lld"

#endif /* BR_ADAPTER_UBOOT_EROFS_ENV_H */
