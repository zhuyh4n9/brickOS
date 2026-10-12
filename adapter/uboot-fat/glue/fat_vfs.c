/*
 * adapter/uboot-fat — VFS 对接 + 生命周期钩子(本件新增, 无上游对应)
 *
 * ============================== 本文件干什么 ==============================
 * 把**上游原件的路径风格 API**(`fat_exists`/`fat_read_file`/`file_fat_write_at`/
 * `fat_unlink`/…, 定义在 `third_party/uboot-imx/fs/fat/fat_write.c` 那一个编译单元里)
 * 接到本项目的 VFS 契约
 * (framework/vfs-core/include/br/vfs/br_vfs.h)上:
 *
 *   层 1(super):     mount(读 BPB 判 12/16/32 + 校验卷几何) / sync(bdev flush) /
 *                     free_inode / unmount(BR_NULL)
 *   层 2(inode):     lookup / create / unlink / mkdir / rmdir / rename / getattr / setattr
 *   层 3(file):      文件面 read/write/lseek/close;目录面 read(一条 br_dirent_t)/close
 *
 * ## inode = 瞬态**路径包装**(设计 SD-3, 与 fs/tmpfs 的"持久节点"正相反)
 *   每次 lookup/create 用 br_malloc 造一个 `uboot_fat_ino_t`:
 *       { 卷 id, 相对挂载点的路径(根 = ""), 类型, 大小快照 }
 *   于是"节点"不需要在介质上建索引 —— 胶水层直接把路径交给上游的路径风格 API。
 *   所有权: **谁拿到谁释放**。free_inode 必须 br_free 掉它(照 br_vfs.c 的 ino_put 契约);
 *   唯一的例外是**挂载根**: 它由 mount 分配、挂在 uboot_fat_fs_t 上长期持有, vfs 永不把
 *   它交给 free_inode —— 但它仍然要用 br_malloc(不许进 BSS, 见 glue/fat_glue.h 的说明)。
 *
 * ## 为什么目录 read 由本层合成 "." 与 ".."
 *   FAT 的**根目录**里没有 "." / ".."(非 FAT32 的根目录甚至不在簇链上), 而 VFS 的
 *   消费者(以及 TC-VFS-*)把它当 POSIX 目录看待 ⇒ 头两条由本层就地合成(与 fs/tmpfs 同款)。
 *
 * ## 有意偏离/_欠债(见 README)
 *   · `link`/`symlink`/`readlink` 三个槽位**不填**(BR_NULL ⇒ -ENOTSUP): FAT 没有链接。
 *   · 名字上界: FAT 的长名可以到 255 字节, 而 br_dirent_t.name 只有 BR_NAME_MAX(32) ⇒
 *     长名 > 31 字节的文件在 readdir 里返回 -ENOSPC(与 fs/tmpfs 同口径, 见 uboot_fat_name_ok)。
 *   · `BR_O_APPEND` 与 `O_TRUNC` 在 fops->open 里处理; 权限执法(只读 fd 不许写)归 vfs/posix。
 * ==========================================================================
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "fat_glue.h"

/* ==================================================================== 挂载表(本插件私有的"挂载点 → 卷"
 * ★ 为什么还要一份自己的表: vfs 的挂载表只记 {path, ops, fs_priv, root}; 而本插件的
 *   "哪个挂载点对应哪块块设备"现在是**挂载计划的事实**(ADR-0030): APP 调
 *   `mount(源设备, 目标, "fat", …)` 把 (dev, mnt) 交给本件, 由 `glue/fat_blk.c` 的
 *   `uboot_fat_vol_claim()` 认领一个卷槽。
 *   这里只缓存 id 供 mount 用, 不与 vfs 的表重复存根 inode。
 */
typedef struct uboot_fat_mount_slot {
    /* ★ 挂载点是**我们自己的副本**(定长, 有界), 不是 vfs 给的指针: `cfg->path` 指向
     *   `br_vfs_mount()` 里的**栈缓冲**(`norm`), mount 返回后即失效 —— 直接存指针会在
     *   自检阶段读到被覆盖的栈内存(实测症状: `TC-FAT-001 vol_info_failed` /
     *   `TC-FAT-008 fs_ops.sync_rc=-2`, 而挂载那一刻的日志完全正常)。
     *   上界 = UB_FAT_PATH_MAX(本插件的路径口径)。 */
    char            mnt[UB_FAT_PATH_MAX];
    uboot_fat_fs_t *fs;      /* mount 成功后才填 */
    br_bool         mounted;
} uboot_fat_mount_slot_t;

static uboot_fat_mount_slot_t s_slots[UB_FAT_VOL_MAX];

/* ==================================================================== ops 表(先声明函数) */

static br_inode_t *uboot_fat_lookup (br_inode_t *dir, const char *name);
static int  uboot_fat_create (br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out);
static int  uboot_fat_unlink (br_inode_t *dir, const char *name);
static int  uboot_fat_mkdir  (br_inode_t *dir, const char *name);
static int  uboot_fat_rmdir  (br_inode_t *dir, const char *name);
static int  uboot_fat_rename (br_inode_t *dir, const char *name,
                              br_inode_t *ndir, const char *nname);
static int  uboot_fat_getattr(br_inode_t *ino, br_stat_t *st);
static int  uboot_fat_setattr(br_inode_t *ino, const br_stat_t *st);

static int   uboot_fat_file_open (br_file_t *f, br_u32 flags);
static br_s64 uboot_fat_file_read (br_file_t *f, void *buf, br_size_t n);
static br_s64 uboot_fat_file_write(br_file_t *f, const void *buf, br_size_t n);
static br_s64 uboot_fat_file_lseek(br_file_t *f, br_s64 off, int whence);
static int   uboot_fat_file_close(br_file_t *f);

static int   uboot_fat_fsync    (br_file_t *f);
static int   uboot_fat_dir_open (br_file_t *f, br_u32 flags);
static br_s64 uboot_fat_dir_read (br_file_t *f, void *buf, br_size_t n);
static int   uboot_fat_dir_close(br_file_t *f);

static int  uboot_fat_mount     (void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
static void uboot_fat_free_inode(br_inode_t *ino);
static int  uboot_fat_sync      (void *fs_priv);

/*
 * 卷级落盘(**唯一实现**): 让块设备 flush。
 *
 * 为什么是唯一一处: `fs_ops.sync` 与 `br_file_ops.fsync` 是同一件事的两个入口(卷级/句柄级),
 * 两处各写一份 = 第二个真值, 迟早不一致。
 *
 * ★ ADR-0026 之后这里**只剩** bdev.flush —— 因为不再有"glue 持有的 FAT 缓存":
 *   上游每个写入口(`file_fat_write_at`/`fat_unlink`/`fat_mkdir`/`fat_rename`)在自己内部
 *   就把 dirty 的 FAT 窗口(`flush_dirty_fat_buffer`)与目录项(`flush_dir`)落盘了, 而它的
 *   `fsdata` 是**每次调用自带、返回前 free** 的局部量(这正是上游的模型; 上一版"fsdata 归卷"
 *   是我们的偏离, 已随 ADR-0026 撤销)。因此两次 API 调用之间不存在未落盘的插件内状态,
 *   `sync` 的语义就退化成"让设备把已写的扇区真正落介质"(设计 `7-02` §1 的 SD-8 口径)。
 */
static int uboot_fat_flush_volume(uboot_fat_vol_t *vol)
{
    int rc;

    if (vol == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if ((vol->bound == BR_FALSE) || (vol->ops == BR_NULL)) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    return (vol->ops->flush(vol->priv) == 0) ? 0 : BR_ERR(EIO);
}

/* 文件面: 六个槽位填满 + 三个显式 BR_NULL(契约: 不支持的槽位 = BR_NULL ⇒ -ENOTSUP) */
static const br_file_ops_t s_file_fops = {
    .open        = uboot_fat_file_open,
    .read        = uboot_fat_file_read,
    .write       = uboot_fat_file_write,
    .lseek       = uboot_fat_file_lseek,
    .ioctl       = BR_NULL,
    .fsync       = uboot_fat_fsync,   /* 与 fs_ops.sync 同一件事: 先回写 FAT 缓存, 再让设备 flush */
    .poll        = BR_NULL,   /* 块介质恒可读写; 等待面归 v2(SD-7) */
    .poll_attach = BR_NULL,   /* v2 预留(wait-queue), v1 恒 NULL */
    .close       = uboot_fat_file_close,
};

/* 目录面(read = 一条 br_dirent_t)。写/定位对目录无语义, 槽位留空。 */
static const br_file_ops_t s_dir_fops = {
    .open        = uboot_fat_dir_open,
    .read        = uboot_fat_dir_read,
    .write       = BR_NULL,
    .lseek       = BR_NULL,
    .ioctl       = BR_NULL,
    .fsync       = uboot_fat_fsync,   /* 目录句柄的"落盘"= 本卷的落盘 */
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = uboot_fat_dir_close,
};

/*
 * 层 2: inode ops。★ 目录与文件**共用一张表**: lookup/create/… 的槽位自己判类型,
 * 而"文件节点"只需要 getattr/setattr(截断)两个槽位真的可达(vfs 只对目录派发名字空间操作)。
 * 为什么不像 tmpfs 那样也共用: 这里要让"给文件节点派发名字空间操作"得到一个明确的
 * -ENOTDIR/-EISDIR 而不是"表里恰好有实现"—— 于是本件按**类型分表**(表只有两张, 代价为零)。
 */
static const br_inode_ops_t s_dir_iops = {
    .lookup  = uboot_fat_lookup,
    .create  = uboot_fat_create,
    .unlink  = uboot_fat_unlink,
    .mkdir   = uboot_fat_mkdir,
    .rmdir   = uboot_fat_rmdir,
    .rename  = uboot_fat_rename,
    .getattr = uboot_fat_getattr,
    .setattr = uboot_fat_setattr,
    /* ADR-0013 追加(表尾): FAT 没有链接 ⇒ 三个槽位显式 BR_NULL(-ENOTSUP) */
    .symlink  = BR_NULL,
    .readlink = BR_NULL,
    .link     = BR_NULL,
};

/* 文件节点的 iops: 只有 getattr/setattr 有意义(vfs 不会给非目录派发名字空间操作,
 * 而 br_fstat / br_truncate 会经它)。 */
static const br_inode_ops_t s_file_iops = {
    .lookup  = BR_NULL,
    .create  = BR_NULL,
    .unlink  = BR_NULL,
    .mkdir   = BR_NULL,
    .rmdir   = BR_NULL,
    .rename  = BR_NULL,
    .getattr = uboot_fat_getattr,
    .setattr = uboot_fat_setattr,
    .symlink  = BR_NULL,
    .readlink = BR_NULL,
    .link     = BR_NULL,
};

static const br_fs_ops_t s_fs_ops = {
    .mount      = uboot_fat_mount,
    .free_inode = uboot_fat_free_inode,
    .unmount    = BR_NULL,   /* v1 的签名先行槽位(运行时卸载不早于 v3, O-S3) */
    .sync       = uboot_fat_sync,
};

/* ==================================================================== 小工具 */

/*
 * 有界复制(cap **含**结尾 '\0')。★ 名字刻意与 `compat/fat_env.h` 的
 * `uboot_fat_strcpy()`(2 参, 上界 256 —— 那是**给上游**用的环境实现)区分开: 这是 glue 自己的
 * 工具, 长度由调用方给(本插件的路径缓冲 `UB_FAT_PATH_MAX`)。
 */
static void uboot_fat_strcpy_bounded(char *dst, const char *src, br_size_t cap)
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

/*
 * 名字规则(唯一执法点; lookup/create/mkdir/rmdir/rename 都走它):
 *   非空、不含 '/'、长度 ≤ UB_FAT_NAME_MAX-1(= BR_NAME_MAX-1 = 31)。
 * 为什么是**长度上界**而不是"FAT 允许的 255": 本仓库的目录项是定长 br_dirent_t,
 * name[] 只有 32 字节 ⇒ 长名能建出来却列不出来, 那比直接拒绝更坏(与 fs/tmpfs 同口径)。
 * 逐字节扫到 NUL 或扫满 BR_NAME_MAX ⇒ 未终止的串也**只读 32 字节**。
 */
br_bool uboot_fat_name_ok(const char *name)
{
    br_u32 i;

    if (name == BR_NULL) {
        return BR_FALSE;
    }
    if (name[0] == '\0') {
        return BR_FALSE;
    }
    for (i = 0u; i < UB_FAT_NAME_MAX; i++) {
        if (name[i] == '\0') {
            return BR_TRUE;
        }
        if (name[i] == '/') {
            return BR_FALSE;
        }
    }
    return BR_FALSE;                 /* 扫满窗口仍无 NUL ⇒ 超过 BR_NAME_MAX-1 */
}

/* 名字是 "." / ".." 吗(变更操作用它显式拒绝 —— 根的自指语义见 lookup) */
static br_bool uboot_fat_name_dot(const char *name)
{
    if (name == BR_NULL) {
        return BR_FALSE;
    }
    if ((name[0] == '.') && (name[1] == '\0')) {
        return BR_TRUE;
    }
    if ((name[0] == '.') && (name[1] == '.') && (name[2] == '\0')) {
        return BR_TRUE;
    }
    return BR_FALSE;
}

/*
 * 拼接"父目录相对路径 + 名字"→ 相对路径(本插件所有路径的唯一构造处)。
 *   dir_path == "" ⇒ 结果就是 name(挂载根下的直接子项)
 *   dir_path != "" ⇒ "dir_path/name"
 * 覆盖写 out(调用方给的栈缓冲, cap ≥ UB_FAT_PATH_MAX)。返回 0 / -ENOSPC。
 */
int uboot_fat_join_path(const char *dir_path, const char *name,
                        char *out, br_size_t cap)
{
    br_size_t n = 0u;
    br_size_t i;

    if ((dir_path == BR_NULL) || (name == BR_NULL) || (out == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if (dir_path[0] != '\0') {
        for (i = 0u; dir_path[i] != '\0'; i++) {
            if ((n + 1u) >= cap) {
                return BR_ERR(ENOSPC);
            }
            out[n++] = dir_path[i];
        }
        if ((n + 1u) >= cap) {
            return BR_ERR(ENOSPC);
        }
        out[n++] = '/';
    }
    for (i = 0u; name[i] != '\0'; i++) {
        if ((n + 1u) >= cap) {
            return BR_ERR(ENOSPC);
        }
        out[n++] = name[i];
    }
    out[n] = '\0';
    return 0;
}

/* 相对路径的父目录(就地截断): "" 的父仍是 ""; "a" 的父是 ""; "a/b" 的父是 "a"。 */
static void uboot_fat_parent_path(const char *path, char *out, br_size_t cap)
{
    br_size_t n = uboot_fat_strlen(path);

    while ((n > 0u) && (path[n - 1u] != '/')) {
        n--;
    }
    if ((n > 0u) && (path[n - 1u] == '/')) {
        n--;                          /* 去掉分隔符本身 */
    }
    if (n >= cap) {
        n = (cap > 0u) ? (cap - 1u) : 0u;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

/* 相对路径的深度(分量个数; "" = 0)。用来给"环"设一个静态上界(与 vfs 的路径上界一致)。 */
static br_u32 uboot_fat_path_depth(const char *path)
{
    br_u32 depth = 0u;

    for (br_size_t i = 0u; path[i] != '\0'; i++) {
        if ((i == 0u) || ((path[i] != '/') && (path[i - 1u] == '/'))) {
            depth++;
        }
    }
    return depth;
}

/*
 * 目录句柄的**会话状态**: 上游 `fat_opendir()` 交回一个不透明的
 * `struct fs_dir_stream *`(它的具体类型 `fat_dir` 是上游那个编译单元的私事, glue 看不见),
 * 而 VFS 面的目录读还要额外合成两条 "."/".." 记录 ⇒ 游标必须由 glue 自己持有。
 * 于是每个 open 造一个包装: {上游的流, 游标}; close 时把两者都还回去。
 * 游标: 0 = ".", 1 = "..", ≥2 = 第 (cursor-2) 条真目录项。
 */
typedef struct uboot_fat_dirstream {
    struct fs_dir_stream *ds;      /* 上游 fat_opendir 给的流(close 时 fat_closedir 归还) */
    br_u32                cursor;  /* vfs 面的迭代游标(见上) */
} uboot_fat_dirstream_t;

/* 节点身份: head.fpriv == 节点自己(CA-2 的"首字段"布局; 与 tmpfs 同款) */
static uboot_fat_ino_t *uboot_fat_node(const br_inode_t *ino)
{
    return (uboot_fat_ino_t *)ino->fpriv;
}

/* 造一个瞬态节点。 `type` = BR_INODE_DIR/BR_INODE_FILE; `size` 只对文件有意义。 */
static uboot_fat_ino_t *uboot_fat_node_new(br_u32 vol, br_u32 type,
                                           const char *path, br_u32 size)
{
    uboot_fat_ino_t *n = (uboot_fat_ino_t *)br_malloc(sizeof(*n));

    if (n == BR_NULL) {
        return BR_NULL;
    }
    n->head.type  = type;
    n->head.iops  = (type == BR_INODE_DIR) ? &s_dir_iops : &s_file_iops;
    n->head.fops  = (type == BR_INODE_DIR) ? &s_dir_fops : &s_file_fops;
    n->head.fpriv = n;
    n->vol        = (br_u8)vol;
    n->type       = (br_u8)type;
    n->size       = size;
    /* 路径在构造点就已经被 uboot_fat_join_path 限长; 这里再做一次有界复制是纯防御。 */
    uboot_fat_strcpy_bounded(n->path, path, UB_FAT_PATH_MAX);
    return n;
}

/* 只对文件节点取"当前长度"(目录 ⇒ -EISDIR)。path 为空(= 卷根)时也是目录。 */
static int uboot_fat_size_of(const uboot_fat_ino_t *n, br_u32 *out)
{
    loff_t sz = 0;
    uboot_fat_vol_t *vol;
    int rc;

    vol = uboot_fat_vol_get(n->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    if (n->path[0] == '\0') {
        return BR_ERR(EISDIR);          /* 卷根 ⇒ 目录 */
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    if (fat_exists(n->path) == 0) {
        /* ★ 关键: 先判存在。u-boot 的 fs_size() 对"是目录"的路径**故意返回成功 + size 0**
         * (它的注释写着 fs_size 不该对目录失败)—— 直接用它会把目录当成 0 字节文件。 */
        return BR_ERR(ENOENT);
    }
    rc = fat_size(n->path, &sz);
    if (rc < 0) {
        return rc;
    }
    if (sz < 0) {
        return BR_ERR(EIO);
    }
    *out = (br_u32)sz;
    return 0;
}

/* 目录项的路径 → 类型(存在性判定 + 类型判定一次做完)。未命中 ⇒ -ENOENT。 */
static int uboot_fat_kind_of(const uboot_fat_ino_t *n, br_bool *is_dir)
{
    uboot_fat_vol_t *vol = uboot_fat_vol_get(n->vol);
    int rc;

    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    if (n->path[0] == '\0') {
        *is_dir = BR_TRUE;               /* 卷根 */
        return 0;
    }
    /* 目录: 只有目录能被 fat_opendir 打开(文件 ⇒ -ENOTDIR) */
    {
        struct fs_dir_stream *ds = BR_NULL;
        int rc2 = fat_opendir(n->path, &ds);

        if (rc2 == 0) {
            fat_closedir(ds);
            *is_dir = BR_TRUE;
            return 0;
        }
    }
    if (fat_exists(n->path) != 0) {
        *is_dir = BR_FALSE;
        return 0;
    }
    return BR_ERR(ENOENT);
}

/* 卷内绝对路径(上游风格): "/hello.txt" / "/dir" / "/"。返回 0 或负 errno。 */
static int uboot_fat_raw_path(const uboot_fat_ino_t *n, char *out, br_size_t cap)
{
    const br_size_t need = uboot_fat_strlen(n->path) + 2u;

    if ((out == BR_NULL) || (need > cap)) {
        return BR_ERR(ENOSPC);
    }
    out[0] = '/';
    memcpy(out + 1, n->path, uboot_fat_strlen(n->path) + 1u);
    return 0;
}

/* 目录项名 → br_dirent_t(VFS 的定长记录)。名字超上界 ⇒ -ENOSPC(见文件头)。 */
static int uboot_fat_fill_dirent(br_dirent_t *de, br_u32 type, const char *name)
{
    const br_size_t len = uboot_fat_strlen(name);

    if (len >= (br_size_t)UB_FAT_NAME_MAX) {
        return BR_ERR(ENOSPC);
    }
    memset(de, 0, sizeof(*de));
    de->type     = type;
    de->name_len = (br_u32)len;
    memcpy(de->name, name, len + 1u);
    return 0;
}

/* ==================================================================== 层 3: 文件面 */

static int uboot_fat_file_open(br_file_t *f, br_u32 flags)
{
    uboot_fat_ino_t *n = (uboot_fat_ino_t *)br_file_fpriv(f);
    int rc;

    if ((n == BR_NULL) || (n->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);
    }
    /* O_TRUNC: 打开即截断到 0(走同一个截断原语, 没有第二条"清空"路径) */
    if ((flags & BR_O_TRUNC) != 0u) {
        char raw[UB_FAT_PATH_MAX];

        rc = uboot_fat_raw_path(n, raw, sizeof(raw));
        if (rc != 0) {
            return rc;
        }
        rc = uboot_fat_truncate(raw, 0u);
        if (rc != 0) {
            return rc;
        }
        n->size = 0u;
    }
    if ((flags & BR_O_APPEND) != 0u) {
        br_u32 sz = 0u;

        rc = uboot_fat_size_of(n, &sz);      /* 每次打开都从介质取真值, 不信缓存的 size */
        if (rc != 0) {
            return rc;
        }
        n->size = sz;
        br_file_set_offset(f, (br_s64)sz);
    }
    return 0;
}

static br_s64 uboot_fat_file_read(br_file_t *f, void *buf, br_size_t n)
{
    uboot_fat_ino_t *node = (uboot_fat_ino_t *)br_file_fpriv(f);
    uboot_fat_vol_t *vol;
    char raw[UB_FAT_PATH_MAX];
    br_u32 size = 0u;
    br_s64 off;
    br_size_t want;
    loff_t act = 0;
    int rc;

    if ((node == BR_NULL) || (node->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);
    }
    if (n == 0u) {
        return 0;                            /* 空读是空操作 */
    }
    rc = uboot_fat_size_of(node, &size);
    if (rc != 0) {
        return rc;
    }
    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(EINVAL);
    }
    if ((br_u64)off >= (br_u64)size) {
        return 0;                            /* 读到/越过 EOF: 0 字节是**成功** */
    }

    vol = uboot_fat_vol_get(node->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_raw_path(node, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    want = n;
    if (want > (br_size_t)((br_u64)size - (br_u64)off)) {
        want = (br_size_t)((br_u64)size - (br_u64)off);
    }
    /* ★ len 必须显式给 want: 上游 `len == 0` 的语义是"读到文件末尾", 与 POSIX read 不同。 */
    rc = fat_read_file(raw, buf, (loff_t)off, (loff_t)want, &act);
    if (rc < 0) {
        return rc;
    }
    br_file_set_offset(f, off + (br_s64)act);
    return (br_s64)act;
}

static br_s64 uboot_fat_file_write(br_file_t *f, const void *buf, br_size_t n)
{
    uboot_fat_ino_t *node = (uboot_fat_ino_t *)br_file_fpriv(f);
    uboot_fat_vol_t *vol;
    char raw[UB_FAT_PATH_MAX];
    br_s64 off;
    loff_t act = 0;
    int rc;

    if ((node == BR_NULL) || (node->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);
    }
    /* BR_O_APPEND 的契约是"**每次写前**定位到末尾", 所以零长度写也要先定位 */
    if ((br_file_flags(f) & BR_O_APPEND) != 0u) {
        br_u32 sz = 0u;

        rc = uboot_fat_size_of(node, &sz);
        if (rc != 0) {
            return rc;
        }
        node->size = sz;
        br_file_set_offset(f, (br_s64)sz);
    }
    if (n == 0u) {
        return 0;
    }
    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(EINVAL);
    }

    vol = uboot_fat_vol_get(node->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_raw_path(node, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    /* 上游的语义: 从 pos 写 size 字节; **不允许空洞**(pos > 现长 ⇒ -EINVAL) */
    rc = file_fat_write_at(raw, off, (void *)(br_uintptr_t)buf, (loff_t)n, &act);
    if (rc < 0) {
        return rc;
    }
    br_file_set_offset(f, off + (br_s64)act);
    if ((br_u64)(off + (br_s64)act) > (br_u64)node->size) {
        node->size = (br_u32)(off + (br_s64)act);   /* 快照跟上(目录项的真值在介质上) */
    }
    return (br_s64)act;
}

static br_s64 uboot_fat_file_lseek(br_file_t *f, br_s64 off, int whence)
{
    uboot_fat_ino_t *node = (uboot_fat_ino_t *)br_file_fpriv(f);
    br_s64 base;
    br_s64 pos;
    int rc;

    if ((node == BR_NULL) || (node->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);
    }
    switch (whence) {
    case BR_SEEK_SET:
        base = 0;
        break;
    case BR_SEEK_CUR:
        base = br_file_offset(f);
        break;
    case BR_SEEK_END: {
        br_u32 sz = 0u;

        rc = uboot_fat_size_of(node, &sz);   /* 从介质取, 不用可能过期的快照 */
        if (rc != 0) {
            return rc;
        }
        node->size = sz;
        base = (br_s64)sz;
        break;
    }
    default:
        return BR_ERR(EINVAL);
    }
    if (base < 0) {
        return BR_ERR(EINVAL);
    }
    if (__builtin_add_overflow(base, off, &pos) || (pos < 0)) {
        return BR_ERR(EINVAL);
    }
    br_file_set_offset(f, pos);
    return pos;
}

/*
 * `fops->fsync`(= runtime/posix 的 `fsync(fd)`)。
 *
 * ★ 为什么它必须有(而不是像 v1 早期那样留 BR_NULL):
 *   `fsync` 的语义是"把我写下去的字节弄到介质上", 而本插件的写路径已经**逐次**把数据与
 *   目录项落盘, 唯一可能还留在内存里的是 **FAT 缓存**(`fsdata.fatbuf`, 6 扇区窗口,
 *   由 set_fatent_value 标记 dirty)。所以 fsync = ①回写 dirty 的 FAT 窗口 ②让 bdev flush。
 *   留 BR_NULL 会让消费者拿到 -ENOTSUP, 而它在语义上并不是"不支持"。
 *
 * 用 `br_file_inode()` 取节点而不是 `br_file_fpriv()`: **目录句柄**的 fpriv 已经被
 * 目录迭代器占用了(fat_dir*), 而 inode 的 fpriv 恒等于节点本身(CA-2 首字段布局)。
 */
static int uboot_fat_fsync(br_file_t *f)
{
    const br_inode_t *ino = br_file_inode(f);
    const uboot_fat_ino_t *node = (ino != BR_NULL) ? uboot_fat_node(ino) : BR_NULL;
    uboot_fat_vol_t *vol;

    if (node == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    vol = uboot_fat_vol_get(node->vol);
    return uboot_fat_flush_volume(vol);   /* 与 fs_ops.sync **同一段代码** */
}

static int uboot_fat_file_close(br_file_t *f)
{
    /* 会话状态 = inode 里的路径(不在 fpriv 里另开一份) ⇒ 没有要归还的东西。
     * vfs 随后会经 free_inode 释放**瞬态 inode** 本身。 */
    (void)f;
    return 0;
}

/* ==================================================================== 层 3: 目录面 */

static int uboot_fat_dir_open(br_file_t *f, br_u32 flags)
{
    uboot_fat_ino_t *node = (uboot_fat_ino_t *)br_file_fpriv(f);
    uboot_fat_vol_t *vol;
    uboot_fat_dirstream_t *st;
    struct fs_dir_stream *ds = BR_NULL;
    char raw[UB_FAT_PATH_MAX];
    int rc;

    if ((node == BR_NULL) || (node->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    (void)flags;                     /* vfs 已挡下"目录写打开 ⇒ -EISDIR" */
    vol = uboot_fat_vol_get(node->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_raw_path(node, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    rc = fat_opendir(raw, &ds);
    if (rc < 0) {
        return rc;
    }
    /* 会话状态(迭代游标)放在**文件句柄**的 fpriv 上, 不动节点身份 */
    st = (uboot_fat_dirstream_t *)br_malloc(sizeof(*st));
    if (st == BR_NULL) {
        fat_closedir(ds);
        return BR_ERR(ENOMEM);
    }
    st->ds     = ds;
    st->cursor = 0u;
    br_file_set_fpriv(f, st);
    return 0;
}

static br_s64 uboot_fat_dir_read(br_file_t *f, void *buf, br_size_t n)
{
    uboot_fat_dirstream_t *st = (uboot_fat_dirstream_t *)br_file_fpriv(f);
    br_dirent_t *de = (br_dirent_t *)buf;
    struct fs_dirent *dent = BR_NULL;
    int rc;

    if ((st == BR_NULL) || (st->ds == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* 定长记录不可截断(与 fs/tmpfs 同口径): 装不下一条就 -EINVAL ——
     * 返回 0 会被 br_readdir 读成"目录已穷尽", 那是撒谎。 */
    if ((buf == BR_NULL) || (n < (br_size_t)sizeof(br_dirent_t))) {
        return BR_ERR(EINVAL);
    }

    /* 头两条就地合成(见文件头): FAT 的根目录里没有 "."/".."。 */
    if (st->cursor == 0u) {
        st->cursor = 1u;
        rc = uboot_fat_fill_dirent(de, BR_INODE_DIR, ".");
        return (rc == 0) ? (br_s64)sizeof(br_dirent_t) : rc;
    }
    if (st->cursor == 1u) {
        st->cursor = 2u;
        rc = uboot_fat_fill_dirent(de, BR_INODE_DIR, "..");
        return (rc == 0) ? (br_s64)sizeof(br_dirent_t) : rc;
    }
    st->cursor++;

    rc = fat_readdir(st->ds, &dent);
    if (rc < 0) {
        return 0;                    /* 上游用 -ENOENT 表示"目录穷尽" ⇒ 收敛成 0 */
    }
    if (dent == BR_NULL) {
        return 0;
    }
    rc = uboot_fat_fill_dirent(de,
                               (dent->type == FS_DT_DIR) ? BR_INODE_DIR : BR_INODE_FILE,
                               dent->name);
    if (rc != 0) {
        return rc;
    }
    return (br_s64)sizeof(br_dirent_t);
}

static int uboot_fat_dir_close(br_file_t *f)
{
    uboot_fat_dirstream_t *st = (uboot_fat_dirstream_t *)br_file_fpriv(f);

    if (st != BR_NULL) {
        if (st->ds != BR_NULL) {
            fat_closedir(st->ds);
        }
        br_free(st);
        br_file_set_fpriv(f, BR_NULL);   /* 让二次 close 成为空操作 */
    }
    return 0;
}

/* ==================================================================== 层 2: inode ops */

static br_inode_t *uboot_fat_lookup(br_inode_t *dir, const char *name)
{
    uboot_fat_ino_t *d;
    uboot_fat_ino_t *out = BR_NULL;
    char parent[UB_FAT_PATH_MAX];
    char child[UB_FAT_PATH_MAX];
    br_bool is_dir = BR_FALSE;
    br_u32 size = 0u;
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR) || (name == BR_NULL)) {
        return BR_NULL;
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_NULL;
    }

    /* "." / "..": 就地解析(根的父亲 = 根, 于是 "/.." == "/" 这条 POSIX 性质成立) */
    if (uboot_fat_name_dot(name) == BR_TRUE) {
        const char *parent_path;

        if (uboot_fat_strcmp(name, ".") == 0) {
            return &d->head;
        }
        uboot_fat_parent_path(d->path, parent, sizeof(parent));
        parent_path = parent;
        out = uboot_fat_node_new(d->vol, BR_INODE_DIR, parent_path, 0u);
        return (out != BR_NULL) ? &out->head : BR_NULL;
    }

    /* 非法名字(空/含 '/')在走查里就是"不存在"; 超长 ⇒ 本层没有错误码通道(返回 BR_NULL,
     * vfs 会给 -ENOENT)。名字超上界这条**判据**在 create/mkdir/readdir 上有显式 errno。 */
    if (uboot_fat_name_ok(name) == BR_FALSE) {
        return BR_NULL;
    }
    /* 深度上界: 相对路径 ≤ BR_NAME_MAX-1 的分量数(纯防御; 真值由 vfs 的路径缓冲管) */
    if (uboot_fat_path_depth(d->path) >= (UB_FAT_NAME_MAX - 1u)) {
        return BR_NULL;
    }

    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return BR_NULL;
    }
    if (child[0] == '\0') {
        return BR_NULL;
    }

    {
        uboot_fat_ino_t probe;

        probe.vol  = d->vol;
        probe.type = BR_INODE_NONE;
        probe.size = 0u;
        uboot_fat_strcpy_bounded(probe.path, child, UB_FAT_PATH_MAX);
        rc = uboot_fat_kind_of(&probe, &is_dir);
        if (rc == BR_ERR(ENOENT)) {
            return BR_NULL;                 /* 未命中: BR_NULL(不是错误码) */
        }
        if (rc != 0) {
            return BR_NULL;
        }
        if (is_dir == BR_FALSE) {
            rc = uboot_fat_size_of(&probe, &size);
            if (rc != 0) {
                size = 0u;                  /* 取不到长度就报 0; getattr 会重新问介质 */
            }
        }
    }

    out = uboot_fat_node_new(d->vol,
                             (is_dir == BR_TRUE) ? BR_INODE_DIR : BR_INODE_FILE,
                             child, size);
    return (out != BR_NULL) ? &out->head : BR_NULL;
}

static int uboot_fat_create(br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out)
{
    uboot_fat_ino_t *d;
    uboot_fat_ino_t *n;
    char child[UB_FAT_PATH_MAX];
    br_bool is_dir = BR_FALSE;
    int rc;

    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *out = BR_NULL;
    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    /* 名字规则里"超长"这一支给 -ENOSPC(与 fs/tmpfs 同口径: 本仓库用定长目录项) */
    if (uboot_fat_name_dot(name) == BR_TRUE) {
        return BR_ERR(EINVAL);
    }
    if (uboot_fat_name_ok(name) == BR_FALSE) {
        return BR_ERR(ENOSPC);
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }

    {
        uboot_fat_ino_t probe;

        probe.vol  = d->vol;
        probe.type = BR_INODE_NONE;
        probe.size = 0u;
        uboot_fat_strcpy_bounded(probe.path, child, UB_FAT_PATH_MAX);
        rc = uboot_fat_kind_of(&probe, &is_dir);
        if (rc == 0) {
            /* 已存在: O_EXCL ⇒ -EEXIST; 否则复用(Linux 口径; vfs 的走查已经先 lookup 过一次) */
            if ((flags & BR_O_EXCL) != 0u) {
                return BR_ERR(EEXIST);
            }
            if (is_dir == BR_TRUE) {
                return BR_ERR(EISDIR);
            }
            n = uboot_fat_node_new(d->vol, BR_INODE_FILE, child, 0u);
            if (n == BR_NULL) {
                return BR_ERR(ENOMEM);
            }
            rc = uboot_fat_size_of(n, &n->size);
            if (rc != 0) {
                br_free(n);
                return rc;
            }
            *out = &n->head;
            return 0;
        }
        if (rc != BR_ERR(ENOENT)) {
            return rc;
        }
    }

    /* 真正建文件。步骤与 fs/tmpfs 的 create 同型: 先"准入判据"(能不能写), 再落介质。
     * 先写 0 字节(上游 file_fat_write_at 的 create_link 路径)把目录项建出来,
     * 之后每次 write 才能走到"文件已存在"的分支。 */
    {
        uboot_fat_vol_t *vol = uboot_fat_vol_get(d->vol);
        char raw[UB_FAT_PATH_MAX];
        loff_t act = 0;

        if (vol == BR_NULL) {
            return BR_ERR(ENODEV);
        }
        rc = uboot_fat_vol_bind(vol);
        if (rc != 0) {
            return rc;
        }
        raw[0] = '/';
        memcpy(raw + 1, child, uboot_fat_strlen(child) + 1u);
        rc = file_fat_write_at(raw, 0, (void *)(br_uintptr_t)"", 0, &act);
        if (rc < 0) {
            return rc;                          /* 介质写不下/名字非法 ⇒ 原样上传 */
        }
    }

    n = uboot_fat_node_new(d->vol, BR_INODE_FILE, child, 0u);
    if (n == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    *out = &n->head;
    return 0;
}

static int uboot_fat_unlink(br_inode_t *dir, const char *name)
{
    uboot_fat_ino_t *d;
    uboot_fat_vol_t *vol;
    char child[UB_FAT_PATH_MAX];
    char raw[UB_FAT_PATH_MAX];
    br_bool is_dir = BR_FALSE;
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    if ((uboot_fat_name_dot(name) == BR_TRUE) || (uboot_fat_name_ok(name) == BR_FALSE)) {
        return BR_ERR(EINVAL);
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }
    {
        uboot_fat_ino_t probe;

        probe.vol  = d->vol;
        probe.type = BR_INODE_NONE;
        probe.size = 0u;
        uboot_fat_strcpy_bounded(probe.path, child, UB_FAT_PATH_MAX);
        rc = uboot_fat_kind_of(&probe, &is_dir);
        if (rc != 0) {
            return rc;                       /* -ENOENT 原样上传 */
        }
        if (is_dir == BR_TRUE) {
            return BR_ERR(EISDIR);        /* 目录要 rmdir */
        }
    }
    vol = uboot_fat_vol_get(d->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    raw[0] = '/';
    memcpy(raw + 1, child, uboot_fat_strlen(child) + 1u);
    rc = fat_unlink(raw);
    return rc;
}

static int uboot_fat_mkdir(br_inode_t *dir, const char *name)
{
    uboot_fat_ino_t *d;
    uboot_fat_vol_t *vol;
    char child[UB_FAT_PATH_MAX];
    char raw[UB_FAT_PATH_MAX];
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    if ((uboot_fat_name_dot(name) == BR_TRUE) || (uboot_fat_name_ok(name) == BR_FALSE)) {
        return BR_ERR(ENOSPC);
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }
    vol = uboot_fat_vol_get(d->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    /* 上游的 raw 路径是 "/a/b"; 挂载根 ⇒ "/" */
    raw[0] = '/';
    memcpy(raw + 1, child, uboot_fat_strlen(child) + 1u);
    /* 上游对"已存在"返回 -EEXIST(见 fat_mkdir), 与 vfs 的期望一致 */
    return fat_mkdir(raw);
}

static int uboot_fat_rmdir(br_inode_t *dir, const char *name)
{
    uboot_fat_ino_t *d;
    uboot_fat_vol_t *vol;
    char child[UB_FAT_PATH_MAX];
    char raw[UB_FAT_PATH_MAX];
    br_bool is_dir = BR_FALSE;
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    if ((uboot_fat_name_dot(name) == BR_TRUE) || (uboot_fat_name_ok(name) == BR_FALSE)) {
        return BR_ERR(EINVAL);
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }
    {
        uboot_fat_ino_t probe;

        probe.vol  = d->vol;
        probe.type = BR_INODE_NONE;
        probe.size = 0u;
        uboot_fat_strcpy_bounded(probe.path, child, UB_FAT_PATH_MAX);
        rc = uboot_fat_kind_of(&probe, &is_dir);
        if (rc != 0) {
            return rc;
        }
        if (is_dir == BR_FALSE) {
            return BR_ERR(ENOTDIR);
        }
    }
    vol = uboot_fat_vol_get(d->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    raw[0] = '/';
    memcpy(raw + 1, child, uboot_fat_strlen(child) + 1u);
    return fat_unlink(raw);
}

static int uboot_fat_rename(br_inode_t *dir, const char *name,
                            br_inode_t *ndir, const char *nname)
{
    uboot_fat_ino_t *d;
    uboot_fat_ino_t *nd;
    uboot_fat_vol_t *vol;
    char child[UB_FAT_PATH_MAX];
    char nchild[UB_FAT_PATH_MAX];
    char old_raw[UB_FAT_PATH_MAX];
    char new_raw[UB_FAT_PATH_MAX];
    int rc;

    if ((dir == BR_NULL) || (ndir == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if ((dir->type != BR_INODE_DIR) || (ndir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    /* 源与目标是同一卷(跨挂载由 vfs 在更前面挡下) */
    if (dir->iops != &s_dir_iops) {
        return BR_ERR(EXDEV);
    }
    d  = uboot_fat_node(dir);
    nd = uboot_fat_node(ndir);
    if ((d == BR_NULL) || (nd == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if (d->vol != nd->vol) {
        return BR_ERR(EXDEV);
    }
    if ((uboot_fat_name_dot(name) == BR_TRUE) ||
        (uboot_fat_name_dot(nname) == BR_TRUE) ||
        (uboot_fat_name_ok(name) == BR_FALSE) ||
        (uboot_fat_name_ok(nname) == BR_FALSE)) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_join_path(nd->path, nname, nchild, sizeof(nchild));
    if (rc != 0) {
        return rc;
    }
    vol = uboot_fat_vol_get(d->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    old_raw[0] = '/';
    memcpy(old_raw + 1, child, uboot_fat_strlen(child) + 1u);
    new_raw[0] = '/';
    memcpy(new_raw + 1, nchild, uboot_fat_strlen(nchild) + 1u);
    /* 上游的 fat_rename 已经做了"不许把目录搬进自己的子树"(check_path_prefix),
     * 本层不重复实现(第二份实现 = 第二个真值)。 */
    return fat_rename(old_raw, new_raw);
}

static int uboot_fat_getattr(br_inode_t *ino, br_stat_t *st)
{
    uboot_fat_ino_t *n;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if ((ino->type != BR_INODE_DIR) && (ino->type != BR_INODE_FILE)) {
        return BR_ERR(ENOTSUP);
    }
    n = uboot_fat_node(ino);
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    st->valid = BR_STAT_SIZE;      /* nlink 不置位: FAT 没有硬链接(契约允许不填) */
    st->type  = ino->type;
    if (ino->type == BR_INODE_DIR) {
        st->size = 0u;             /* 目录的 size 在本仓库无契约, 取 0(与 tmpfs 同) */
    } else {
        br_u32 sz = 0u;
        const int rc = uboot_fat_size_of(n, &sz);   /* 每次问介质(目录项的 size 是唯一真值) */

        if (rc != 0) {
            return rc;
        }
        n->size = sz;
        st->size = sz;
    }
    st->nlink = 0u;
    return 0;
}

static int uboot_fat_setattr(br_inode_t *ino, const br_stat_t *st)
{
    uboot_fat_ino_t *n;
    uboot_fat_vol_t *vol;
    char raw[UB_FAT_PATH_MAX];
    int rc;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* v1 只认 size(截断)。不认的位**必须**报 -ENOTSUP —— 假装成功比拒绝危险。 */
    if ((st->valid & ~(br_u32)BR_STAT_SIZE) != 0u) {
        return BR_ERR(ENOTSUP);
    }
    if ((st->valid & BR_STAT_SIZE) == 0u) {
        return 0;                  /* 没要求改任何东西 */
    }
    if (ino->type != BR_INODE_FILE) {
        return BR_ERR(EISDIR);
    }
    n = uboot_fat_node(ino);
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    vol = uboot_fat_vol_get(n->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_raw_path(n, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_truncate(raw, st->size);
    if (rc == 0) {
        n->size = (br_u32)st->size;
    }
    return rc;
}

/* ==================================================================== 层 1: super */

/*
 * BPB 判定 + **卷几何校验**。上游的 get_fs_info 只做"参数是否自洽"; 本件的 mount 还得回答
 * "这个 BPB 与背后那块设备的几何对得上吗"—— 对不上就拒挂(首败即停机是刻意的)。
 * 校验三件事(全部由 BPB 与 geometry 机械可判):
 *   ① 卷内扇区总数 ≤ 设备扇区数(否则读会越出设备);
 *   ② 数据区起点(可负, FAT12/16 的"负簇"约定)+ 簇数 ≥ 0 且在卷内;
 *   ③ 簇数落在该 FAT 类型的**定义区间**(12: < 4085; 16: 4085..65524; 32: ≥ 65525)——
 *      这条同时验证了 determine_legacy_fat_bits 的判定与 BPB 自洽。
 */
static int uboot_fat_mount_check(const uboot_fat_vol_t *vol, char *why, br_size_t cap)
{
    /* ★ 这里的 `vol->*` 是**挂载期从 BPB 解析出来的观测值**(fat_blk.c 的
     * uboot_fat_vol_probe), 不是第二份 fsdata: 上游每个 API 自带 fsdata, glue 拿不到它, 也
     * 不需要它 —— 判据只需要"类型/簇数/数据区在卷内"这三个事实。 */
    const uboot_fat_vol_t *fs = vol;
    const br_u64 n_sectors = vol->geom.n_sectors;
    br_u32 data_start;
    br_u32 total_clusters;
    br_u32 lo, hi;

    if (fs->clust_size == 0u) {
        uboot_fat_strcpy_bounded(why, "cluster size is zero", cap);
        return BR_ERR(EINVAL);
    }
    if ((br_u64)fs->total_sect > n_sectors) {
        uboot_fat_strcpy_bounded(why, "BPB total_sect > device sectors", cap);
        return BR_ERR(EINVAL);
    }

    if (fs->fatsize == 32) {
        /* FAT32: data_begin 已经"等于"首簇(簇 2)的扇区号 */
        data_start = fs->data_begin + (fs->clust_size * 2u);
    } else {
        data_start = fs->rootdir_sect + fs->rootdir_size;
    }
    if (data_start >= fs->total_sect) {
        uboot_fat_strcpy_bounded(why, "data area starts past volume end", cap);
        return BR_ERR(EINVAL);
    }
    total_clusters = (fs->total_sect - data_start) / fs->clust_size;

    switch (fs->fatsize) {
    case 12:
        lo = 1u;
        hi = UB_FAT_MAX_FAT12;                  /* ≤ 0xFF4(见 glue/fat_glue.h 的出处) */
        break;
    case 16:
        lo = UB_FAT_MAX_FAT12 + 1u;             /* 4085 */
        hi = 65524u;
        break;
    case 32:
        lo = 65525u;
        hi = 0x0ffffff0u;                       /* FAT32 的簇号空间 */
        break;
    default:
        uboot_fat_strcpy_bounded(why, "unknown FAT type", cap);
        return BR_ERR(EINVAL);
    }
    if ((total_clusters < lo) || (total_clusters > hi)) {
        uboot_fat_strcpy_bounded(why, "cluster count inconsistent with FAT type", cap);
        return BR_ERR(EINVAL);
    }
    return 0;
}

static int uboot_fat_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    uboot_fat_fs_t *fs;
    uboot_fat_vol_t *vol;
    uboot_fat_ino_t *r;
    char why[64];
    br_u32 id = 0u;
    int rc;

    (void)fs_priv;      /* ★ ADR-0030: 驱动注册时不带挂载体 —— 每次 mount 各造一份 */

    if ((cfg == BR_NULL) || (cfg->path == BR_NULL) || (cfg->dev == BR_NULL) ||
        (root == BR_NULL)) {
        br_log_error("uboot-fat: mount 参数不全(path=%s dev=%s) —— "
                     "介质类 FS 必须由 APP 给出源设备(mount(source, target, \"fat\", …))",
                     (cfg != BR_NULL && cfg->path != BR_NULL) ? cfg->path : "(null)",
                     (cfg != BR_NULL && cfg->dev != BR_NULL) ? cfg->dev : "(null)");
        return BR_ERR(EINVAL);
    }
    *root = BR_NULL;

    /* 卷 = (源设备, 挂载点) 这一对, 由 APP 的挂载计划给出; 认领一个槽。 */
    rc = uboot_fat_vol_claim(cfg->dev, cfg->path, &id);
    if (rc != 0) {
        br_log_error("uboot-fat: 认领卷失败 (dev=%s mnt=%s) rc=%d", cfg->dev, cfg->path, rc);
        return rc;
    }
    vol = uboot_fat_vol_get(id);
    if (vol == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /* 每次 mount 各有一份挂载体(它持有根 inode; vfs 的挂载表持它到关机) */
    fs = (uboot_fat_fs_t *)br_malloc(sizeof(*fs));
    if (fs == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    fs->vol  = vol;
    fs->root = BR_NULL;

    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        br_free(fs);
        return rc;
    }
    why[0] = '\0';
    rc = uboot_fat_mount_check(vol, why, sizeof(why));
    if (rc != 0) {
        br_log_error("uboot-fat: %s rejected: %s", cfg->path, why);
        uboot_fat_vol_unbind();
        br_free(fs);
        return rc;
    }

    /* 根 inode: br_malloc, 由本插件长期持有(vfs 永不 free_inode 它) */
    r = uboot_fat_node_new(id, BR_INODE_DIR, "", 0u);
    if (r == BR_NULL) {
        uboot_fat_vol_unbind();
        br_free(fs);
        return BR_ERR(ENOMEM);
    }
    fs->root = &r->head;

    /* ★ ADR-0030: 挂载体是**每次 mount 各造一份**, 所以在**这里**登记内部视图
     *   (sync / vol_info 的查询面)。从前这张表在 init 相预置, 现在由挂载事实填充。
     *   `cfg->path` 由 vfs 的挂载表持有(到关机), 生命周期够长。 */
    uboot_fat_strcpy_bounded(s_slots[id].mnt, cfg->path, (br_size_t)sizeof(s_slots[id].mnt));
    s_slots[id].fs      = fs;
    s_slots[id].mounted = BR_TRUE;

    /* ★ 就绪行的**形状是集成契约**: `tests/gates.toml` 的 `fat-test` 关要求逐卷点名
     *   `uboot-fat: <挂载点> 就绪`(与 `adapter/uboot-erofs` 的
     *   `uboot-erofs: /assets 就绪`、`adapter/uboot-mmc` 的
     *   `uboot-mmc: eMMC ready` 同款)。后面的括号是给人看的诊断, 不是判据。 */
    br_log_info("uboot-fat: %s 就绪 (dev=%s, FAT%d, %u sectors x %u B, cluster %u sectors, %u clusters)",
                cfg->path, vol->dev, vol->fatsize, vol->total_sect,
                (unsigned)vol->sect_size, vol->clust_size,
                (vol->clust_size != 0u) ? (vol->total_sect / vol->clust_size) : 0u);
    *root = fs->root;
    return 0;
}

static void uboot_fat_free_inode(br_inode_t *ino)
{
    if (ino != BR_NULL) {
        br_free(ino);                        /* 瞬态路径包装: 谁造谁释放 */
    }
}

/* 卷级 sync(= `br_file_ops.fsync` 的另一入口, 共用 uboot_fat_flush_volume)。 */
static int uboot_fat_sync(void *fs_priv)
{
    uboot_fat_fs_t *fs = (uboot_fat_fs_t *)fs_priv;

    if (fs == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    return uboot_fat_flush_volume(fs->vol);
}

/* ==================================================================== 自检需要的内部视图 */

/* 卷级 sync 的内部视图(自检 TC-FAT-008 的判据; fs_ops.sync 就是它) */
int uboot_fat_test_sync(const char *mnt)
{
    for (br_u32 i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
        if ((s_slots[i].mounted == BR_TRUE) && (s_slots[i].fs != BR_NULL) &&
            (uboot_fat_strcmp(s_slots[i].mnt, mnt) == 0)) {
            return uboot_fat_sync(s_slots[i].fs);
        }
    }
    return BR_ERR(ENOENT);
}

int uboot_fat_mount_vol_info(const char *mnt, uboot_fat_vol_info_t *out)
{
    br_u32 i;

    if ((mnt == BR_NULL) || (out == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    for (i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
        if ((s_slots[i].mounted == BR_TRUE) &&
            (uboot_fat_strcmp(s_slots[i].mnt, mnt) == 0)) {
            return uboot_fat_vol_info(i, out);
        }
    }
    return BR_ERR(ENOENT);
}

/* ==================================================================== 生命周期
 *
 * ADR-0030 之后只剩一件事: **start 相登记驱动**(`"fat"`)。挂载由 **APP** 发起 ——
 * 它按自己的 `[[mount]]` 计划调 POSIX `mount("emmcN", "/fatNN", "fat", …)`, 于是
 * `uboot_fat_mount()` 每次认领一个卷、造一份挂载体。
 * 顺序前提: 块设备由 `adapter/uboot-mmc` 在**它的** start 相注册, 而 APP 的 start 在
 * 全部非 APP start 之后(插件管理器的相位规则)⇒ 到这里设备一定已就绪。
 * EARLY/CORE 空手返回: early 相堆还没建池(br_mem_init 在 core.init), 而挂载要分配根 inode。
 */
int uboot_fat_early_init(void);
int uboot_fat_init(void);
int uboot_fat_start(void);

int uboot_fat_early_init(void)
{
    /* EARLY 相: 堆还没建池(br_mem_init 在 core.init), 本件此时不分配任何东西。 */
    return 0;
}

int uboot_fat_start(void)
{
    /* ★ ADR-0030: 只**登记驱动**, 不挂载 —— 挂哪块卡、挂到哪个挂载点, 由 APP 的
     *   挂载计划决定(每次 `mount("emmcN", "/fatNN", "fat", 0, NULL)` 认领一个卷)。
     *   从前这里按插件自己的三行表逐卷挂载 ⇒ "挂载计划"有两个真值(该欠债已由 ADR-0030 还清)。 */
    const int rc = br_fs_driver_register("fat", &s_fs_ops, BR_NULL);

    if (rc != 0) {
        br_log_error("uboot-fat: 注册驱动 fat 失败 rc=%d", rc);
        return rc;
    }
    return 0;
}

int uboot_fat_init(void)
{
    /* CORE 相: 无事可做。挂载点与源设备现在都由 **APP 的挂载计划**给出(ADR-0030),
     * 卷槽在 mount 时认领 —— 本件不再预置任何"三卷表"(那曾是挂载计划的第二个真值)。 */
    return 0;
}


