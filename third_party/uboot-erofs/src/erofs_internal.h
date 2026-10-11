/*
 * third_party/uboot-erofs — 插件内部契约(不导出)
 *
 * ============================== 本文件是什么 ==============================
 * 本插件**不导出任何 API**(plugin.toml 没有 `[[export]]`, 也没有 `[build].includes`),
 * 所以这里不是"对外面", 而是**本插件内部的唯一声明处**:
 *   - 器件契约(设备名 / 挂载点 / 卷结构 / 全局卷状态);
 *   - 跨文件原型(块层 shim、super/namei/data/zmap/decompress/VFS 胶水);
 *   - 共享的静态块缓冲与映射缓冲(见下方"栈纪律");
 *   - 自检用到的可观测面(SD-3 的 inode 是瞬态的, 自检必须能拿到"挂载成功了吗"这类事实)。
 *
 * ============================== 分层(文件 ↔ 上游) ==============================
 *   erofs_vfs.c        VFS 对接 + 生命周期钩子       (上游 fs.c 的 blk 粘合 + 本项目的 ops)
 *   erofs_super.c      超级块                        (上游 super.c)
 *   erofs_namei.c      inode 解析 + 路径走查          (上游 namei.c)
 *   erofs_data.c       读路径 + 平面映射              (上游 data.c)
 *   erofs_zmap.c       压缩映射迭代                   (上游 zmap.c)
 *   erofs_decompress.c LZ4/INTERLACED/SHIFTED 解压    (上游 decompress.c + lz4.c)
 *   erofs_fs.h         on-disk 格式(on-disk 真值, 上游同名文件裁剪)
 *   erofs_compat.h     u-boot/Linux → brickOS 兼容层
 *   erofs_selftest.c   TC-EROFS-* 自检
 *
 * ============================== 一个卷 ==============================
 *   bdev `emmc3`(16 MiB 的 eMMC 卡, 镜像从扇区 0 开始)只读挂到 `/assets`。
 *   挂载发生在 **start** 相: 块设备由 `third_party/uboot-mmc` 在自己的 start 相注册,
 *   本插件在 `[[dep]]`(kind = init)里声明了对它的依赖 ⇒ 相位顺序由插件管理器保证。
 *
 * ============================== 栈纪律(重要) ==============================
 *   内核对 APP 线程的栈是 4 KiB 量级, 而 EROFS 的上游代码里有好几处
 *   `char buf[EROFS_MAX_BLOCK_SIZE]`(4096 B)与 `struct erofs_map_blocks`(内含 4096 B 的
 *   `mpage`)。**一个 4 KiB 的数组放栈上就已经把栈用光了**, 所以本件把这类缓冲全部
 *   提到**文件域静态**:
 *     - `s_scratch`(4 KiB): 所有"临时读一块"的场合(inode 读、映射头读、chunk index 读、
 *       目录块的 home —— 后者见 erofs_vfs.c 的 s_dir_blk);
 *     - `s_map`(erofs_data.c 的 `struct erofs_map_blocks`): 平面/压缩映射迭代的工作台。
 *   本插件**单线程调用**(挂载/走查/读全部跑在调用者线程上, 没有自己的线程), 所以
 *   静态缓冲不需要加锁 —— 这一条写在这里, 免得后来人以为它被保护了。
 *   换用静态的**代价**: 缓冲在两次调用之间保留内容, 于是"索引块缓存"不能再靠
 *   `map->index` 隐式判定(它可能在两次映射之间被别人覆盖)。本件的做法见
 *   `struct z_erofs_maprecorder.index`: **缓存标记挂在录制器上**, 只有映射自己写
 *   scratch 时才有效。
 * ==========================================================================
 */
#ifndef BR_THIRD_PARTY_UBOOT_EROFS_INTERNAL_H
#define BR_THIRD_PARTY_UBOOT_EROFS_INTERNAL_H

#include <br/dev/br_bdev.h>

#include "erofs_compat.h"
#include "erofs_fs.h"

/* ==================================================================== 器件契约(私有) */
#define BR_EROFS_DEV_NAME    "emmc3"      /* bdev 名(third_party/uboot-mmc 之外的 16 MiB 卡) */
/* WORKAROUND(br-wa-fs-001): 下面两个常量与 `plugin.toml` 的 `[[mount]]`/设备名是**两处
 * 真值** —— 工具侧既不消费 `[[mount]]`, 也不校验它与此处一致。退出条件见 `WORKAROUNDS.md`。 */
#define BR_EROFS_MOUNT_PATH  "/assets"    /* 挂载点(plugin.toml 的 [[mount]] 与这里两处真值) */
#define BR_EROFS_TYPE_NAME   "erofs"      /* 诊断用 */

/*
 * 瞬态 inode 里保存的"相对挂载点路径"的静态上界。
 * ★ 为什么不复用 `BR_VFS_PATH_MAX`: 那是 vfs-core 的**私有**常量(定义在 br_vfs.c 里,
 *   不在契约头里)⇒ 本插件看不到它, 也不该看到(越层)。取 256:
 *     - 远大于夹具里最深的路径(3 段 × ≤31 字节 ≈ 96);
 *     - 一个瞬态 inode 因此 = sizeof(br_inode_t) + sizeof(struct erofs_inode) + 256 + 对齐
 *       ≈ 480 B。走查一次最多同时存在 2 个(vfs 用完即弃), 对 core 堆毫无压力。
 * 装不下 ⇒ `lookup` 返回 BR_NULL(vfs 得到 -ENOENT), 与"名字超 BR_NAME_MAX"同口径
 * (见 README 的名字上界取舍)。
 */
#define UBOOT_EROFS_REL_PATH_MAX  256u

/* 本插件的**最大逻辑块**。EROFS 允许 blkszbits ∈ [9, 12], 但上游的压缩路径只支持
 * PAGE_SIZE(4096)的块(`erofs_read_inode_from_disk` 里那条显式检查), 本件照抄。 */
#define EROFS_MAX_BLOCK_SIZE  4096u
#define EROFS_BLOCK_SIZE_BITS 12u

/* ==================================================================== 夹具常量
 *
 * ★ 与宿主 `prototype/tools/mk-fs-images.sh` **逐字节对齐**。自检(erofs_selftest.c)只
 *   用这些常量做**生成式**判据(第 i 字节 = 某个式子), 不内嵌大块字面量:
 *
 *   路径               大小    内容(生成式)                          mkfs 实测 datalayout
 *   --------------------------------------------------------------------------------
 *   /hello.txt          12     "erofs hello\n"                      2 = FLAT_INLINE
 *   /comp/lz4.txt    70000     "ABCDEFGH" 重复(第 i 字节 = "ABCDEFGH"[i%8])  3 = 压缩(LZ4)
 *   /raw.bin          8192     ((i*1103515245+12345)>>16)&0xFF     3 = 压缩(LZ4, 压不动)
 *   /dir/a.bin        4096     第 i 字节 = i & 0xFF                 0 = FLAT_PLAIN
 *   /dir/sub/deep.txt    5     "deep\n"                            2 = FLAT_INLINE
 *
 * 70000 不是 4096 的整数倍 ⇒ 刻意覆盖"压缩件的尾 extent"(partial decoding)那条分支;
 * raw.bin 是"压不太动的数据"(压缩比 50%)⇒ 覆盖 LZ4 的另一条路径(几乎全是字面量)。
 * ★ 自检**不**断言目录条目总数(夹具加文件不该让用例变红), 只按名字断言。
 */
#define EROFS_FX_HELLO_PATH      "hello.txt"
#define EROFS_FX_HELLO_SIZE      12u
#define EROFS_FX_HELLO_TEXT      "erofs hello\n"
#define EROFS_FX_HELLO_LAYOUT    EROFS_INODE_FLAT_INLINE

#define EROFS_FX_LZ4_PATH        "comp/lz4.txt"
#define EROFS_FX_LZ4_SIZE        70000u
#define EROFS_FX_LZ4_PERIOD      8u
#define EROFS_FX_LZ4_PATTERN     "ABCDEFGH"      /* 第 i 字节 = PATTERN[i % 8] */
#define EROFS_FX_LZ4_LAYOUT      EROFS_INODE_COMPRESSED_COMPACT

#define EROFS_FX_RAW_PATH        "raw.bin"
#define EROFS_FX_RAW_SIZE        8192u
#define EROFS_FX_RAW_LAYOUT      EROFS_INODE_COMPRESSED_COMPACT

#define EROFS_FX_ABIN_PATH       "dir/a.bin"
#define EROFS_FX_ABIN_SIZE       4096u
#define EROFS_FX_ABIN_LAYOUT     EROFS_INODE_FLAT_PLAIN

#define EROFS_FX_DEEP_PATH       "dir/sub/deep.txt"
#define EROFS_FX_DEEP_SIZE       5u
#define EROFS_FX_DEEP_TEXT       "deep\n"

/* TC-EROFS-003 的"中段读"窗口(故意不从 0 开始: 覆盖非零 offset 的 extent 切分) */
#define EROFS_FX_ABIN_MID_OFF    1000u
#define EROFS_FX_ABIN_MID_LEN    500u

/* 目录块(EROFS 的目录数据是"每块一组 dirent + 名字区")按块解析; 名字上界 255。 */
#define EROFS_DIRENT_MAX_NAME EROFS_NAME_LEN

/* ==================================================================== 卷结构
 *
 * 相对上游 `internal.h` 的 `struct erofs_sb_info`, 本件**裁掉**了:
 *   - `struct erofs_device_info *devs` + `extra_devices` 的多设备表(本插件一个卷,
 *     多设备 ⇒ 挂载期 -ENOTSUP, 见 erofs_super.c);
 *   - `volume_name`(没有任何消费者: u-boot 的 erofs 只回答 uuid, 本件连 uuid 都不读);
 *   - `xattr_prefix_*`(长 xattr 前缀, 本件不解析 xattr)。
 * 保留 `devt_slotoff`/`device_id_mask` 所在的 union? —— 不保留: 多设备被拒之后它没有读者。
 */
struct erofs_sb_info {
    u64 total_blocks;
    u64 primarydevice_blocks;

    erofs_blk_t meta_blkaddr;
    erofs_blk_t xattr_blkaddr;

    u32 feature_compat;
    u32 feature_incompat;
    u64 build_time;
    u32 build_time_nsec;

    unsigned char islotbits;
    unsigned char blkszbits;

    /* what we really care is nid, rather than ino.. */
    erofs_nid_t root_nid;
    /* used for statfs, f_files - f_favail */
    u64 inos;

    u8 uuid[16];

    u16 available_compr_algs;
    u16 lz4_max_distance;

    u32 checksum;
    erofs_nid_t packed_nid;
};

/* 全局卷状态(唯一实例名带本插件的私有前缀, 避免污染镜像的符号空间)。
 * 上游把它写成 `struct erofs_sb_info sbi;`(fs.c 顶部) —— 本件只换名字, 结构不变。 */
extern struct erofs_sb_info uboot_erofs_sbi;

/* 与上游 `internal.h` 逐字同义的几何取用宏(名字加深, 因为它们是**函数式**的) */
#define erofs_blksiz()      (1u << uboot_erofs_sbi.blkszbits)
#define erofs_blknr(addr)   ((addr) >> uboot_erofs_sbi.blkszbits)
#define erofs_blkoff(addr)  ((addr) & (erofs_blksiz() - 1u))
#define erofs_pos(nr)       ((erofs_off_t)(nr) << uboot_erofs_sbi.blkszbits)

#define BLK_ROUND_UP(addr)  DIV_ROUND_UP(addr, 1u << uboot_erofs_sbi.blkszbits)

static inline erofs_off_t uboot_erofs_iloc(erofs_nid_t nid)
{
    return erofs_pos(uboot_erofs_sbi.meta_blkaddr) + (nid << uboot_erofs_sbi.islotbits);
}

/* 认得出是移植: 上游的 `iloc(vi->nid)` 一字不改, 只是展开成本件的实现 */
#define iloc(nid)  uboot_erofs_iloc(nid)
/* 上游 `EROFS_FEATURE_FUNCS(...)` 生成的 `erofs_sb_has_*()` —— 逐字保留(它们都是
 * "读一个位"的小函数, 由 super.c 在挂载期填好 feature_*)。 */
#define EROFS_FEATURE_FUNCS(name, compat, feature)   \
static inline bool erofs_sb_has_##name(void)         \
{                                                    \
    return (uboot_erofs_sbi.feature_##compat & EROFS_FEATURE_##feature) ? true : false; \
}

EROFS_FEATURE_FUNCS(lz4_0padding, incompat, INCOMPAT_ZERO_PADDING)
EROFS_FEATURE_FUNCS(compr_cfgs, incompat, INCOMPAT_COMPR_CFGS)
EROFS_FEATURE_FUNCS(big_pcluster, incompat, INCOMPAT_BIG_PCLUSTER)
EROFS_FEATURE_FUNCS(chunked_file, incompat, INCOMPAT_CHUNKED_FILE)
EROFS_FEATURE_FUNCS(device_table, incompat, INCOMPAT_DEVICE_TABLE)
EROFS_FEATURE_FUNCS(ztailpacking, incompat, INCOMPAT_ZTAILPACKING)
EROFS_FEATURE_FUNCS(fragments, incompat, INCOMPAT_FRAGMENTS)
EROFS_FEATURE_FUNCS(dedupe, incompat, INCOMPAT_DEDUPE)
EROFS_FEATURE_FUNCS(xattr_prefixes, incompat, INCOMPAT_XATTR_PREFIXES)
EROFS_FEATURE_FUNCS(sb_chksum, compat, COMPAT_SB_CHKSUM)

/* 压缩 inode 的初始化标记(上游 `EROFS_I_*`)。本件只用到 Z_INITED:
 * `erofs_inode.flags` 就是这两个位, 没有别的读者(erofsfuse 的运行期标记被裁掉)。 */
#define EROFS_I_Z_INITED  (1u << 1)

/* ==================================================================== inode
 *
 * 相对上游 `struct erofs_inode`, 本件**裁掉**了:
 *   - `i_hash` / `i_subdirs` / `i_xattrs`(`struct list_head`: u-boot 侧一个也没用);
 *   - `i_count` / `i_parent` / `i_ino[2]` / `i_srcpath`(erofsfuse/mkfs 专用);
 *   - `bh` / `bh_inline` / `bh_data` / `idata` / `eof_tailraw*`(mkfs 的写路径);
 *   - `compressmeta` / `chunkindexes` 指针(读路径不用; 只用匿名结构里的压缩头字段);
 *   - `capabilities` / `fragment_size`(没有消费者)。
 * 保留 `fragmentoff` 与 `z_tailextent_headlcn`: zmap.c 的 fragment/ztailpacking 分支要它们
 * (本件**保留**这两条分支的代码 —— 见 erofs_zmap.c 的说明)。
 * `i_uid`/`i_gid`/`i_mtime`/`i_nlink` 保留: getattr 的口径要与盘上一致(哪怕 v1 只报
 * type/size/nlink)。
 */
struct erofs_inode {
    union {
        /* (erofsfuse) runtime flags */
        unsigned int flags;
        /* (mkfs.erofs) device ID containing source file */
        u32 dev;
    };

    umode_t i_mode;
    erofs_off_t i_size;

    u32 i_uid;
    u32 i_gid;
    u64 i_mtime;
    u32 i_mtime_nsec;
    u32 i_nlink;

    union {
        u32 i_blkaddr;
        u32 i_blocks;
        u32 i_rdev;
        struct {
            unsigned short chunkformat;
            unsigned char  chunkbits;
        };
    } u;

    unsigned char datalayout;
    unsigned char inode_isize;
    /* inline tail-end packing size */
    unsigned short idata_size;
    bool compressed_idata;
    bool lazy_tailblock;

    unsigned int xattr_isize;
    unsigned int extent_isize;

    erofs_nid_t nid;

    union {
        void *compressmeta;
        void *chunkindexes;
        struct {
            uint16_t z_advise;
            uint8_t  z_algorithmtype[2];
            uint8_t  z_logical_clusterbits;
            uint8_t  z_physical_clusterblks;
            uint64_t z_tailextent_headlcn;
            unsigned int z_idataoff;
#define z_idata_size idata_size
        };
    };
    erofs_off_t fragmentoff;
};

static inline bool is_inode_layout_compression(struct erofs_inode *inode)
{
    return erofs_inode_is_data_compressed(inode->datalayout);
}

static inline unsigned int erofs_bitrange(unsigned int value, unsigned int bit,
                                          unsigned int bits)
{
    return (value >> bit) & ((1u << bits) - 1u);
}

static inline unsigned int erofs_inode_version(unsigned int value)
{
    return erofs_bitrange(value, EROFS_I_VERSION_BIT, EROFS_I_VERSION_BITS);
}

static inline unsigned int erofs_inode_datalayout(unsigned int value)
{
    return erofs_bitrange(value, EROFS_I_DATALAYOUT_BIT, EROFS_I_DATALAYOUT_BITS);
}

/*
 * 由 mode 判 EROFS 的 dirent 文件类型(`struct erofs_dirent.file_type` 的口径)。
 * ★ 为什么需要它: 目录面上的 readdir 直接拿 `de->file_type`(省一次 inode 读), 而
 *   `lookup` 拿的是 inode 的 `i_mode`; 两者必须**同源同判**, 否则同一个名字在
 *   `readdir` 里是"目录"、在 `lookup` 里却成了"文件"(自检 TC-EROFS-005 会当场红)。
 *   方案: `i_mode & S_IFMT` 是唯一真值来源; 未知类型收成 `EROFS_FT_UNKNOWN`
 *   (目录面按"跳过并计数"处理, 见 erofs_vfs.c)。
 * 放在头里当 `static inline`: 它是**两个**编译单元的公共小工具, 且没有状态。
 */
static inline u8 uboot_erofs_ftype_from_mode(umode_t mode)
{
    switch ((unsigned int)(mode & (umode_t)S_IFMT)) {
    case S_IFREG:  return (u8)EROFS_FT_REG_FILE;
    case S_IFDIR:  return (u8)EROFS_FT_DIR;
    case S_IFLNK:  return (u8)EROFS_FT_SYMLINK;
    case S_IFCHR:  return (u8)EROFS_FT_CHRDEV;
    case S_IFBLK:  return (u8)EROFS_FT_BLKDEV;
    case S_IFIFO:  return (u8)EROFS_FT_FIFO;
    case S_IFSOCK: return (u8)EROFS_FT_SOCK;
    default:       return (u8)EROFS_FT_UNKNOWN;
    }
}

/* ==================================================================== 映射
 *
 * `BH_*` / `EROFS_MAP_*` 是 data.c 与 zmap.c 之间的共同词汇, 逐字保留(上游 internal.h)。
 */
enum {
    BH_Meta,
    BH_Mapped,
    BH_Encoded,
    BH_FullMapped,
    BH_Fragment,
    BH_Partialref,
};

/* Has a disk mapping */
#define EROFS_MAP_MAPPED      (1u << BH_Mapped)
/* Located in metadata (could be copied from bd_inode) */
#define EROFS_MAP_META        (1u << BH_Meta)
/* The extent is encoded */
#define EROFS_MAP_ENCODED     (1u << BH_Encoded)
/* The length of extent is full */
#define EROFS_MAP_FULL_MAPPED (1u << BH_FullMapped)
/* Located in the special packed inode */
#define EROFS_MAP_FRAGMENT    (1u << BH_Fragment)
/* The extent refers to partial decompressed data */
#define EROFS_MAP_PARTIAL_REF (1u << BH_Partialref)

struct erofs_map_blocks {
    /* ★ 偏离: 上游这是内嵌的 4 KiB 缓冲, 且**索引块缓存就住在里面**
     *   (`z_erofs_reload_indexes()` 把块读进它, 再用 `map->index` 标记)。
     *   本件把块读的承载者换成共享静态 scratch(见本头的"共享静态缓冲"与
     *   erofs_zmap.c 的文件头"偏离 1"), 于是:
     *     - 这个字段**没有任何读者**(留着是为了保持结构体形状与上游一致);
     *     - 索引块缓存的判据改挂在 `struct z_erofs_maprecorder.index` 上。
     *   本件的实例是 erofs_data.c 的文件域静态 s_map —— 它一个字节都不占栈。 */
    char mpage[EROFS_MAX_BLOCK_SIZE];

    erofs_off_t m_pa, m_la;
    u64 m_plen, m_llen;

    unsigned short m_deviceid;
    char m_algorithmformat;
    unsigned int m_flags;
    /* 上游的"缓存的是哪一块索引"; 本件不再用它做判据(见上), 只保留初值语义。 */
    erofs_blk_t index;
};

/*
 * Used to get the exact decompressed length, e.g. fiemap (consider lookback
 * approach instead if possible since it's more metadata lightweight.)
 */
#define EROFS_GET_BLOCKS_FIEMAP   0x0002
/* Used to map tail extent for tailpacking inline or fragment pcluster */
#define EROFS_GET_BLOCKS_FINDTAIL 0x0008

/* 上游 internal.h 的"运行时算法号": 0..Z_EROFS_COMPRESSION_MAX-1 是**盘上**算法号,
 * 这两个是映射迭代"算出来"的算法号(SHIFTED = 按字节平移的未压缩数据; INTERLACED =
 * 首尾环绕切分的未压缩数据)。erofs_decompress() 只认这三个 + LZ4。 */
enum {
    Z_EROFS_COMPRESSION_SHIFTED = Z_EROFS_COMPRESSION_MAX,
    Z_EROFS_COMPRESSION_INTERLACED,
    Z_EROFS_COMPRESSION_RUNTIME_MAX
};

struct erofs_map_dev {
    erofs_off_t m_pa;
    unsigned int m_deviceid;
};

/* ==================================================================== 解压请求 */
/* 上游 decompress.h 的 `struct z_erofs_decompress_req` —— 逐字保留(它是 data.c 与
 * decompress.c 之间的唯一接口)。 */
struct z_erofs_decompress_req {
    char *in, *out;

    /*
     * initial decompressed bytes that need to be skipped
     * when finally copying to output buffer
     */
    unsigned int decodedskip;
    unsigned int inputsize, decodedlength;

    /* cut point of interlaced uncompressed data */
    unsigned int interlaced_offset;

    /* indicate the algorithm will be used for decompression */
    unsigned int alg;
    bool partial_decoding;
};

/* ==================================================================== 块层 shim
 *
 * 上游的这两个函数在 fs.c 里(它认识 u-boot 的 `struct blk_desc`/`disk_partition`)。
 * 本件把它们改写成"在一个 `br_bdev_ops_t` 上按**字节偏移**寻址" —— EROFS 内部全部按
 * 字节寻址, 而 bdev 按扇区: `sector = off / sector_size`, `sector_off = off % sector_size`。
 *
 * 单次 I/O 不超过 `geometry.max_rw`(本平台 = 64 扇区 = 32 KiB): 实现**逐扇区**下发
 * (每扇区一次 `read`, 天然满足上界), 大读由上层按 4 KiB 块切(见 erofs_data.c)。
 */
int uboot_erofs_bdev_bind(void);                                      /* 挂载期: 取 emmc3 并记住几何 */
int uboot_erofs_dev_read(int device_id, void *buf, u64 offset, size_t len);
int uboot_erofs_blk_read(void *buf, erofs_blk_t start, u32 nblocks);

/* 上游的 `erofs_dev_read` / `erofs_blk_read`: fs.c 里那两个名字的直接对应物。
 * 保留裸名字让移植代码看起来还是上游(它们是本插件的**私有**符号)。 */
#define erofs_dev_read(dev, buf, off, len) uboot_erofs_dev_read((dev), (buf), (off), (len))
#define erofs_blk_read(buf, start, nb)     uboot_erofs_blk_read((buf), (start), (nb))

/* 挂载期只读一次的事实(自检也读它; 见 erofs_internal.h 的"可观测面") */
typedef struct br_erofs_dev {
    const br_bdev_ops_t *ops;
    void                *priv;
    br_u64               n_sectors;
    br_u32               sector_size;
    br_u32               max_rw;        /* 扇区 */
    br_u32               align;
} br_erofs_dev_t;

/* 挂载期绑定好的块设备(自检经 `uboot_erofs_dev_read` 直接读盘上字节做判据 ——
 * 这样"magic 在偏移 1024"这件事的证据来自介质, 而不是来自我们自己的解析结果)。
 * 自检**只读**它; 写它的唯一入口是 `uboot_erofs_bdev_bind()`(挂载期)。 */
extern br_erofs_dev_t uboot_erofs_dev;

/* ==================================================================== 共享静态缓冲
 *
 * 一个 4 KiB 的"临时读一块"缓冲。`erofs_namei` 之类把 inode/映射头读进来解析,
 * 都是"读进来 → 立刻解析完 → 再也不碰"的用法, 所以一份就够。
 * ★ 谁写它: erofs_namei.c(inode 结构体)、erofs_data.c(chunk index 块)、erofs_zmap.c
 *   (映射头)。谁**不能**在持有它的内容时调别人: 见 erofs_vfs.c 的目录读 —— 那里把
 *   目录块读进**自己**的静态缓冲(s_dir_blk), 不借共享 scratch, 于是"读目录块"与
 *   "读 inode"不会互相踩。
 */
u8 *uboot_erofs_scratch(void);

/* ==================================================================== 跨文件原型 */

/* super.c */
int erofs_read_superblock(void);

/* namei.c */
int erofs_read_inode_from_disk(struct erofs_inode *vi);
int erofs_ilookup(const char *path, struct erofs_inode *vi);

/* data.c */
int erofs_pread(struct erofs_inode *inode, char *buf, erofs_off_t count, erofs_off_t offset);
int erofs_map_blocks(struct erofs_inode *inode, struct erofs_map_blocks *map, int flags);
int erofs_map_dev(struct erofs_map_dev *map);
int erofs_read_one_data(struct erofs_map_blocks *map, char *buffer, u64 offset, size_t len);
int z_erofs_read_one_data(struct erofs_inode *inode, struct erofs_map_blocks *map,
                          char *raw, char *buffer, erofs_off_t skip, erofs_off_t length,
                          bool trimmed);

/* zmap.c */
int z_erofs_fill_inode(struct erofs_inode *vi);
int z_erofs_map_blocks_iter(struct erofs_inode *vi, struct erofs_map_blocks *map, int flags);

/* decompress.c */
int z_erofs_decompress(struct z_erofs_decompress_req *rq);

/* 上游 internal.h 的正向引用: `inode->z_idata_size` 就是 `idata_size`(见上面的 #define)。
 * `IS_ROOT()` 没有读者(它是 erofsfuse 的), 裁掉 —— 不留无用宏。 */

#endif /* BR_THIRD_PARTY_UBOOT_EROFS_INTERNAL_H */
