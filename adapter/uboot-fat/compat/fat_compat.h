/*
 * adapter/uboot-fat — 私有兼容层(u-boot/Linux 式类型与工具宏 + 字符串小工具)
 *
 * ============================== 本文件是什么 ==============================
 * 上游 FAT(`fs/fat/fat.c` + `fs/fat/fat_write.c`)是**u-boot 风格**的 C: 它假设有
 * `<fat.h>`(FAT 盘上结构 + 私有 fsdata)、`<part.h>` 的 `struct blk_desc`/`disk_partition`、
 * `<malloc.h>` 的 `malloc_cache_aligned`、`blk_dread/blk_dwrite`、libc 的
 * `strdup/strchr/strrchr/strcasecmp/strncasecmp/rand`、`printf`/`debug`、`assert`。
 * 本插件是**裸机 unikernel(无 libc)**, 所以这一层把那些假设**逐条落回本项目的真值**:
 *
 *   u-boot/Linux 侧                          →  brickOS 侧
 *   ---------------------------------------------------------------------------------
 *   __u8/__u16/__u32/u8.../loff_t            →  br_u8/… / br_s64(fat_compat.h 里 typedef)
 *   struct boot_sector/…/fsdata(include/fat.h) →  本文件**逐字段照抄**(数值逐字对齐)
 *   struct blk_desc + disk_partition         →  **极小的 shim**: 整设备一个分区
 *                                                (起始扇区 0, 扇区数 = geometry.n_sectors)
 *   blk_dread/blk_dwrite                     →  disk_read/disk_write → br_bdev_ops_t.read/write
 *   malloc/free/realloc                      →  br_malloc/br_free/br_realloc
 *   malloc_cache_aligned (DMA 对齐分配)       →  br_malloc(堆对齐 ≥ 16 B, > ARCH_DMA_MINALIGN)
 *   memcpy/memset/memcmp/memmove             →  core 提供(项目纪律: 这四个直接用)
 *   strlen/strcmp/strncmp/strcpy/strchr/…    →  本文件的 `uboot_fat_*`(见下)
 *   printf/debug/log_err                     →  br_log_error/debug(带 "uboot-fat: " 前缀)
 *   assert()                                 →  不用: 有意偏离, 越界/不变量失败一律返回
 *                                                -EIO/BR_ERR(BR_EIO)(见 NOTICE 的改写要点)
 *   blk_get_devnum_by_uclass_id / DM / env    →  全部裁掉(设备由 br_bdev_get 给)
 *
 * ## 为什么字符串要自己写
 *   硬约束(任务书): `strlen/strcmp/strncmp/strcpy/strchr/strrchr/tolower` **必须自己写小的**,
 *   不许依赖 runtime/posix。这里全部是 `static inline` + `uboot_fat_` 前缀。
 *   ★ 与隔壁 uboot-erofs 的一个**有意差异**: 那边用宏把裸名字接过来(`#define strlen …`),
 *   这边**不接宏** —— 移植点上直接写 `uboot_fat_strlen(...)`。理由: 宏会污染整个编译单元
 *   (包括本项目自己的头), 而 `-Wshadow`/审查都看不见"这里其实调的是自研实现"; 显式前缀
 *   让"哪些是自研的"一眼可查。代价是移植代码的名字与上游不同, 已登记在 NOTICE 的改写要点。
 *
 * ## 本文件**不**放什么
 *   FAT 盘上结构之外的类型与宏不在这里(它们属"插件内部契约", 见 fat_internal.h);
 *   块设备 shim 的**声明**在 fat_internal.h, 实现在 fat_core.c —— 因为 shim 要用到
 *   卷(`uboot_fat_vol_t`)这个内部概念。
 * ==========================================================================
 */
#ifndef BR_THIRD_PARTY_UBOOT_FAT_COMPAT_H
#define BR_THIRD_PARTY_UBOOT_FAT_COMPAT_H

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
/* 块设备与 VFS 契约: 上游从 `part.h`/`blk.h` 拿, 本件从这两件拿。**只有本头**
 * (适配层入口)可以 include `<br/...>` —— `src/` 下的上游派生件一律经本头取环境。 */
#include <br/dev/br_bdev.h>
#include <br/vfs/br_vfs.h>

/* ==================================================================== 小端前提 */
/* 上游的 `FAT2CPU16/32`、`le16_to_cpu()` 在本件里是**恒等**函数 —— 只有当目标真的小端时
 * 才成立。aarch64 的 QEMU virt 与绝大多数同类板子都是小端; 大端目标下这份移植是错的,
 * 所以宁可在这里停住也不要静默给出错的字。 */
#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "uboot-fat 的 FAT2CPU16/32 是恒等实现: 本移植只支持小端目标"
#endif

/* ==================================================================== 基础类型 */
typedef br_u8   u8;
typedef br_s8   s8;
typedef br_u16  u16;
typedef br_s16  s16;
typedef br_u32  u32;
typedef br_s32  s32;
typedef br_u64  u64;
typedef br_s64  s64;
typedef br_bool bool;

/* u-boot 的 `loff_t` = s64(文件偏移/长度); 上游 get_contents/set_contents 全靠它。
 * `size_t` 不在这里 typedef: 上游只用它做 `sizeof` 的伴侣, 直接用 br_size_t 即可。 */
typedef br_s64 loff_t;

/* on-disk 小端整数(与 u-boot 的 `__leN` 同型; 本件不做稀疏标注)。 */
typedef br_u8  __u8;
typedef br_u16 __u16;
typedef br_u32 __u32;
typedef br_u64 __u64;
typedef br_u16 __le16;
typedef br_u32 __le32;

#define true  BR_TRUE
#define false BR_FALSE

_Static_assert(sizeof(br_u64) == 8u, "u64 必须是 64 位");
_Static_assert(sizeof(loff_t) == 8u, "loff_t 必须是 64 位(上游 aarch64 口径)");

/* ==================================================================== 无 libc 的四块搬运
 * 硬约束: `memcpy/memset/memcmp/memmove` **由 core 提供**(core/src/string.c, 链进镜像),
 * 本件直接声明并使用它们 —— 不 include <string.h>(那是 libc 的头)。
 * 声明而不是宏: 这四个是真正的跨模块符号, 让编译器看见原型, 免得 -Wimplicit 通过。
 * `NULL` 同理: 无 <stddef.h>, 用本项目的口径(BR_NULL 是 (void *)0, 这里给指针用)。 */
void *memcpy (void *dst, const void *src, br_size_t n);
void *memmove(void *dst, const void *src, br_size_t n);
void *memset (void *dst, int c, br_size_t n);
int   memcmp (const void *a, const void *b, br_size_t n);

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ==================================================================== 字节序 */
/* aarch64 = 小端 ⇒ 转换是恒等。保留函数形态是为了让移植代码**看起来还是上游**(`FAT2CPU32`)。 */
static inline u16 uboot_fat_le16_to_cpu(u16 v) { return v; }
static inline u32 uboot_fat_le32_to_cpu(u32 v) { return v; }
static inline u16 uboot_fat_cpu_to_le16(u16 v) { return v; }
static inline u32 uboot_fat_cpu_to_le32(u32 v) { return v; }

_Static_assert(sizeof(u16) == 2u, "u16 必须是 16 位");
_Static_assert(sizeof(u32) == 4u, "u32 必须是 32 位");

/* 非对齐取小端: 目标开了 `-mstrict-align`, 所以一律走 __builtin_memcpy
 * (GCC 在 aarch64 上把它降成单条 `ldr`/`str`, 不会真的调 libc 的 memcpy)。
 * 上游用的是 u-boot 的 `get_unaligned_le16/32()`。 */
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

/* ==================================================================== u-boot 头文件的等价定义
 * 上游把下面这些放在 `<fat.h>`(盘上结构 + 私有参数)。本件搬到这里, **数值逐字对齐**。 */

/* ---- 名字与缓冲上界(上游 include/fat.h) ---- */
#define VFAT_MAXLEN_BYTES   256   /* LFN 缓冲字节上界; 另见本件的 BR_NAME_MAX 执法 */
#define VFAT_MAXSEQ         9
#define MAX_LFN_SLOT        20    /* 一条长名最多占多少个 12 槽(规范值) */
#define FATBUFBLOCKS        6     /* FAT 缓存粒度(扇区) */
#define SHORT_NAME_SIZE     11    /* name[8] + ext[3] */

/*
 * ★ 有意偏离(1/3): 上游的 `FATBUFSIZE` 依赖 `mydata->sect_size`, 于是
 * `FAT12BUFSIZE`/`FAT16BUFSIZE`/`FAT32BUFSIZE` 也依赖它 —— 那是宏里塞参数的写法,
 * 在 `-Wall -Wextra` 下很容易写出"看着是常量其实每次重算"的代码。本件把它们改成
 * **显式函数**(卷的扇区大小 = 512, 见 fat_internal.h 的 UB_FAT_SECTOR_SIZE)。
 * 数值与上游逐字一致: FATBUFBLOCKS=6 ⇒ 3072 B 缓存 ⇒ FAT32 768 项/块。
 */
static inline u32 uboot_fat_fat12_bufsize(u32 sect_size)
{
    return (sect_size * FATBUFBLOCKS * 2u) / 3u;
}
static inline u32 uboot_fat_fat16_bufsize(u32 sect_size)
{
    return (sect_size * FATBUFBLOCKS) / 2u;
}
static inline u32 uboot_fat_fat32_bufsize(u32 sect_size)
{
    return (sect_size * FATBUFBLOCKS) / 4u;
}

/* ---- 文件属性(上游 include/fat.h) ---- */
#define ATTR_RO      1
#define ATTR_HIDDEN  2
#define ATTR_SYS     4
#define ATTR_VOLUME  8
#define ATTR_DIR     16
#define ATTR_ARCH    32
#define ATTR_VFAT    (ATTR_RO | ATTR_HIDDEN | ATTR_SYS | ATTR_VOLUME)

#define DELETED_FLAG ((char)0xe5)   /* name[0] = 已删除 */
#define aRING        0x05           /* name[0] = 0x05(替代 0xe5 的转义) */
#define LAST_LONG_ENTRY_MASK 0x40   /* 长名槽序列的最后一条 */

#define ISDIRDELIM(c) ((c) == '/' || (c) == '\\')

/* ---- lcase 位(上游 dir_entry::lcase 的两个位) ---- */
#define CASE_LOWER_BASE 8
#define CASE_LOWER_EXT  16

/* ---- FAT 表判据(上游 include/fat.h; 数值逐字对齐) ---- */
#define MAX_FAT12 0xFF4u   /* 超过它就不再是 FAT12(见 determine_legacy_fat_bits) */

#define FAT2CPU16(x)  uboot_fat_le16_to_cpu(x)
#define FAT2CPU32(x)  uboot_fat_le32_to_cpu(x)

/* ==================================================================== 盘上结构(逐字段照抄上游 include/fat.h) */

typedef struct boot_sector {
    __u8  ignored[3];        /* 引导代码 */
    char  system_id[8];      /* 文件系统名 */
    __u8  sector_size[2];    /* 字节/扇区 */
    __u8  cluster_size;      /* 扇区/簇 */
    __u16 reserved;          /* 保留扇区数 */
    __u8  fats;              /* FAT 份数 */
    __u8  dir_entries[2];    /* 根目录项数 */
    __u8  sectors[2];        /* 扇区数(FAT12/16) */
    __u8  media;             /* 介质码 */
    __u16 fat_length;        /* 扇区/FAT(FAT12/16) */
    __u16 secs_track;        /* 扇区/磁道 */
    __u16 heads;             /* 磁头数 */
    __u32 hidden;            /* 隐藏扇区数 */
    __u32 total_sect;        /* 扇区数(sectors == 0 时) */

    /* 以下仅 FAT32 */
    __u32 fat32_length;      /* 扇区/FAT */
    __u16 flags;             /* bit8: FAT 镜像; 低 4 位: 活动 FAT */
    __u8  version[2];        /* 版本 */
    __u32 root_cluster;      /* 根目录首簇 */
    __u16 info_sector;       /* FSINFO 扇区 */
    __u16 backup_boot;       /* 备份引导扇区 */
    __u16 reserved2[6];      /* 未用 */
} boot_sector;

typedef struct volume_info {
    __u8 drive_number;       /* BIOS 驱动器号 */
    __u8 reserved;           /* 未用 */
    __u8 ext_boot_sign;      /* 0x29 = 下面的字段存在(DOS 3.3+) */
    __u8 volume_id[4];       /* 卷 ID */
    char volume_label[11];   /* 卷标 */
    char fs_type[8];         /* 通常是 FAT12/FAT16/FAT32 */
} volume_info;

struct nameext {
    char name[8];
    char ext[3];
};

typedef struct dir_entry {
    struct nameext nameext;  /* 8.3 名字 */
    __u8  attr;              /* 属性位 */
    __u8  lcase;             /* 大小写(CASE_LOWER_x) */
    __u8  ctime_ms;          /* 创建时间的毫秒 */
    __u16 ctime;             /* 创建时间 */
    __u16 cdate;             /* 创建日期 */
    __u16 adate;             /* 访问日期 */
    __u16 starthi;           /* FAT32 的高 16 位首簇 */
    __u16 time, date, start; /* 时间 / 日期 / 首簇低 16 位 */
    __u32 size;              /* 字节数 */
} dir_entry;

typedef struct dir_slot {
    __u8  id;                /* 槽序号 */
    __u8  name0_4[10];       /* 名字前 5 个字符 */
    __u8  attr;              /* 属性字节 */
    __u8  reserved;          /* 未用 */
    __u8  alias_checksum;    /* 8.3 别名校验和 */
    __u8  name5_10[12];      /* 再 6 个字符 */
    __u16 start;             /* 未用 */
    __u8  name11_12[4];      /* 最后 2 个字符 */
} dir_slot;

/* FAT 私有参数(上游 include/fat.h 的 `fsdata`)。★ `alloc` 与 `total_sect` 加了显式类型,
 * 免得 `-Wundef`/移植期类型不明; 其余逐字段一致。 */
typedef struct fsdata {
    __u8   *fatbuf;          /* FAT 缓存(堆分配, 对齐 ≥ 16 B) */
    int     fatsize;         /* FAT 位数(12/16/32) */
    __u32   fatlength;       /* FAT 长度(扇区) */
    __u16   fat_sect;        /* FAT 起始扇区 */
    __u8    fat_dirty;       /* fatbuf 被改过 ⇒ 需回写 */
    __u32   rootdir_sect;    /* 根目录起始扇区 */
    __u16   sect_size;       /* 扇区字节数 */
    __u16   clust_size;      /* 簇 = 多少扇区 */
    int     data_begin;      /* 首簇的扇区号(可为负, 见上游注释) */
    int     fatbufnum;       /* get_fatent 用; 初值 -1 */
    int     rootdir_size;    /* 非 FAT32 的根目录大小(扇区) */
    __u32   root_cluster;    /* FAT32 的根目录首簇 */
    u32     total_sect;      /* 卷内扇区总数 */
    int     fats;            /* FAT 份数 */
} fsdata;

/* 目录流(上游 `<fs.h>` 的 `struct fs_dir_stream`)。上游基类里只有 {desc, part},
 * 本件的具体实现(fat_dir, 见 fat_internal.h)不需要它们 —— 保留空结构是为了让
 * `struct fs_dir_stream *` 这个**签名形状**与上游一致(移植代码认得出是移植)。 */
struct fs_dir_stream {
    int unused;
};

/* 目录项(上游 `<fs.h>` 的 `struct fs_dirent`)。本件裁掉 rtc_time 与时戳字段:
 * 本插件的 VFS 面(br_dirent_t)只消费 {type, name, size}, 而 RTC 在 v1 还不存在
 * (上游那三个字段只在 `CONFIG_EFI_LOADER` 下才被填)。有意偏离, 见 NOTICE。 */
#define FS_DT_DIR 4
#define FS_DT_REG 8
#define FS_DT_LNK 10

struct fs_dirent {
    unsigned int type;       /* FS_DT_x(不是掩码) */
    loff_t       size;       /* 文件字节数 */
    u32          attr;       /* 属性位 */
    char         name[VFAT_MAXLEN_BYTES];
};

/* ==================================================================== u-boot 的工具宏 */
/* `START`: 目录项里的首簇(低 16 位 + FAT32 的高 16 位)。上游默认调用者作用域里有
 * `fsdata *mydata` —— 本件保持同一约定(逐字移植), 每个使用点都有 `mydata` 在作用域内。 */
#define START(dent) (FAT2CPU16((dent)->start) + \
                     ((mydata->fatsize != 32) ? 0 : \
                      (FAT2CPU16((dent)->starthi) << 16)))

#define IS_LAST_CLUST(x, fatsize) ((x) >= ((fatsize) != 32 ? \
                                    ((fatsize) != 16 ? 0xff8 : 0xfff8) : \
                                    0xffffff8))
#define CHECK_CLUST(x, fatsize) ((x) <= 1 || \
                                 (x) >= ((fatsize) != 32 ? \
                                         ((fatsize) != 16 ? 0xff0 : 0xfff0) : \
                                         0xffffff0))

/* ==================================================================== 通用小工具宏 */
#define DIV_ROUND_UP(n, d)   (((n) + (d) - 1u) / (d))
#define round_up(x, y)       ((((x) + ((y) - 1u)) / (y)) * (y))
#define round_down(x, y)     (((x) / (y)) * (y))

#define min(a, b)            (((a) < (b)) ? (a) : (b))
#define max(a, b)            (((a) > (b)) ? (a) : (b))
#define min_t(type, a, b)    ((type)min((type)(a), (type)(b)))
#define max_t(type, a, b)    ((type)max((type)(a), (type)(b)))

#define likely(x)   (x)
#define unlikely(x) (x)

static inline unsigned int uboot_fat_ilog2(unsigned int v)
{
    unsigned int n = 0u;

    while (v > 1u) {
        v >>= 1;
        n++;
    }
    return n;
}

/* `fls(i)`(上游 set_name 用它做"随机短名后缀"的位宽): 最高置位的位置, 1 起算。 */
static inline int uboot_fat_fls(unsigned int v)
{
    int n = 0;

    while (v != 0u) {
        v >>= 1;
        n++;
    }
    return n;
}

static inline int uboot_fat_is_power_of_2(unsigned int v)
{
    return (v != 0u) && ((v & (v - 1u)) == 0u) ? 1 : 0;
}

/* 无符号 64 位除法取商与余(u-boot 的 `div_u64_rem`)。内核版对常量除数有优化分支;
 * 这里只在 check_overflow 用一次(除数 = 卷扇区大小), 普通实现足够。 */
static inline u64 uboot_fat_div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
    *remainder = (u32)(dividend % (u64)divisor);
    return dividend / (u64)divisor;
}

/* ==================================================================== 日志 */
/* 上游: `debug(...)`(仅 `#define DEBUG` 时输出)、`log_err(...)`、`printf(...)`。
 * 本件: `debug` 恒开(它是本插件唯一的诊断面), `printf` 在**数据路径**上降级为
 * br_log_error + 返回错误码 —— "打印并继续"会让调用方拿到半个结果(见 NOTICE)。 */
#define debug(fmt, ...)     br_log_debug("uboot-fat: " fmt, ##__VA_ARGS__)
#define log_err(fmt, ...)   br_log_error("uboot-fat: " fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...) debug(fmt, ##__VA_ARGS__)

/* ==================================================================== errno 口径
 *
 * 上游直接用 Linux 的裸名字(它在 u-boot 里有 `<linux/errno.h>`)。本件**不自带映射表** ——
 * 那些名字由 **core** 提供(`<br/core/br_errno.h>`, ADR-0023), 与 `br_error.h` 的数值
 * 一一对应(值逐字取自 Linux 内核, 宿主 `ft-test` 逐码对拍)。
 *
 * ★ 为什么这里以前有一串 `#define EIO BR_EIO`: 那份名字表原先挂在 `runtime/posix` 上,
 *   而本插件不该依赖 POSIX 运行时(它是介质 FS, 不是 POSIX 面)⇒ 只好自己抄一份。
 *   那既是**第二处真值**, 也是对上游代码的**多余改动**。现在名字表归 core:
 *   上游的拼写原样成立, 本件只多一行 include。
 *
 * ★ 纪律(脚本门禁 `check-errno` 执法): **全树只许有一处 errno 表**(core 的那一处);
 *   三方插件若要一个新的码, 走 ADR-0023 D-4 —— 从内核表里补进 `br_error.h`/`br_errno.h`,
 *   不许在插件里自造编号或映射到别的码上。 */
#include <br/core/br_errno.h>

/* ==================================================================== 字符串小工具
 *
 * 全部 `static inline` + `uboot_fat_` 前缀(硬约束: 不依赖 runtime/posix)。
 * 长度纪律(与 fs/tmpfs 的 tmpfs_name_ok 同款): 一律只按调用方给的 `n` 走,
 * 绝不多读一个字节 —— 名字来自调用方, 未终止的串必须只读窗口内。
 */
static inline br_size_t uboot_fat_strlen(const char *s)
{
    br_size_t n = 0u;

    if (s == BR_NULL) {
        return 0u;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static inline br_size_t uboot_fat_strnlen(const char *s, br_size_t n)
{
    br_size_t i = 0u;

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

static inline int uboot_fat_strncmp(const char *a, const char *b, br_size_t n)
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

/* 有界复制(cap **含**结尾 '\0'; 与 fs/tmpfs 的 tmpfs_str_copy 同语义) */
static inline void uboot_fat_strcpy(char *dst, const char *src, br_size_t cap)
{
    br_size_t i = 0u;

    if (cap == 0u) {
        return;
    }
    while ((i + 1u) < cap) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return;
        }
        i++;
    }
    dst[cap - 1u] = '\0';
}

static inline char *uboot_fat_strchr(const char *s, int c)
{
    const char ch = (char)c;

    for (;; s++) {
        if (*s == ch) {
            return (char *)(br_uintptr_t)s;
        }
        if (*s == '\0') {
            return BR_NULL;
        }
    }
}

static inline char *uboot_fat_strrchr(const char *s, int c)
{
    const char *last = BR_NULL;
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

/* 大小写不敏感比较(上游 `strcasecmp`/`strncasecmp`; FAT 的名字比较全靠它) */
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

static inline int uboot_fat_strncasecmp(const char *a, const char *b, br_size_t n)
{
    br_size_t i;

    for (i = 0u; i < n; i++) {
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

/* 往有界缓冲里放一个字符(唯一的写入点: 越界一律丢弃, 只记长度)。 */
static inline void uboot_fat_putc(char *dst, br_size_t cap, br_size_t *n, char c)
{
    if ((*n + 1u) < cap) {
        dst[*n] = c;
    }
    (*n)++;
}

/*
 * 有界格式化(上游用 `sprintf` 拼 8.3 短名与 `~N` 后缀)。本件**不用** sprintf:
 * 无 libc, 而 core 的 br_log_write 只输出不回填缓冲。语义覆盖上游用到的子集:
 *   `%.*s`(精度截断) / `%s` / `%d` / `%u` / `%x` / `%%` / 字面量; 不认的转换符原样打出。
 * 返回应写入的字符数(不含结尾 '\0', 照 sprintf 口径); 超 `cap` 就截断
 * (调用点都会给足缓冲 —— 见 set_name 的 13 B buf 与 8+3+1 的短名)。
 */
static inline int uboot_fat_snprintf(char *dst, br_size_t cap, const char *fmt, ...)
{
    br_size_t n = 0u;
    br_size_t i = 0u;
    __builtin_va_list ap;

    if (cap == 0u) {
        return 0;
    }
    __builtin_va_start(ap, fmt);
    while (fmt[i] != '\0') {
        if (fmt[i] != '%') {
            uboot_fat_putc(dst, cap, &n, fmt[i]);
            i++;
            continue;
        }
        i++;                                  /* 跳过 '%' */

        int prec = -1;
        if (fmt[i] == '.') {                  /* `.*` 或 `.<数字>`(上游两处都在用) */
            i++;
            if (fmt[i] == '*') {
                prec = __builtin_va_arg(ap, int);
                i++;
            } else {
                prec = 0;
                while ((fmt[i] >= '0') && (fmt[i] <= '9')) {
                    prec = (prec * 10) + (int)(fmt[i] - '0');
                    i++;
                }
            }
        }

        if (fmt[i] == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            br_size_t k = 0u;

            i++;
            if (s == BR_NULL) {
                s = "(null)";
            }
            while ((s[k] != '\0') && ((prec < 0) || (k < (br_size_t)prec))) {
                uboot_fat_putc(dst, cap, &n, s[k]);
                k++;
            }
        } else if ((fmt[i] == 'd') || (fmt[i] == 'u') || (fmt[i] == 'x')) {
            const int as_signed = (fmt[i] == 'd');
            const int as_hex    = (fmt[i] == 'x');
            int v = __builtin_va_arg(ap, int);
            char tmp[12];
            int  tn = 0;
            unsigned int uv;

            i++;
            if (as_signed && (v < 0)) {
                uboot_fat_putc(dst, cap, &n, '-');
                uv = (unsigned int)(-v);
            } else {
                uv = (unsigned int)v;
            }
            do {                              /* 逆序生成, 再正序吐出 */
                const unsigned int base = as_hex ? 16u : 10u;
                const unsigned int d    = uv % base;

                tmp[tn] = (char)((d < 10u) ? ('0' + d) : ('a' + (d - 10u)));
                tn++;
                uv /= base;
            } while ((uv != 0u) && (tn < (int)sizeof(tmp)));
            while (tn > 0) {
                tn--;
                uboot_fat_putc(dst, cap, &n, tmp[tn]);
            }
        } else if (fmt[i] == '%') {
            uboot_fat_putc(dst, cap, &n, '%');
            i++;
        } else {
            /* 不认: 原样打出 '%' 与转换符(与 core 的 br_log_write 同口径) */
            uboot_fat_putc(dst, cap, &n, '%');
            if (fmt[i] != '\0') {
                uboot_fat_putc(dst, cap, &n, fmt[i]);
                i++;
            }
        }
    }
    __builtin_va_end(ap);
    dst[(n < cap) ? n : (cap - 1u)] = '\0';
    return (int)n;
}

#endif /* BR_THIRD_PARTY_UBOOT_FAT_COMPAT_H */
