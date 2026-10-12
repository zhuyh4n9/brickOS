/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * adapter/uboot-fat — 插件内部契约(**唯一**声明处; 不导出)
 *
 * ============================== 分层与文件分工 ==============================
 * ADR-0026 D-2/D-3 之后本插件只有三类东西:
 *   `plugin.toml` 的 sources  上游 `third_party/uboot-imx/fs/fat/fat_write.c`
 *                             (**原件参与编译**: 它第 23 行 `#include "fat.c"`, 于是
 *                              一个编译单元同时含读+写; **不**再单独编 fat.c, 会重复符号)
 *   compat/                   u-boot 运行环境(头名适配壳 + `fat_env.h`);
 *   glue/                     本项目的对接:
 *       fat_blk.c    块层 shim(`blk_dread/blk_dwrite` → `br_bdev_ops_t`)+ 卷表 +
 *                    BPB 观测(挂载判据)+ 环境里那几个"我们实现的"东西
 *                    (`strdup`/`sprintf`/`rand`/truncate 的公开 API 拼装);
 *       fat_vfs.c    VFS 四层 ops + 生命周期钩子;
 *       fat_selftest.c  TC-FAT-001..008。
 * 本头是 glue 内部的契约(布局/常量/原型), **不导出**任何对外 API。
 *
 * ============================== 与上一版的差别(ADR-0026) ==============================
 *   · `src/` 那一层**没有了**: 上游原件直接参与编译, 插件里没有一份"改写过的上游";
 *   · 上一版的 `fsdata 归卷` 偏离**撤销**: 上游每个 API 自带一份 `fsdata`(用它自己的
 *     malloc/free 语义), 而 glue 只调**公开 API** ⇒ 不再有"跨调用保留 FAT 缓存"这件事,
 *     也就不需要"每个入口记得刷新"这条不变量。卷级 sync 只剩 `bdev.flush`(上游每个写入口
 *     在自己内部就把 FAT 窗口与目录项落盘了, 见 `file_fat_write_at` 的 flush 两连);
 *   · 上一版 160 行的 `uboot_fat_truncate()` 改写**删除**: 截断只用上游公开 API 拼
 *     (见 `uboot_fat_truncate` 的函数头: 变短/变 0 是上游 `file_fat_write_at` 自己的
 * 截断/扩展到 size(上游没有 truncate, 只在写路径里有一处 "truncate the rest" 分支)。
 *
 * ============================== 所有权规则 ==============================
 * 设计 SD-3: 本插件的 inode 是**瞬态路径包装** —— 每次 lookup/create 用 br_malloc 造一个
 * `uboot_fat_ino_t`(里面有 {卷 id, 相对挂载点的路径, 类型, 大小}), `free_inode` 必须
 * br_free 掉它。**只有挂载根 inode 例外**: 它由 mount 用 br_malloc 造好、挂在
 * `uboot_fat_fs_t` 里长期持有(vfs 永不把根交给 free_inode), 但仍然不许放进 BSS ——
 * "堆分配 / 显式释放"这条线要一路走通。
 *
 * ============================== 卷与挂载 ==============================
 * 三个卷各挂一块独立块设备(由 adapter/uboot-mmc 在它的 start 相注册):
 *   emmc0 = 2 MiB  FAT12 → /fat12
 *   emmc1 = 16 MiB FAT16 → /fat16
 *   emmc2 = 32 MiB FAT32 → /fat32
 * 挂载在**本插件自己的 start 相**发生(`[[dep]] kind = "init"` 保证相位顺序)。卷 id 与
 * 挂载点的对应是**编译期常量表**(见 fat_blk.c 的 `s_vols`), 不是运行期探测。
 * ==========================================================================
 */
#ifndef BR_ADAPTER_UBOOT_FAT_GLUE_H
#define BR_ADAPTER_UBOOT_FAT_GLUE_H

/* ★ 上游 .c 与 glue 都经**环境头**取运行环境; glue 另外直接用 vendored 的盘上结构
 * (`<fat.h>` 是 compat 的转发壳 —— 不是三方件的头, 门禁判据 ⑤ 见
 * tools/check-third-party-pristine.sh)。 */
#include "fat_env.h"
/* 本插件是**文件系统**插件: inode/文件的公共头(`br_inode_t`)与四层 ops 的类型来自 VFS
 * 契约。上游 .c 不需要它(上游只认路径字符串), 所以它在 compat 之外 —— glue 才消费它。 */
#include <br/vfs/br_vfs.h>

/* 上游 FAT 的**公开 API 面**(`fat_exists`/`fat_size`/`fat_read_file`/`fat_opendir`/
 * `fat_readdir`/`fat_closedir`/`file_fat_write`/`fat_unlink`/`fat_mkdir`/`fat_rename`/
 * `fat_set_blk_dev`/`clust_to_sect`/…) —— 声明来自 vendor 的 `include/fat.h`(经 compat 的
 * 转发壳)。glue 只走这些入口, 于是"上游原件原样参与编译"这件事在**调用面**上也成立。 */
#include <fat.h>

/* ---- 上游导出、但 vendored `<fat.h>` 里**没有**声明的入口 ----
 * `file_fat_write_at()` 是上游 `fs/fat/fat_write.c` 的公开函数(u-boot 的 `fs/fs.c` 用它做
 * `fs_write` 的"定位写"), 却没写进 `include/fat.h`。vendor 头是**逐字节上游原件**(不许编辑),
 * 而 glue 要消费它 ⇒ 原型写在适配层(与上游同签名; 有原型才有 -Wmissing-prototypes 的另一面)。
 * 也正因为有它, 本插件的截断**不需要**改上游(见 fat_blk.c 的 uboot_fat_truncate)。 */
int file_fat_write_at(const char *filename, loff_t pos, void *buffer, loff_t size,
                      loff_t *actwrite);

/* ==================================================================== 卷事实(编译期常量) */

#define UB_FAT_VOL_MAX   3u      /* 本插件挂几个卷(与 plugin.toml 的 [[mount]] 条数一致) */

/* 块设备名(dev-core 命名规则 `[a-z][a-z0-9]*`) */
/* ★ ADR-0030: 源设备名不再由本件决定(APP 的 [[mount]].source 给);
 *   下面三个只留给自检当夹具名, 与 app 的挂载计划一致。 */
#define UB_FAT_DEV12     "emmc0"
#define UB_FAT_DEV16     "emmc1"
#define UB_FAT_DEV32     "emmc2"

/*
 * 挂载点与设备名常量 —— **仅供自检当夹具名**(与 `app/hello/plugin.toml` 的 `[[mount]]` 一致)。
 *
 * ★ ADR-0030 起"哪块卡挂到哪个挂载点"由 **APP 的 `[[mount]]`** 给出: 本件只登记驱动
 *   `"fat"`, 挂载时按 `(cfg->dev, cfg->path)` **认领卷槽**(`uboot_fat_vol_claim`)。
 *   从前这里的三行 挂载点↔设备 表与 manifest 是**两处真值**(欠债 br-wa-fs-001);
 *   生成链路落地 + 表取消之后, 该欠债已还清 —— 这些常量随之退化为自检夹具名。
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

/* FAT12 的簇数上界(上游 `fs/fat/fat.c` 里那个 `MAX_FAT12` 是同名的文件域宏; 那份是
 * static 的编译单元内部量, glue 无法引用 ⇒ 这里按**盘上事实**再声明一次, 并写明出处)。 */
#define UB_FAT_MAX_FAT12 0xFF4u

/*
 * 卷的扇区大小。整个插件按"512 B 扇区"写死 —— 理由是**上游也是这么假设的**:
 * `boot_sector.sector_size` 必须等于块设备的 `blksz`(上游 `get_fs_info` 的判据), 而本
 * 插件的三块 emmc 恒 512; 挂载期把 `sect_size != 512` 判成 -EINVAL(宁可拒挂也不做
 * 4 KiB 扇区的算术)。单次 I/O 的扇区上界 = 块设备 `geometry.max_rw`(实测 64 扇区),
 * shim 内部切块。
 */
#define UB_FAT_SECTOR_SIZE   512u

/* 名字上界(本仓库 BR_NAME_MAX = 32, br_dirent_t.name[32])。
 * 长名(LFN)字节数超过它就 ⇒ BR_ERR(ENOSPC)(与 fs/tmpfs 同口径)。 */
#define UB_FAT_NAME_MAX      BR_NAME_MAX

/* 拼接后路径缓冲上界(挂载点 ≤ 8 + '/' + 名字 ≤ 31 + '\0' = 40; 留 256 给未来的深路径)。
 * 它同时是环境里 `strdup` 的上界(Ub_FAT_STRDUP_CAP 见 compat/fat_env.h)。 */
#define UB_FAT_PATH_MAX      256u

/* 变长(truncate 的"扩展"方向)时每次追加的零字节块(栈上, 512 B) */
#define UB_FAT_ZERO_CHUNK    512u

/* ==================================================================== 宿主预置内容
 *
 * 三卷里的文件由宿主在构建后门禁里造好(见 README 的"前置工件")。这些常量是本插件对
 * 那份工件形状的**契约**: 宿主造出来的形状与这里不一致 ⇒ TC-FAT-002/003 红 —— 这正是
 * 我们要的判据(而不是"自检自己造数据自己验")。
 */
#define UB_FAT_C12_HELLO       "fat12 hello\n"
#define UB_FAT_C16_HELLO       "fat16 hello\n"
#define UB_FAT_C32_HELLO       "fat32 hello\n"

/* `/dir/a.bin` 的字节数与其填充字符(见 README 的造镜像脚本契约) */
#define UB_FAT_C12_DIR_A_BYTES 16u
#define UB_FAT_C16_DIR_A_BYTES 256u
#define UB_FAT_C32_DIR_A_BYTES 4096u

/* 超长(> BR_NAME_MAX-1 = 31 字节)的长名文件名: 用来验"长名 ⇒ -ENOSPC"这条口径 */
#define UB_FAT_C32_LFN_NAME    "long name file.txt"

/* ==================================================================== 卷表与块层 */

/*
 * 卷(bdev + 几何 + **BPB 观测**)。**设备 I/O 的入口**: 上游的 `blk_dread/blk_dwrite`
 * 经 `s_cur_vol` 找到它, 于是"卷"这件事在插件里只在 shim 一处出现。
 *
 * `fatsize`/`sect_size`/`clust_size`/`total_sect` 等是**挂载期从 BPB 解析出来的观测值**
 * (上游的 `get_fs_info` 是 static, 没有对外观测点 ⇒ glue 用 vendored `boot_sector` 布局
 * 自己解析一遍; 它**只用于判据与日志**, 不参与任何 I/O 决策 —— I/O 全在上游手里)。
 */
typedef struct uboot_fat_vol {
    const char          *dev;          /* 块设备名(常量) */
    const char          *mnt;          /* 挂载点(常量) */
    const br_bdev_ops_t *ops;          /* bdev 类 API 的 ops 表 */
    void                *priv;         /* bdev 私有句柄 */
    br_bdev_geom_t       geom;         /* 注册时的几何(扇区大小/总数/max_rw) */
    br_bool              bound;        /* 卷是否已绑定(挂载成功) */
    /* ---- 以下为 BPB 观测(fat_blk.c 的 uboot_fat_vol_probe 填) ---- */
    int                  fatsize;      /* 12 / 16 / 32 */
    br_u32               sect_size;    /* 扇区字节数(必须 == geom.sector_size) */
    br_u32               clust_size;   /* 簇 = 多少扇区 */
    br_u32               total_sect;   /* 卷内扇区总数 */
    br_u32               fats;         /* FAT 份数 */
    br_u32               fat_sect;     /* FAT 起始扇区 */
    br_u32               fatlength;    /* 每份 FAT 的扇区数 */
    br_u32               rootdir_sect; /* 根目录起始扇区 */
    br_u32               rootdir_size; /* 非 FAT32 的根目录扇区数 */
    br_u32               data_begin;   /* 首簇(簇 2)的扇区号(非 FAT32 可为"负簇") */
    br_u32               root_cluster; /* FAT32 的根目录首簇 */
} uboot_fat_vol_t;

/* GET_INFO 式观测(挂载判据 + 自检用; 生产路径只经卷表内部访问) */
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
    br_inode_t head;                  /* 公共头(type / iops / fops / fpriv) */
    br_u8      vol;                   /* 卷 id(UB_FAT_VOL*) */
    br_u8      type;                  /* BR_INODE_DIR / BR_INODE_FILE */
    br_u32     size;                  /* 文件字节数(目录 = 0); lookup 时快照 */
    char       path[UB_FAT_PATH_MAX]; /* 相对挂载点; 根 = "" */
} uboot_fat_ino_t;

/* 单卷挂载体(mount 的 fs_priv; 挂载表长期持有) */
typedef struct uboot_fat_fs {
    uboot_fat_vol_t *vol;
    br_inode_t      *root;            /* 根 inode(br_malloc; 挂载表持有, 永不 free_inode) */
} uboot_fat_fs_t;

/* ---- 卷绑定/解绑/查询(fat_blk.c) ---- */
int  uboot_fat_vol_bind(uboot_fat_vol_t *vol);
void uboot_fat_vol_unbind(void);
uboot_fat_vol_t *uboot_fat_vol_get(br_u32 id);
int  uboot_fat_vol_claim(const char *dev, const char *mnt, br_u32 *id);
int  uboot_fat_vol_info(br_u32 id, uboot_fat_vol_info_t *out);

/*
 * 截断/扩展到 size(上游没有 truncate, 只在写路径里有一处 "truncate the rest" 分支)。
 * **只用上游公开 API 拼**(见 fat_blk.c 的实现与函数头), 不再有上一版那 160 行改写。
 */
int uboot_fat_truncate(const char *path, br_u64 size);

/* ==================================================================== VFS 对接的内部视图
 * 这些不是"对外 API"(本插件不导出任何 API), 而是 fat_selftest.c / fat_vfs.c 之间需要的。 */
int  uboot_fat_mount_vol_info(const char *mnt, uboot_fat_vol_info_t *out);
int  uboot_fat_join_path(const char *dir_path, const char *name, char *out, br_size_t cap);
br_bool uboot_fat_name_ok(const char *name);
int  uboot_fat_test_sync(const char *mnt);

#endif /* BR_ADAPTER_UBOOT_FAT_GLUE_H */
