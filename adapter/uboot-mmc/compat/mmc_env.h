/*
 * adapter/uboot-mmc — **u-boot 运行环境**(适配层; ADR-0026 D-3)
 *
 * ============================== 本文件是什么 ==============================
 * 上游 `drivers/mmc/{mmc,mmc_write,mmc_legacy,sdhci,sdhci-adma}.c` 是 **u-boot 风格**的 C:
 * 它假设有 `linux/types.h` 的 `u8/u32/bool`、`malloc.h`、`log.h` 的 `log_err/pr_debug`、
 * `cpu_func.h` 的 `flush_dcache_range`、`udelay()`/`get_timer()`、`asm/io.h` 的
 * `readl/writel`…… 本插件是**裸机 unikernel(无 libc)**, 这些都不存在。
 *
 * 本文件就是那份环境在 brickOS 上的**唯一落点**: `compat/<u-boot 头名>` 的每一个薄壳都
 * 指到这里, 而**上游 .c 一个字都不用改**就能编过。这是"给上游它依赖的环境", 而不是
 * "改上游让它适应我们"(ADR-0026 D-3 的判据)。
 *
 *   u-boot 侧                          →  brickOS 侧
 *   ---------------------------------------------------------------------------
 *   u8/u16/u32/u64/bool                →  br_u8/… / `<stdbool.h>`
 *   malloc/calloc/free                 →  br_malloc/br_calloc/br_free
 *   memalign(对齐 + DMA 安全)           →  br_dma_alloc(BR_DMA_F_ALIGN4K, …)(glue 兑现)
 *   memcpy/memset/memcmp/memmove       →  core/src/string.c(项目纪律: 直接用)
 *   printf/sprintf/puts                →  本文件的极小格式化器 → br_log_write / 缓冲
 *   log_err/log_warning/log_info/…     →  br_log_error/warn/info/debug
 *   pr_err/pr_info/pr_debug            →  同上
 *   E* 裸名字(-EIO/-ETIMEDOUT/…)       →  **core 的 `<br/core/br_error.h>`**(ADR-0027:
 *                                          名字就是 POSIX 原名, 于是上游原样成立)
 *   ydelay/mdelay/get_timer            →  ★ **可被设备中断提前唤醒的睡眠等待**(见下)
 *   readl/writel/readw/…               →  直接 MMIO + 记录"刚被访问的窗口"(见下)
 *   flush_dcache_range/flush_cache     →  br_mm_cache_flush / br_mm_cache_invalidate
 *   dma_map_single/dma_unmap_single    →  恒等映射 + 按方向的 cache 维护
 *   dev_phys_to_bus(dev= NULL)         →  恒等(v1 恒等映射 ⇒ dma_addr == vaddr)
 *
 * ============ ★ 核心手法: 把上游的**轮询等待**变成**中断驱动的睡眠等待** ============
 * 上游 `sdhci.c` 的两条等待路径(任务书 §2.4):
 *
 *   - 命令完成: `start = get_timer(0); do { stat = sdhci_readl(host, SDHCI_INT_STATUS);
 *     …; if (get_timer(start) >= SDHCI_READ_STATUS_TIMEOUT) … } while ((stat & mask) != mask);`
 *   - 数据传输: `do { stat = sdhci_readl(host, SDHCI_INT_STATUS); …;
 *     if (timeout-- > 0) udelay(10); … } while (!(stat & SDHCI_INT_DATA_END));`
 *
 * 上游**只**通过 `get_timer()` / `udelay()` 让出 CPU。于是环境层把这两个实现成
 * "**让出 CPU 直到'下一 ms 或设备中断'**" —— 上游的轮询循环就自动变成了
 * "睡下 → 设备中断 → 被唤醒 → 再读一次 INT_STATUS"的**中断驱动**等待, 而
 * **一行上游代码都不用改**。这就是本件的核心手法。
 *
 * "设备中断"由 glue 兑现(`glue/emmc_bdev.c` 的 `uboot_mmc_env_wait()`):
 *   { ISR: 读 INT_STATUS + 把 SIGNAL_ENABLE 写 0 去断言电平线 + br_work_submit() 交 bh }
 *   { bh:  br_sem_give() }
 *   { 本文件的等待原语: **重新打开 SIGNAL_ENABLE** + 睡(所以每一轮都能被下一次中断唤醒) }
 * 这就是 ADR-0021 D-4 的形态在"上游原样编译"下的实现。
 *
 * ★ 环境层怎么知道"现在等的是哪块卡": 上游的等待循环没有设备参数。所以 MMIO 访问器会
 *   把**刚被访问的地址**告诉 glue(`uboot_mmc_env_reg_touched()`), 由 glue 用它反查
 *   窗口 → 实例。这样多卡并发等待也各自挂到自己的中断通道上(不是"全局单例")。
 *
 * ============================== 依赖方向 ==============================
 * `compat/` 是环境, `glue/` 是实现。⇒ 本文件**只声明** glue 必须兑现的三个钩子
 * (`uboot_mmc_env_wait` / `uboot_mmc_env_reg_touched` / `uboot_mmc_env_memalign`),
 * 不 include 任何 `glue/` 下的东西。反方向(glue → compat)是唯一允许的方向。
 *
 * ★ 本文件**不**声明任何上游数据类型(那是 `compat/mmc.h`/`sdhci.h` 转发 vendor 原件的事),
 *   只管"运行环境"。
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_MMC_ENV_H
#define BR_ADAPTER_UBOOT_MMC_ENV_H

#include <stddef.h>     /* NULL / size_t / offsetof —— 编译器自带头, 不是 libc */
#include <stdbool.h>    /* bool/true/false —— u-boot 的 linux/types.h 也这么做 */
#include <stdint.h>     /* uint8_t/uint32_t/uint64_t(blk.h 的 lbaint_t 用) */

#include <br/core/br_error.h>   /* errno 名字与数值(ADR-0027: 名字 = POSIX 原名) */
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_mm.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* 配置矩阵: 与 u-boot 真实构建里 `generated/autoconf.h` 同角色。
 * ★ 必须在任何用到 `CONFIG_IS_ENABLED()` 的上游头之前(见 compat/config.h 的说明)。 */
#include <linux/kconfig.h>

/* ==================================================================== 配置矩阵自检
 *
 * 矩阵的"陷阱"在 `compat/generated/autoconf.h` 里逐条写了。这里把两条**本件依赖的前提**
 * 做成编译期判据 —— 免得将来有人"顺手把 DM_MMC 定义成 0"或"打开 BLK"而没人发现:
 * `#ifdef` 认的是"定义没定义", 所以这两条**必须**是 `!defined`, 不是 `== 0`。 */
#ifdef CONFIG_DM_MMC
#error "CONFIG_DM_MMC 必须**不被定义**: 定义了它, 上游 sdhci.c 的 #ifdef 会走 DM/udevice 形态"
#endif
#ifdef CONFIG_BLK
#error "CONFIG_BLK 必须**不被定义**: 定义了它, 上游 blk_desc/mmc_bread 会走 uclass 形态"
#endif
#if !defined(CONFIG_MMC_SDHCI_ADMA) || !defined(CONFIG_MMC_SDHCI_ADMA_HELPERS)
#error "本件的 sources 含 sdhci-adma.c ⇒ ADMA 与 ADMA_HELPERS 必须在矩阵里启用"
#endif
#if !defined(CONFIG_MMC_WRITE)
#error "本件的 sources 含 mmc_write.c ⇒ CONFIG_MMC_WRITE 必须启用"
#endif

/* ==================================================================== 基础类型
 *
 * ★ `u64`/`s64` 是 `unsigned/signed long long` —— **不是** `uint64_t`。u-boot 的
 *   `asm-generic/int-ll64.h` 明写 `typedef unsigned long long __u64; typedef __u64 u64;`,
 *   而 LP64 上 `uint64_t` 是 `unsigned long`: 同宽**不同型**。上游满地的格式化/赋值
 *   都按 `long long` 写, 换了类型就会在 `-Wformat` 等处炸掉(erofs 那边实测踩到过同一坑)。 */
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

/* u-boot 的 `phys_addr_t`/`dma_addr_t`: aarch64 上都是 64 位。本件**不开**
 * `CONFIG_DMA_ADDR_T_64BIT`(那是 u-boot 给 32 位 DMA 控制器的开关), 直接把两者都
 * 定成 `unsigned long` —— 恒等映射下它们就是虚拟/物理地址本身。 */
typedef unsigned long phys_addr_t;
typedef unsigned long dma_addr_t;

/*
 * 字节序(`asm/byteorder.h` → `linux/byteorder/little_endian.h` 在 aarch64 上的展开)。
 *   - `__leN` 家族在**小端目标**上是恒等转换(u-boot 也是这么实现的);
 *   - `__beN` 家族是 `__swabN`(本目标小端 ⇒ 必须真翻);
 *   - 裸名 `be32_to_cpu`/`le32_to_cpu` 等是小写别名(u-boot `little_endian.h` 末尾的
 *     `#define be32_to_cpu __be32_to_cpu` 那一段)。
 * 上游用到的是 `__be32_to_cpu`(SCR)与 `be32_to_cpu`(SSR); 这里把两族给全, 免得下一个
 * 用到 `be16`/`le32` 的地方又得改环境层。
 *
 * ★ 本件与 `erofs_env.h` 一样在文件头用 `#error` 守住"小端前提"(大端目标下这份移植是错的)。
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

/* u-boot `linux/types.h` 的 `__uN`/`__sN` 与 `__leN`/`__beN` 家族(逐条同上游口径)。
 * 上游用到的是 `__be32`(`sd_get_capabilities` 的 SCR 缓冲)。稀疏标注(`__bitwise`)去掉 ——
 * 与 `erofs_env.h` 同款处置: aarch64 上那些标注只是静态检查, 不影响语义。 */
typedef br_u8  __u8;   typedef br_u16 __u16;  typedef br_u32 __u32;  typedef br_u64 __u64;
typedef br_s8  __s8;   typedef br_s16 __s16;  typedef br_s32 __s32;  typedef br_s64 __s64;
typedef br_u16 __le16; typedef br_u32 __le32; typedef br_u64 __le64;
typedef br_u16 __be16; typedef br_u32 __be32; typedef br_u64 __be64;

/* `loff_t`/`dev_t`/`umode_t`: u-boot 的 `linux/types.h` 有, `mmc.h` 间接用到。 */
typedef long long      loff_t;
typedef unsigned int   dev_t;
typedef unsigned short umode_t;

/* 编译期自检: 类型宽度错了一切都错 */
_Static_assert(sizeof(u8)  == 1u, "u8 必须是 8 位");
_Static_assert(sizeof(u16) == 2u, "u16 必须是 16 位");
_Static_assert(sizeof(u32) == 4u, "u32 必须是 32 位");
_Static_assert(sizeof(u64) == 8u, "u64 必须是 64 位");

/* ==================================================================== 小端前提
 * 上面 `__leN` 是恒等、`__beN` 是 __swabN —— 只有当**目标真的小端**时才成立。
 * aarch64 的 QEMU virt 与绝大多数同类板子都是小端; 大端目标下这份移植是错的,
 * 所以宁可在这里停住也不要静默给出错的字(与 `erofs_env.h` 同款守卫)。 */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-mmc 的 leN_to_cpu()/beN_to_cpu() 按小端目标实现: 本移植只支持小端"
#endif

/* ==================================================================== 属性/标注
 * u-boot 的 `include/linux/compiler*.h` 一族提供这些。本件不 vendor 那族(它要求
 * `__KERNEL__` + `COMPILER_HAS_GENERIC_BUILTIN_OVERFLOW` 等一堆构建期探测), 只给
 * 上游实际用到的几个 —— 每个都在上游有明确用处。 */
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

/* ==================================================================== 位运算/小工具 */
#define BITS_PER_LONG        64
#define BITS_PER_LONG_LONG   64
#define BITS_PER_BYTE        8

#define BIT(nr)              (1UL << (nr))
#define BIT_ULL(nr)          (1ULL << (nr))
#define BIT_MASK(nr)         (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)         ((nr) / BITS_PER_LONG)
#define GENMASK(h, l)        (((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l)    (((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))

/* u-boot `<linux/bitops.h>` 的 fls/ffs(generic 实现, 语义与上游一致) */
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

/* 与上游 `include/linux/kernel.h`(第 156-231 行)同款的形态: **每个实参只求值一次**,
 * 且带类型检查。★ 不能用朴素三元式: 上游有多处 `max(a, b())` 风格/带副作用的实参,
 * 朴素三元式会把其中一个求值两次(语义就不一样了)。 */
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

/* ★ `lower_32_bits`/`upper_32_bits` 逐字取自上游 `include/linux/kernel.h`(第 108/114 行)。
 *   **不是装饰**: ADMA2 描述符的 `addr_lo` 与 `SDHCI_ADMA_ADDRESS` 都靠 `lower_32_bits()`。
 *   (实测教训: 本层第一版把 `lower_32_bits` 误抄成 `upper_32_bits` 的展开式, 于是每个
 *    描述符的数据地址都成了 `addr >> 32 == 0` ⇒ QEMU 的 ADMA 引擎去取 0 号地址 ⇒
 *    `SDHCI_INT_ADMA_ERROR`(`st=0x02008001`, `adma_err=0x1`) ⇒ `mmc_init` 返回 `-ECOMM`。
 *    这一条是**适配层的 bug**, 不是上游的, 也不是"上游在 QEMU 的 emmc 模型上跑不起来"。) */
#define lower_32_bits(n)  ((u32)((n) & 0xffffffff))
#define upper_32_bits(n)  ((u32)(((n) >> 16) >> 16))
#define lower_16_bits(n)  ((u16)(n))

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* u-boot `<linux/sizes.h>` 的子集(上游 mmc.c 的 SD SSR 表用到这些)。
 * 这些是**平台/接口常量**, 不是可调参数; 值与上游逐个一致。 */
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
 * 64 位除法工具(u-boot `include/div64.h` + `include/linux/math64.h` 里 BITS_PER_LONG==64
 * 那一支的语义)。本件不 vendor 那两份头(它们的另一半是 32 位平台专用的 log2/位对齐技巧),
 * 只给上游用到的三件 —— 语义必须与内核那一套一致:
 *   - `do_div(n, base)`: **就地**改 `n` 为商, 返回余数(调用点靠这个副作用);
 *   - `div_u64_rem`: 不取负的 64/32 除法 + 出参余数;
 *   - `lldiv`: `do_div` 的"不改实参"包装(上游 `mmc.c` 用它算 LBA)。
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

/* ==================================================================== 日志/打印
 *
 * 上游用 `printf`/`puts`/`sprintf` 与 `log_err/log_warning/log_info/log_debug/debug`、
 * `pr_err/pr_info/pr_debug`。本件**没有 libc 的 printf**(core 的 `br_log_write` 只做
 * 行输出, `runtime/posix` 也不提供 printf 家族 —— ADR-0014 §4)。
 *
 * ⇒ 本文件自带一个**极小的格式化器**, 覆盖上游**实际用到**的转换(逐个调用点查过):
 *   `%d %i %u %x %X %c %s %p %%`, 标志 `#` 与 `0`, 宽度(如 `%06x`/`%08x`/`%03d`),
 *   长度修饰 `l`/`ll`/`z`。
 *   ★ 为什么**不带** `format(printf)` 属性: 上游把 `uint` 传给 `%d`(u-boot 自己的
 *     printf 也不做严格校验), 带上属性会立刻让 vendor 代码在 `-Werror` 下红 —— 而
 *     "为过告警改上游"正是被禁止的。属性只加在转发到 core 的那一层(那里的调用点是我们写的)。
 *
 * ★ 有意偏离: 上游的 `printf` 走 u-boot 的 console; 本件走 core 的日志(BR_LOG_INFO),
 *   于是"驱动说的话"与内核其它日志**同一条流**(门禁正是按行抓这些行的)。
 */
#define UBOOT_MMC_LOG_MAX 256u

/* 无符号整数 → 缓冲(小端数字表反向吐)。返回新的写指针。 */
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
 * 本插件的格式化器。`cap` 含结尾 NUL; 超出即静默截断(日志/设备串是诊断面, 不是判据面)。
 * 覆盖范围见文件头; 未实现的修饰语**原样吐出**(宁可日志难看也不静默吞掉)。
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

        char *const mark = p;              /* 本转换的起点(宽度补齐要用) */

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
        case '\0':                          /* 尾随 '%': 原样收尾 */
            fmt--;
            break;
        default:                            /* 未实现的修饰语: 原样吐出 */
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
 * ★ 为什么不直接用 `__builtin_vsnprintf`: GCC 的 `__builtin_vsnprintf` **不是自足的** ——
 *   展开后仍是对 libc `vsnprintf` 的调用(链接期 `undefined reference`)。本仓库没有 libc
 *   (`runtime/posix` 也没有 printf 家族)⇒ 只能自带一个(与 `erofs_env.h` 同款处置)。
 */
static inline int printf(const char *fmt, ...)
{
    char line[UBOOT_MMC_LOG_MAX];
    __builtin_va_list ap;

    __builtin_va_start(ap, fmt);
    (void)uboot_mmc_vformat(line, sizeof(line), fmt, ap);
    __builtin_va_end(ap);
    /* 上游的格式串自带 '\n'; br_log_write 也按行收尾 ⇒ 多一个换行无害(且门禁按行抓)。 */
    br_log_write(BR_LOG_INFO, "%s", line);
    return 0;
}

static inline int sprintf(char *dst, const char *fmt, ...)
{
    __builtin_va_list ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = uboot_mmc_vformat(dst, 256u, fmt, ap);   /* 上游的调用点缓冲都 ≥ 9 B; 256 是安全上界 */
    __builtin_va_end(ap);
    return n;
}

static inline int puts(const char *s)
{
    br_log_write(BR_LOG_INFO, "%s", (s != NULL) ? s : "");
    return 0;
}

/* ---- u-boot 的 log_* / pr_* / debug ----
 * 上游写的是 `log_err(fmt, ...)`, 而**实现由我们给** —— 这正是"只做适配"的样子。
 * 注意上游格式串自带 '\n'; 本层不再补(与 core 的按行输出一致)。
 *
 * ★★ 这里有一条**必需**的口径区分(不是风格问题, 少了它链接期会红): u-boot 把这两族做成
 *    **不同**的裁剪层 ——
 *
 *      ① `log_err/log_warning/…`(上游 `include/log.h`)**不做编译期裁剪**:
 *         u-boot 里它们展开成 `log(cat, level, …)`, 只在**运行期**按日志级别过滤。
 *         ⇒ 本层照办: 直接转 `br_log_write(BR_LOG_*, …)`(core 的级别过滤就是运行期那道)。
 *      ② `pr_err/pr_info/pr_debug/…`(上游 `include/linux/printk.h`)**按 `CONFIG_LOGLEVEL`
 *         做编译期裁剪**, 形状是"常量条件的三元式":
 *             #define pr_debug(fmt, ...) ({ CONFIG_LOGLEVEL > 7 ? log_debug(fmt, ##__VA_ARGS__) : 0; })
 *
 *    为什么 ② 的裁剪是**必需**的: 上游 `mmc.c:175` 用一个**同一个条件**把
 *    `mmc_mode_name()` 的定义也裁掉了 ——
 *         #if CONFIG_IS_ENABLED(MMC_VERBOSE) || defined(DEBUG) || CONFIG_VAL(LOGLEVEL) >= LOGL_DEBUG
 *         const char *mmc_mode_name(enum bus_mode mode) { … }
 *         #endif
 *    而它的**调用点全部在 `pr_debug(...)` 的实参里**(mmc.c 的 232/1748/1884/2228 行)。
 *    若 `pr_debug` 不按 `CONFIG_LOGLEVEL` 裁剪(比如像 `log_*` 那样无条件转出去), 那些实参
 *    就会被求值 ⇒ **定义被裁掉、引用还在** ⇒ 链接期 `undefined reference to mmc_mode_name`。
 *    实测正是如此(第一版环境层踩到, 见交付报告)。⇒ ② 必须与上游**逐条同阈值**。
 *
 *    阈值逐条照抄上游 `include/linux/printk.h`(不是自创): `>0/>1/>2/>3/>4/>5/>6/>7`。
 *    本件的 `CONFIG_LOGLEVEL = 4` ⇒ `pr_err` 保留, `pr_warn` 及以下被裁 —— 与 u-boot 的
 *    缺省口径一致; 而 `log_warning()`(例如 `sdhci.c` 的复位告警)**不受影响**(它属 ①)。
 */
#define UBOOT_MMC_LOG_GATED(cond, level, ...) \
    ({ (cond) ? br_log_write((level), __VA_ARGS__) : 0; })

/* ① 运行期过滤(不裁剪) */
#define UBOOT_MMC_LOGF(level, ...) br_log_write((level), __VA_ARGS__)
#define log_emerg(...)    UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_alert(...)    UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_crit(...)     UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_err(...)      UBOOT_MMC_LOGF(BR_LOG_ERROR, __VA_ARGS__)
#define log_warning(...)  UBOOT_MMC_LOGF(BR_LOG_WARN,  __VA_ARGS__)
#define log_notice(...)   UBOOT_MMC_LOGF(BR_LOG_INFO,  __VA_ARGS__)
#define log_info(...)     UBOOT_MMC_LOGF(BR_LOG_INFO,  __VA_ARGS__)
#define log_debug(...)    UBOOT_MMC_LOGF(BR_LOG_DEBUG, __VA_ARGS__)

/* ② 编译期裁剪(阈值同上游 printk.h; 见上面"必需的口径区分") */
#define pr_emerg(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 0, BR_LOG_ERROR, __VA_ARGS__)
#define pr_alert(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 1, BR_LOG_ERROR, __VA_ARGS__)
#define pr_crit(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 2, BR_LOG_ERROR, __VA_ARGS__)
#define pr_err(...)       UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 3, BR_LOG_ERROR, __VA_ARGS__)
#define pr_warn(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 4, BR_LOG_WARN,  __VA_ARGS__)
#define pr_notice(...)    UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 5, BR_LOG_INFO,  __VA_ARGS__)
#define pr_info(...)      UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 6, BR_LOG_INFO,  __VA_ARGS__)
#define pr_debug(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 7, BR_LOG_DEBUG, __VA_ARGS__)
#define pr_devel(...)     UBOOT_MMC_LOG_GATED(CONFIG_LOGLEVEL > 7, BR_LOG_DEBUG, __VA_ARGS__)

/* `debug()`: 上游是 `debug_cond(_DEBUG, …)` 而 `_DEBUG = (DEBUG ? 1 : 0)`, 本件不定义 DEBUG
 * ⇒ 恒 0。上游写成 `if (cond) printf(…)`, 靠 -O2 的 DCE 丢掉实参; 本件用常量三元式更稳
 * (常量折叠, 与优化级别无关)。 */
#define _DEBUG            0
#define debug(...)        UBOOT_MMC_LOG_GATED(_DEBUG != 0, BR_LOG_DEBUG, __VA_ARGS__)

/*
 * u-boot 的 log 等级常量(u-boot `include/log.h` 的 `enum log_level_t` 的**同值重述**)。
 *
 * ★ 为什么这里必须是**宏**而不是 enum: 上游 `mmc.c:175` 把 `LOGL_DEBUG` 用在 **`#if`** 里
 *
 *       #if CONFIG_IS_ENABLED(MMC_VERBOSE) || defined(DEBUG) || CONFIG_VAL(LOGLEVEL) >= LOGL_DEBUG
 *
 *   预处理期只认宏 ⇒ enum 名字在那里等于"未定义"(u-boot 自己的那一行其实也如此)。本件在
 *   `-Wundef -Werror` 下必须给出**宏**才能既编过又语义正确: `CONFIG_LOGLEVEL` 取 4
 *   (= WARNING)后该条件为假, "逐命令 trace 打印"不参与编译(与"不刷屏"的口径一致)。
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

/* ==================================================================== 内存
 *
 * 项目纪律: `memcpy/memset/memcmp/memmove` 由 core 提供, 直接用(不自己写、不依赖
 * runtime/posix)。core 的 `string_internal.h` 是私有的, 所以这里把原型写下来
 * (有原型才有类型检查 —— `-Wmissing-prototypes` 那半边纪律)。 */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* u-boot 的 `malloc.h`: 接到 core 堆。★ 用宏而不是"声明函数让 runtime/posix 兑现":
 * 后者会让本插件在**没选 runtime/posix** 的产品里链接失败, 而堆是 core 的能力。 */
#define malloc(n)        br_malloc((br_size_t)(n))
#define calloc(n, s)     br_calloc((br_size_t)(n), (br_size_t)(s))
#define realloc(p, n)    br_realloc((p), (br_size_t)(n))
#define free(p)          br_free(p)

/* u-boot 的 `memalign.h`: 上游 `sdhci_adma_init()` 用它要一张 DMA 安全、对齐的描述符表
 * (`memalign(ARCH_DMA_MINALIGN, ADMA_TABLE_SZ)`)。
 * ★ 为什么不能在环境层"接 br_malloc": ADMA2 描述符表**要给设备读**, 必须来自 DMA 池
 *   (非缓存/物理连续, 见 ADR-0003 的 region 表)。⇒ 由 glue 兑现, 这里只声明。 */
void *uboot_mmc_env_memalign(br_size_t align, br_size_t size);
#define memalign(align, size) uboot_mmc_env_memalign((align), (size))

/* ==================================================================== errno
 * 上游到处是裸的 `-EIO`/`-ETIMEDOUT`/`-ENODEV`/`-ENOSYS`/`-EINVAL`/`-EBUSY`/`-ECOMM`/
 * `-ENOMEM`/`-EOPNOTSUPP`/`-EMEDIUMTYPE`。ADR-0027 把 errno 的名字改成 POSIX 原名且与
 * 数值同处 `br_error.h` ⇒ 那些代码**一个字都不用改**(这正是改名的理由)。 */

/*
 * ---- 上游用到、而 core 的 `br_error.h` **尚未收录**的 errno 名字 ------------------
 *
 * ★ 这是一处**明确的欠债**, 也是本件唯一"新增 errno 名字"的地方:
 *   `core/include/br/core/br_error.h` 按 ADR-0027 只收录"本仓用到的"第二段(35 起)名字,
 *   而本件是**新的消费者**: 上游 `drivers/mmc/` 的原件里出现了下面五个(`-ECOMM` 在命令
 *   超时/错误返码上, `-EMEDIUMTYPE` 在硬件分区不支持时, `-ENOTSUPP` 在"本件未实现的模式"
 *   兜底上, 另两个在 SD 路径)。core 里没有它们 ⇒ "上游一个字不改"就只能在这里补名。
 *
 * ★ 数值**逐字取自 u-boot `include/linux/errno.h`**(它又取自 Linux 内核), 所以将来把这
 *   五条搬进 `br_error.h` 时**一个数值都不用改**; `tools/check-errno.sh` 的判据②
 *   ("除 br_error.h 外不许定义 errno 名")目前只对 br_error.h 里已有的 53 个名字执法,
 *   所以本文件现在**不**触红 —— 但真值该回到 core(见交付报告 §6 的欠债清单)。
 *   本文件**不**修改 core(任务书硬约束: core 目录不动)。
 */
#define ECOMM        70    /* Communication error on send */
#define EBADMSG      74    /* Not a data message */
#define ENOMEDIUM   123    /* No medium found */
#define EMEDIUMTYPE 124    /* Wrong medium type */
#define ENOTSUPP    524    /* Operation is not supported(内核内部码, 非 uapi) */

/* ==================================================================== 时间
 *
 * ★ 本件的核心手法(见文件头): `udelay()`/`get_timer()` 是"**可被设备中断提前唤醒的
 *   睡眠等待**", 于是上游的轮询循环自动变成中断驱动的等待。
 *
 * 两个钩子, **分别对应 u-boot 的两种等待**(由 glue 兑现):
 *   - `uboot_mmc_env_wait(max_us)`: "让出 CPU **至多 max_us, 或直到当前设备的设备中断**"
 *     —— 这是**等设备事件**的那条路(命令完成), 睡下之前重开本卡 `SIGNAL_ENABLE`
 *     (ISR 用"把 SIGNAL_ENABLE 写 0"去断言电平线), 于是正常情况下由 **设备中断**唤醒;
 *     在没有可等的设备通道(或调度器尚未就位)时退回一次短让出。
 *   - `uboot_mmc_env_spin(us)`: **忙等**(不让出 CPU)。上游拿 `udelay()` 当"等寄存器稳定/
 *     复位完成"的自旋(实测取值只有 10/100/1000/2000 µs, 见 §"为什么不用睡眠"), 那些循环
 *     本来就没有设备事件可等; 而调度器的睡眠粒度是**一拍**(1/HZ = 5 ms)⇒ 把它们接到睡眠
 *     路径会"睡 100 µs 实际 5 ms"(实测: 四块卡 bring-up 0.037 s → 0.35 s, 并把整段自检
 *     推过 `irq-test` 的 6 s 门禁上限, 见 ADR-0029 §4)。
 * `uboot_mmc_env_reg_touched()` 由 glue 兑现: 记录"刚被访问的 MMIO 地址", 供上面那句
 *   "当前设备"的反查(上游的等待循环没有设备参数)。
 */
void uboot_mmc_env_wait(br_u32 max_us);
void uboot_mmc_env_spin(br_u32 us);
void uboot_mmc_env_reg_touched(const volatile void *reg);

/*
 * 单次让出窗口的**上界**(= 任务书 §2.4 的"下一 ms"): 只有"等设备事件"那条路用满它。
 */
#define UBOOT_MMC_WAIT_MAX_US   1000u

/* 一个"让出 CPU"的窗口: 至多 max_us 微秒, 或设备中断。 */
static inline void uboot_mmc_yield_once(br_u32 max_us)
{
    uboot_mmc_env_wait(max_us);
}

/*
 * `udelay(us)` —— u-boot 的语义就是**忙等**; 上游只拿它等"寄存器稳定/复位完成"
 * (取值 10/100/1000/2000 µs), 所以这里照 u-boot 忙等: 不让出 CPU, 也就不会把
 * µs 级自旋放大成"一拍"(5 ms)。上界不承诺(上游也只保证"不早醒")。
 */
static inline void udelay(unsigned long usec)
{
    uboot_mmc_env_spin((br_u32)usec);
}

/*
 * `mdelay(ms)`: u-boot 拿它等**卡**就绪(`mmc_send_op_cond`/`mmc_complete_op_cond` 的
 * 轮询间隔, 取值 5/10 ms)。这属于"等设备事件"⇒ 走 `udelay` 的相反那条路: **让出 CPU**,
 * 每片 ≤1 ms, 且可被设备中断提前唤醒。
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
 * `get_timer(base)`: 返回"自 base 起的毫秒数"。★ 每次调用**先让出 CPU**(到下一 ms 或
 * 设备中断)—— 上游把它当"轮询的节拍器"用, 于是它的循环变成中断驱动。
 */
static inline ulong get_timer(ulong base)
{
    uboot_mmc_yield_once(UBOOT_MMC_WAIT_MAX_US);
    return (ulong)(br_clock_now() / (br_time_t)BR_US_PER_MS) - base;
}

/* u-boot 的微秒读数(`include/time.h`); `time_after()` 是无符号回绕安全比较。 */
static inline ulong timer_get_us(void)
{
    return (ulong)br_clock_now();
}
#define time_after(a, b) ((long)((b) - (a)) < 0)

/* ==================================================================== MMIO
 *
 * 上游 `sdhci.h` 的 `sdhci_readl/writel/…`(未开 `MMC_SDHCI_IO_ACCESSORS`)直接落到
 * `readl(host->ioaddr + reg)`。这里就是那份环境。
 *
 * ★ 每次访问都 `uboot_mmc_env_reg_touched(addr)`: 那是"环境层怎么知道现在等的是哪块卡"
 *   的全部秘密(见文件头)。它只是一次 store + 反查(在等待时), 不影响正确性。
 * ★ 对齐: PCIe BAR 在 Device-nGnRnE 背景里, 且本目标 `-mstrict-align` ⇒ 必须对齐访问。
 *   上游的 `sdhci_readl/writel` 用的就是对齐偏移, 所以这里按宽度直接解引用。
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
 * u-boot 的 `cpu_func.h`/`asm/cache.h`/`linux/dma-mapping.h`:
 *   - `ARCH_DMA_MINALIGN`: 与平台一致 —— Cortex-A53 的 L1 D-cache 行是 64 B(QEMU virt);
 *   - `flush_cache(addr, size)`(上游 `sdhci-adma.c` 在填完描述符表后调)/`flush_dcache_range`/
 *     `invalidate_dcache_range` → core 的 `br_mm_cache_*`(命中非缓存 region ⇒ 无操作);
 *   - `dma_map_single`/`dma_unmap_single`: u-boot 的语义就是"按方向做 cache 维护 + 返回
 *     恒等映射的 dma_addr"(见上游 `linux/dma-mapping.h`);
 *   - `dev_phys_to_bus(dev, addr)`: 本件 `!DM_MMC` ⇒ `mmc_to_dev()` 是 NULL; 恒等映射
 *     ⇒ 原样返回(`phys2bus.h` 的 `#define dev_phys_to_bus(_, _addr) _addr`)。
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

/* `linux/dma-direction.h`(逐条同上游) */
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

/* `phys2bus.h`(`!CONFIG_PHYS_TO_BUS` 与 `!DM` 两支都退化成恒等) */
#define dev_phys_to_bus(_dev, addr) ((dma_addr_t)(addr))
#define dev_bus_to_phys(_dev, addr) ((phys_addr_t)(addr))
static inline unsigned long phys_to_bus(unsigned long phys) { return phys; }
static inline unsigned long bus_to_phys(unsigned long bus)  { return bus; }

/* u-boot 的 `virt_to_phys()`: 本件恒等映射 ⇒ 恒等(`sdhci_setup_cfg` 算 ADMA 表地址用)。 */
#define virt_to_phys(addr) ((phys_addr_t)(br_uintptr_t)(addr))

/* ==================================================================== GPIO
 *
 * `struct sdhci_host` 里**按值**嵌了两个 `struct gpio_desc`(pwr_gpio/cd_gpio), 所以类型
 * 必须在场。本件不启用 DM_GPIO/DM_MMC ⇒ 一个都不会被初始化/使用。
 * 定义与上游 `asm-generic/gpio.h` 逐字段一致(三个字段)。
 */
struct udevice;                 /* 前置声明: 本件没有 DM */

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

/* ==================================================================== 其它前置声明
 * 上游 `mmc.h` 自己 `struct bd_info;`; `mmc_initialize()` 的签名要用它。 */
struct bd_info;

#endif /* BR_ADAPTER_UBOOT_MMC_ENV_H */
