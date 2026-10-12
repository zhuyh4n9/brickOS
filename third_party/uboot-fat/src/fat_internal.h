/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * third_party/uboot-fat — 插件内部契约(**唯一**声明处; 不导出)
 *
 * ============================== 分层与文件分工 ==============================
 *   fat_core.c     移植上游 `fs/fat/fat.c`: 卷识别(引导扇区/BPB)、FAT 表读取、
 *                  目录迭代器(fat_itr)、路径→目录项解析、只读文件面、目录流;
 *                  **外加块层 shim**(disk_read/disk_write → br_bdev_ops_t)。
 *   fat_write.c    移植上游 `fs/fat/fat_write.c`: 8.3/LFN 名字生成、空闲目录槽寻找、
 *                  FAT 表写入与簇分配、文件写入(set_contents)、unlink/mkdir/rename;
 *                  **外加 uboot_fat_truncate()**(上游没有, 见该函数头)。
 *   fat_vfs.c      本项目契约对接: br_fs_ops/br_inode_ops/br_file_ops + 生命周期钩子。
 *   fat_selftest.c TC-FAT-001..008。
 * 三者之间只有本头里的原型 —— 上游把 fat_write.c 用 `#include "fat.c"` 接进同一个编译
 * 单元(于是 static 函数随便用);本件按本项目纪律拆成两个 TU, 所以**凡是跨文件的都提成
 * 非 static 并在这里声明**(函数体保持上游原样, 只去掉 `static`)。登记在 NOTICE。
 *
 * ============================== 所有权规则(与 fs/tmpfs 相反的一侧) ==============================
 * 设计 SD-3: 本插件的 inode 是**瞬态路径包装** —— 每次 lookup/create 用 br_malloc 造一个
 * `uboot_fat_ino_t`(里面有 {卷 id, 相对挂载点的路径, 类型, 大小}), `free_inode` 必须
 * br_free 掉它。**只有挂载根 inode 例外**: 它由 mount 用 br_malloc 造好、挂在 `uboot_fat_fs_t`
 * 里长期持有(vfs 永不把根交给 free_inode), 但仍然不许放进 BSS —— "堆分配 / 显式释放"
 * 这条线要一路走通, 免得出现"一半静态一半动态"的所有权二义。
 *
 * ============================== 卷与挂载 ==============================
 * 三个卷各挂一块独立块设备(由 third_party/uboot-mmc 在它的 start 相注册):
 *   emmc0 = 2 MiB  FAT12 → /fat12
 *   emmc1 = 16 MiB FAT16 → /fat16
 *   emmc2 = 64 MiB FAT32 → /fat32
 * 挂载在**本插件自己的 start 相**发生(块设备那时已注册; `[[dep]] kind = "init"` 保证
 * 相位顺序)。挂载失败 ⇒ 返回负 errno(首败即停机是刻意的)。
 * 卷 id 与挂载点的对应是**编译期常量表**(见 fat_vfs.c 的 s_mounts), 不是运行期探测 ——
 * 于是"哪个挂载点用哪块设备"只有一处真值。
 * ==========================================================================
 */
#ifndef BR_THIRD_PARTY_UBOOT_FAT_INTERNAL_H
#define BR_THIRD_PARTY_UBOOT_FAT_INTERNAL_H

/* ★ 不直接 include `<br/...>`: 运行环境(bdev 与 VFS 契约)由适配层入口兑现。 */
#include "fat_compat.h"

/* ==================================================================== 卷事实(编译期常量) */

#define UB_FAT_VOL_MAX   3u      /* 本插件挂几个卷(与 plugin.toml 的 [[mount]] 条数一致) */

/* 块设备名(dev-core 命名规则 `[a-z][a-z0-9]*`) */
#define UB_FAT_DEV12     "emmc0"
#define UB_FAT_DEV16     "emmc1"
#define UB_FAT_DEV32     "emmc2"

/*
 * 挂载点(绝对路径, 与 plugin.toml 的 [[mount]] 逐字一致)。
 *
 * WORKAROUND(br-wa-fs-001): 这两个常量与 `plugin.toml` 的 `[[mount]]`/设备名是**两处真值**
 * —— 工具侧既不消费 `[[mount]]`, 也不校验它与这里的常量一致(组合器还没有"挂载计划"这条
 * 生成链路)。改一处不报错, 只会静默挂错地方。ADR-0022 把这条欠债的面从 2 件扩到 4 件
 * (新增两件介质 FS), 性质未变; 退出条件见 `WORKAROUNDS.md`。
 */
#define UB_FAT_MNT12     "/fat12"
#define UB_FAT_MNT16     "/fat16"
#define UB_FAT_MNT32     "/fat32"

/* 卷 id(数组下标; 顺序 = 挂载顺序) */
#define UB_FAT_VOL12     0u
#define UB_FAT_VOL16     1u
#define UB_FAT_VOL32     2u

/* 卷的 FAT 类型(上电时由 BPB 判定, 是**运行期事实**; 这三个是自检的期望值) */
#define UB_FAT_TYPE12    12
#define UB_FAT_TYPE16    16
#define UB_FAT_TYPE32    32

/*
 * 卷的扇区大小。整个插件按"512 B 扇区"写死 —— 理由是**上游也是这么假设的**:
 * `boot_sector.sector_size` 必须等于块设备的 `blksz`, 而本插件的三块 emmc 恒 512;
 * 挂载期把 `sect_size != 512` 判成 -EINVAL(宁可拒挂也不做 4 KiB 扇区的算术)。
 * 单次 I/O 的扇区上界 = 块设备 `geometry.max_rw`(实测 64 扇区 = 32 KiB), shim 内部切块。
 */
#define UB_FAT_SECTOR_SIZE   512u

/* 名字上界(本仓库 BR_NAME_MAX = 32, br_dirent_t.name[32])。
 * 长名(LFN)字节数超过它就 ⇒ BR_ERR(BR_ENOSPC)(与 fs/tmpfs 同口径)。 */
#define UB_FAT_NAME_MAX      BR_NAME_MAX

/* 拼接后路径缓冲上界(挂载点 ≤ 8 + '/' + 名字 ≤ 31 + '\0' = 40; 留 256 给未来的深路径)。
 * 上游的 `strdup(filename)` 在本件里换成**栈上有界缓冲**(无 libc 的 malloc 语义 + 失败面)。 */
#define UB_FAT_PATH_MAX      256u

/* 单条 LFN 的最大字节数(上游 VFAT_MAXLEN_BYTES = 256; 取 255 + 结尾 NUL) */
#define UB_FAT_LFN_MAX       256u

/* 一个簇的字节上界(上游 `CONFIG_FS_FAT_MAX_CLUSTSIZE`, u-boot 默认 32768)。
 * 与 bdev 的 max_rw 一致 ⇒ 一个簇最多一次 64 扇区的传输。 */
#define UB_FAT_MAX_CLUSTSIZE 32768u

/* 长名槽缓冲的上界(上游 `MAX_LFN_SLOT * sizeof(dir_slot)` = 20 * 32) */
#define UB_FAT_LFN_SLOT_MAX  (MAX_LFN_SLOT * (int)sizeof(dir_slot))

/* ==================================================================== 宿主预置内容(自检与 APP 共用的**唯一**真值)
 *
 * 三卷里的文件由宿主在构建后门禁里造好(见 README 的"前置工件")。这些常量是本插件对
 * 那份工件形状的**契约**: 宿主造出来的形状与这里不一致 ⇒ TC-FAT-002/003 红 —— 这正是
 * 我们要的判据(而不是"自检自己造数据自己验")。
 */
#define UB_FAT_C12_HELLO       "fat12 hello\n"
#define UB_FAT_C16_HELLO       "fat16 hello\n"
#define UB_FAT_C32_HELLO       "fat32 hello\n"
#define UB_FAT_C32_LFN         "lfn\n"

/* `/dir/a.bin` 的字节数与其填充字符(见 README 的造镜像脚本契约) */
#define UB_FAT_C12_DIR_A_BYTES 16u
#define UB_FAT_C16_DIR_A_BYTES 256u
#define UB_FAT_C32_DIR_A_BYTES 4096u
#define UB_FAT_A_FILL12        'A'
#define UB_FAT_A_FILL16        'B'
#define UB_FAT_A_FILL32        'C'

/* 超长(> BR_NAME_MAX-1 = 31 字节)的长名文件名: 用来验"长名 ⇒ -ENOSPC"这条口径 */
#define UB_FAT_C32_LFN_NAME    "long name file.txt"

/* ==================================================================== 插件内部结构 */

/*
 * 卷(bdev + 几何 + FAT 类型)。**设备 I/O 的入口**: 上游的 disk_read/disk_write 经
 * `s_cur_vol` 找到它, 于是移植代码里"卷"这件事只在 shim 一处出现。
 */
typedef struct uboot_fat_vol {
    const char          *dev;          /* 块设备名(常量) */
    const char          *mnt;          /* 挂载点(常量) */
    const br_bdev_ops_t *ops;          /* bdev 类 API 的 ops 表 */
    void                *priv;         /* bdev 私有句柄 */
    br_bdev_geom_t       geom;         /* 注册时的几何(扇区大小/总数/max_rw) */
    br_bool              bound;        /* 卷是否已绑定(挂载成功) */
    fsdata               fs;           /* FAT 参数(read_bootsectandvi 填) */
    br_bool              fs_valid;     /* fs 是否有效 */
} uboot_fat_vol_t;

/* GET_INFO 式观测(自检用; 生产路径只经卷表内部访问) */
typedef struct uboot_fat_vol_info {
    const char *dev;
    const char *mnt;
    br_u32      sector_size;
    br_u64      n_sectors;
    br_u32      max_rw;
    br_u32      clust_size_sectors;
    br_u32      max_clust;             /* 数据区簇数(由 BPB 算出) */
    int         fatsize;               /* 12 / 16 / 32 */
    br_bool     mounted;
} uboot_fat_vol_info_t;

/*
 * **瞬态 inode**(设计 SD-3): 每次 lookup/create 造一个, free_inode 释放。
 * `head` 必须首字段(CA-2: vfs 只认 br_inode_t*, 拿到手就能当节点用)。
 * 路径 = **相对挂载点**的路径, 根目录是 ""(空串) —— 于是拼接 = `mnt + "/" + path`,
 * 不依赖"路径里有没有前导 '/'"这种容易写错的约定。
 */
typedef struct uboot_fat_ino {
    br_inode_t head;                 /* 公共头(type / iops / fops / fpriv) */
    br_u8      vol;                  /* 卷 id(UB_FAT_VOL*) */
    br_u8      type;                 /* BR_INODE_DIR / BR_INODE_FILE */
    br_u32     size;                 /* 文件字节数(目录 = 0); lookup 时快照 */
    char       path[UB_FAT_PATH_MAX]; /* 相对挂载点; 根 = "" */
} uboot_fat_ino_t;

/* 单卷挂载体(mount 的 fs_priv; 挂载表长期持有) */
typedef struct uboot_fat_fs {
    uboot_fat_vol_t *vol;
    br_inode_t      *root;           /* 根 inode(br_malloc; 挂载表持有, 永不 free_inode) */
} uboot_fat_fs_t;

/* ==================================================================== 目录迭代器(上游 fat_itr)
 *
 * 逐字段照抄上游 `struct fat_itr`(fat_write.c 里定义), 只改两处:
 *   ① `block[MAX_CLUSTSIZE]` → `UB_FAT_MAX_CLUSTSIZE`(同一个值, 换成本件的名字);
 *   ② 去掉 `__aligned(ARCH_DMA_MINALIGN)`: 本件的缓冲一律来自 br_malloc(对齐 ≥ 16 B),
 *      而 `fat_itr` 自己也是堆分配的 ⇒ 在这里写对齐宏只会挡住栈上放迭代器(上游就这么干过)。
 * 迭代器**很大**(~33 KiB): 一律 br_malloc, 绝不放栈上(APP 线程栈 4 KiB 量级)。
 */
typedef struct fat_itr {
    fsdata       *fsdata;            /* FAT 参数 */
    unsigned int  start_clust;       /* 起始簇 */
    unsigned int  clust;             /* 当前簇 */
    unsigned int  next_clust;        /* remaining == 0 时的下一簇 */
    int           last_cluster;      /* 已到目录最后一簇 */
    int           is_root;           /* 是否在根目录 */
    int           remaining;         /* 当前簇里剩余的目录项数 */
    dir_entry    *dent;              /* 当前目录项 */
    int           dent_rem;          /* 长名起点之后的剩余项 */
    unsigned int  dent_clust;        /* 长名起点所在簇 */
    dir_entry    *dent_start;        /* 长名的首个目录项 */
    char          l_name[UB_FAT_LFN_MAX];   /* 长名 */
    char          s_name[14];               /* 8.3 短名 */
    char         *name;                     /* 有长名用长名, 否则短名 */
    u8            block[UB_FAT_MAX_CLUSTSIZE];  /* 当前簇的缓冲 */
} fat_itr;

/* 目录流(上游 `fat_dir`; 即 `<fs.h>` 的 fs_dir_stream 的具体类型)。
 * 调用方(fat_vfs.c)只知道 `struct fs_dir_stream *`, 本文件是**唯一**解释它的地方。
 * ★ 偏离: 上游的 `fsdata` 是**值**(目录流自带一份 + 自己的 FAT 缓存); 本件改成**指针**,
 *   指向**卷**上那一份 —— 于是目录流不持有缓存, 关闭时不必归还(fat_closedir 也不再 free
 *   fatbuf)。这是"fsdata 归卷"的必然结果, 见 fat_core.c 的 uboot_fat_get_fs_info。 */
typedef struct fat_dir {
    struct fs_dir_stream parent;     /* 上游的基类(本件不用它的字段, 保持层次形状) */
    struct fs_dirent     dirent;     /* fat_readdir 填这一条并交回指针 */
    fsdata              *fsdata;     /* 借用**卷**上的 FAT 参数(不拥有) */
    fat_itr              itr;
    /* vfs 面的迭代游标: 0 = ".", 1 = "..", ≥2 = 第 (cursor-2) 条真目录项。
     * 它是**会话状态**(归 br_file_ops 的 open/close), 所以放在目录流里而不是节点上 ——
     * 同一个目录可以同时被多次打开迭代(vfs 的 inode 是瞬态的, 节点没有"会话"这件事)。 */
    br_u32               cursor;
} fat_dir;

/* ==================================================================== 错误码口径
 * 本插件内部沿用**上游的混用形态**: 只读路径 `-1` + 负 errno, 写路径一律负 errno。
 * 判据统一写成 `rc < 0`(两族都覆盖) ⇒ 不会有"-1 被当成 -EPERM"的坑。 */
#define UB_FAT_BAD  (-1)

/* FAT 缓存字节数(上游的 FATBUFSIZE = 6 扇区) */
static inline u32 uboot_fat_fat_bufsize(u32 sect_size)
{
    return sect_size * FATBUFBLOCKS;
}

/* 卷内扇区号 → 设备 lba 的换算。本件"整设备一个分区"(起始扇区 0)⇒ **恒等**。
 * 保留函数形态是为了让移植代码里"分区偏移"这件事仍然显式可见(上游是
 * `cur_part_info.start + block`)。 */
static inline br_u64 uboot_fat_lba_of(__u32 block)
{
    return (br_u64)block;
}
#define lba_of(block) uboot_fat_lba_of(block)

/* 簇号 → 卷内扇区号(上游 include/fat.h 的 clust_to_sect; 数值逐字一致)。
 * `data_begin` 可为负(非 FAT32 的根目录"负簇"约定), 所以算术在 long 上做。
 * ★ 形参名从上游的 `fsdata` 改成 `fs`: 上游的 `fsdata` 是**类型名**, 用它当形参名会触发
 *   `-Wshadow`(而本件开了 -Werror)。 */
static inline u32 clust_to_sect(fsdata *fs, u32 clust)
{
    return (u32)((long)fs->data_begin + ((long)clust * (long)fs->clust_size));
}

/* ==================================================================== 块层 shim(fat_core.c)
 *
 * u-boot 里 `disk_read/disk_write` 是全设备函数(经 `struct blk_desc *cur_dev` +
 * 分区起始扇区)。本件把 `struct blk_desc`/`disk_partition`/`part.h` 换成**极小的 shim**:
 * "整设备一个分区"(起始扇区 0, 扇区数 = geometry.n_sectors), 于是上游的
 * `cur_part_info.start + block` 恒等于 `block` —— 偏移算术无需改动。
 *
 * 返回值与上游一致: **成功 (>= 0) = 传输的扇区数**, 失败 < 0(负 errno)。
 * 成功时**保证整段传输完成**: 内部按 `geometry.max_rw` 切块(单次 I/O 绝不超过它),
 * 任一块失败即返回负 errno —— 于是上游"`ret != nr_blocks` 就报错"的判据仍然成立。
 */
int disk_read(__u32 block, __u32 nr_blocks, void *buf);
int disk_write(__u32 block, __u32 nr_blocks, void *buf);

/* 卷绑定/解绑/查询(fat_core.c; mount 与 shim 之间唯一的接口) */
int  uboot_fat_vol_bind(uboot_fat_vol_t *vol);
void uboot_fat_vol_unbind(void);
uboot_fat_vol_t *uboot_fat_vol_current(void);
uboot_fat_vol_t *uboot_fat_vol_get(br_u32 id);
int  uboot_fat_mount_to_vol(const char *path, br_u32 *id);
int  uboot_fat_vol_info(br_u32 id, uboot_fat_vol_info_t *out);
/* 当前绑定卷的扇区大小/簇字节数/总扇区/数据区簇数上界; 0 = 没有绑定卷 */
u32  uboot_fat_sect_size(void);
u32  uboot_fat_clust_bytes(void);
u32  uboot_fat_max_clust(void);
u32  uboot_fat_vol_total_sect(void);
/* 当前卷的 FAT 参数(挂载后长期有效; 未挂载 ⇒ BR_NULL)。上游每个 API 自带一份 fsdata,
 * 本件把它提到卷上 —— 理由见 fat_core.c 的 uboot_fat_get_fs_info 函数头。 */
fsdata *uboot_fat_fsdata(void);
/* 交还 fsdata 的 FAT 缓存并把指针清成 BR_NULL(幂等; 重识别与解绑都走它) */
void uboot_fat_fatbuf_release(fsdata *mydata);

/* 卷识别 + 目录迭代(fat_core.c; 写路径要用, 所以不是 static) */
int   uboot_fat_get_fs_info(fsdata *mydata);
int   fat_itr_root(fat_itr *itr);
void  fat_itr_child(fat_itr *itr, fat_itr *parent);
void *fat_next_cluster(fat_itr *itr, unsigned int *nbytes);
dir_entry *next_dent(fat_itr *itr);
dir_entry *extract_vfat_name(fat_itr *itr);
int   fat_itr_next(fat_itr *itr);
int   fat_itr_isdir(fat_itr *itr);
int   fat_itr_resolve(fat_itr *itr, const char *path, unsigned int type);
__u32 get_fatent(fsdata *mydata, __u32 entry);
int   get_cluster(fsdata *mydata, __u32 clustnum, __u8 *buffer, unsigned long size);
int   get_contents(fsdata *mydata, dir_entry *dentptr, loff_t pos, __u8 *buffer,
                   loff_t maxsize, loff_t *gotsize);
__u8  mkcksum(struct nameext *nameext);
void  downcase(char *str, br_size_t len);
void  get_name(dir_entry *dirent, char *s_name);

/* 目录项解析的类型掩码(上游 fat.c 的私有宏; 提升到本头因为两个 TU 都在用) */
#define TYPE_FILE 0x1u
#define TYPE_DIR  0x2u
#define TYPE_ANY  (TYPE_FILE | TYPE_DIR)

/* ==================================================================== 上游 API(fat_core.c)
 * 名字与上游逐字一致(`fat_exists`/`fat_size`/…), 路径都是**卷内绝对路径**(不带挂载点)。 */
int  fat_exists(const char *filename);
int  fat_size(const char *filename, loff_t *size);
int  fat_read_file(const char *filename, void *buf, loff_t offset, loff_t len,
                   loff_t *actread);
int  file_fat_read(const char *filename, void *buffer, int maxsize);
int  fat_opendir(const char *filename, struct fs_dir_stream **dirsp);
int  fat_readdir(struct fs_dir_stream *dirs, struct fs_dirent **dentp);
void fat_closedir(struct fs_dir_stream *dirs);
void fat_close(void);

/* ==================================================================== 上游 API(fat_write.c) */
int file_fat_write_at(const char *filename, loff_t pos, void *buffer, loff_t size,
                      loff_t *actwrite);
int file_fat_write(const char *filename, void *buffer, loff_t offset,
                   loff_t maxsize, loff_t *actwrite);
int fat_unlink(const char *filename);
int fat_mkdir(const char *dirname);
int fat_rename(const char *old_path, const char *new_path);

/* 本件新增(上游没有): 截断/扩展到 size。见 fat_write.c 的函数头。 */
int uboot_fat_truncate(const char *path, br_u64 size);

/* fat_write.c 内部原语(fat_core.c 的 get_fatent 回写缓存时要用; 也是 truncate 的拼装件) */
int    flush_dirty_fat_buffer(fsdata *mydata);
int    set_fatent_value(fsdata *mydata, __u32 entry, __u32 entry_value);
__u32  determine_fatent(fsdata *mydata, __u32 entry);
int    clear_fatent(fsdata *mydata, __u32 entry);
int    set_sectors(fsdata *mydata, u32 startsect, u8 *buffer, u32 size);
int    set_cluster(fsdata *mydata, u32 clustnum, u8 *buffer, u32 size);
int    flush_dir(fat_itr *itr);
int    fat_move_to_cluster(fat_itr *itr, unsigned int cluster);
dir_entry *find_directory_entry(fat_itr *itr, const char *filename);
int    split_filename(char *filename, char **dirname, char **basename);
void   set_start_cluster(const fsdata *mydata, dir_entry *dentptr, __u32 start_cluster);
int    check_overflow(fsdata *mydata, __u32 clustnum, loff_t size);
__u32  find_empty_cluster(fsdata *mydata);

/* ==================================================================== VFS 对接(fat_vfs.c)
 * 这些不是"对外 API"(本插件不导出任何 API), 而是 fat_selftest.c 需要的内部视图。 */

/* 挂载点 → 卷信息(fat_selftest.c 的 TC-FAT-001 用; 与 uboot_fat_vol_info 同型) */
int  uboot_fat_mount_vol_info(const char *mnt, uboot_fat_vol_info_t *out);
/* 拼接"父目录(相对路径) + 名字"→ 相对路径(唯一定义处: fat_vfs.c) */
int  uboot_fat_join_path(const char *dir_path, const char *name,
                         char *out, br_size_t cap);
/* 名字是否可作目录项(非空/无 '/'/不是 . 或 ../长度 ≤ UB_FAT_NAME_MAX-1) */
br_bool uboot_fat_name_ok(const char *name);
/* 按挂载点跑一次卷级 sync(fs_ops.sync 的内部视图; 自检 TC-FAT-008 用) */
int  uboot_fat_test_sync(const char *mnt);

#endif /* BR_THIRD_PARTY_UBOOT_FAT_INTERNAL_H */
