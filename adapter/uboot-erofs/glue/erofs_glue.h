/*
 * adapter/uboot-erofs — **适配层内部契约**(本项目代码; 上游没有对应物)
 *
 * 上游 `fs/erofs/` 现在**原样编译**(见 ADR-0026): 它自己的 `internal.h` 提供
 * `sbi` / `erofs_blksiz()` / `iloc()` / 各函数原型; `erofs_fs.h` 提供盘上格式。
 * 本头只放**上游没有、而适配层需要的**东西:
 *
 *   - 块层: 上游 `erofs_dev_read()` / `erofs_blk_read()` 的原型在上游 `internal.h`,
 *     **实现在我们这里**(`glue/`), 接到 `br_bdev_ops_t` 上;
 *   - 挂载期事实(`br_erofs_dev_t` / 设备名 / 挂载点);
 *   - 共享 scratch 缓冲;
 *   - 夹具常量(与 `tools/mk-fs-images.sh` 逐字节对齐, 自检用)。
 *
 * 依赖方向: 本头属 `glue/`, 因此**可以** include 上游 `internal.h`; 反向上游不知道我们。
 */
#ifndef BR_ADAPTER_UBOOT_EROFS_GLUE_H
#define BR_ADAPTER_UBOOT_EROFS_GLUE_H

#include "erofs_env.h"
#include "internal.h"          /* 上游 fs/erofs/internal.h */

/* ---- 挂载期事实 ---- */
#define BR_EROFS_DEV_NAME    "emmc3"      /* bdev 名(见 platform 的 QEMU 参数) */
#define BR_EROFS_MOUNT_PATH  "/assets"    /* 挂载点(plugin.toml 的 [[mount]] 与这里两处真值) */
#define BR_EROFS_TYPE_NAME   "erofs"      /* 诊断用 */

/* 本插件支持的最大逻辑块。上游允许 blkszbits ∈ [9,12], 但压缩路径只支持 4096
 * (`EROFS_MAX_BLOCK_SIZE` 由上游 internal.h 从 PAGE_SIZE 给出 —— 这里只补"位数")。 */
#define EROFS_BLOCK_SIZE_BITS 12u

/* 瞬态 inode 里"相对挂载点路径"的静态上界(不用 BR_VFS_PATH_MAX: 那是 vfs-core 私有)。 */
#define UBOOT_EROFS_REL_PATH_MAX  256u

/* ---- 块层 ---- */
typedef struct br_erofs_dev {
    const br_bdev_ops_t *ops;
    void                *priv;
    br_u64               n_sectors;
    br_u32               sector_size;
    br_u32               max_rw;        /* 扇区 */
    br_u32               align;
} br_erofs_dev_t;

extern br_erofs_dev_t uboot_erofs_dev;      /* 挂载期绑定; 自检只读它 */
int  uboot_erofs_bdev_bind(void);           /* 取 emmc3 并记住几何 */

/* 共享 4 KiB 中转缓冲(谁写谁负责, 见 erofs_vfs.c 的说明)。 */
u8  *uboot_erofs_scratch(void);

/* ---- 夹具常量(与 tools/mk-fs-images.sh 逐字节对齐) ----
 *
 * 自检只用这些常量做**生成式**判据(第 i 字节 = 某个式子), 不内嵌大块字面量:
 *
 *   路径               大小    内容(生成式)                       mkfs 实测 datalayout
 *   ------------------------------------------------------------------------------
 *   /hello.txt          12     "erofs hello\n"                   2 = FLAT_INLINE
 *   /comp/lz4.txt    70000     "ABCDEFGH" 重复(i%8)              3 = 压缩(LZ4)
 *   /raw.bin          8192     ((i*1103515245+12345)>>16)&0xFF   3 = 压缩(LZ4, 压不动)
 *   /dir/a.bin        4096     第 i 字节 = i & 0xFF               0 = FLAT_PLAIN
 *   /dir/sub/deep.txt    5     "deep\n"                          2 = FLAT_INLINE
 *
 * 70000 不是 4096 的整数倍 ⇒ 刻意覆盖"压缩件的尾 extent"(partial decoding);
 * raw.bin 压不动(压缩比 ~50%)⇒ 覆盖 LZ4 的另一条路径。
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

#define EROFS_DIRENT_MAX_NAME    EROFS_NAME_LEN

/* EROFS 的 dirent 类型码(ftype)→ 我们的 br_dirent 类型。上游 `namei.c` 里只有反方向
 * (`erofs_ftype_from_mode` 不在上游), 这一支是**我们**的对接需要。 */
static inline u8 uboot_erofs_ftype_from_mode(umode_t mode)
{
    if (S_ISDIR(mode))  { return 2u; }   /* DT_DIR */
    if (S_ISREG(mode))  { return 1u; }   /* DT_REG */
    if (S_ISLNK(mode))  { return 7u; }   /* DT_LNK */
    if (S_ISCHR(mode))  { return 3u; }   /* DT_CHR */
    if (S_ISBLK(mode))  { return 4u; }   /* DT_BLK */
    if (S_ISFIFO(mode)) { return 5u; }   /* DT_FIFO */
    if (S_ISSOCK(mode)) { return 6u; }   /* DT_SOCK */
    return 0u;                           /* DT_UNKNOWN */
}

#endif /* BR_ADAPTER_UBOOT_EROFS_GLUE_H */
