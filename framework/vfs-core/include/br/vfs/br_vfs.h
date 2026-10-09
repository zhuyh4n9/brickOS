/*
 * brickOS prototype v0.2.0 — VFS 契约(framework/vfs-core 插件的对外面)
 *
 * 设计依据(设计仓库):
 *   - `7-01-vfs.md` §1(统一可打开模型 SD-1/D21: 一切 = `br_file_t`, `br_open` **只走挂载表**)
 *   - `7-01-vfs.md` §2(挂载与 ops 四层分层 SD-15/D23: super / inode / file / dentry 预留;
 *     **路径走查在 vfs-core**, inode v1 **瞬态**(有 inode ops, 无 cache —— SD-3))
 *   - `7-01-vfs.md` §3(poll 分期: v1 只查就绪位; `poll_attach` 槽位预留, NULL → -ENOTSUP)
 *   - `8-01-device.md` §2(dev-core 的 open_file 钩子返回 **{fops, fpriv}** —— 本头提供这两个类型)
 *   - `3-04-memory.md`(恒等映射 + 三池: `br_open` 经 core 堆分配 `br_file_t`)
 * 逐条裁定与**与设计的偏离**: `docs/decisions/0009-vfs-storage-stack.md`(本头是它的落地契约)。
 *
 * ============================== 契约归属 ==============================
 * 本头的全部 API 由 **framework/vfs-core 插件**实现 —— 不是 core。设计 `1-01` §4.5 / D19:
 * 能力框架以**插件形态**存在("框架件 = 插件的身份, core 的纪律"), 所以这里的 `br_*`
 * 前缀是**治理纪律**(API 面进 golden), 不是"core 本体"。
 * **vfs-core 是纯 VFS(D21)**: 零设备知识 —— 设备经 `fs/devfs` 以 /dev 节点接入。
 * =====================================================================
 *
 * ## 三层角色(谁实现什么)
 *   FS 插件(fs/tmpfs, fs/devfs, fs/littlefs…): 填 `br_fs_ops` + `br_inode_ops` + `br_file_ops`,
 *     用 `br_mount_register()` 挂载; **瞬态 inode 由 FS 创建**, 经 `free_inode` 释放。
 *   设备框架(framework/dev-core + framework/cdev-core): 提供 `open_file` 钩子, 返回
 *     {fops, fpriv} 二元组(D21/D23) —— devfs 把它们塞进设备节点 inode。
 *   消费者(APP / svc-posix / 服务): `br_open` + `br_file_*` + `br_opendir/br_readdir`
 *     + 路径级便捷面(`br_stat`/`br_mkdir`/…)。
 *
 * ## br_file_t 的私有数据(为什么不需要"私有尾")
 *   设计 `8-01` §2 的钩子返回 {fops, fpriv}: `fpriv` 随 **inode** 走(每 inode 一份),
 *   打开时被 vfs 交给 `br_file_t`; `fops->open()` **可以覆写**它(把会话指针放进去),
 *   `fops->close()` 负责释放自己放进去的东西 —— 与 Linux 的 `file->private_data` 同型。
 *   于是"每文件私有状态"有落点, 不必让 FS 报一个"私有尾大小"(sched 的 `tcb_size` 手法
 *   在这里没有必要)。逐条理由见 ADR-0009 §3 裁定 2。
 */
#ifndef BR_VFS_BR_VFS_H
#define BR_VFS_BR_VFS_H

#include <br/core/br_types.h>

/* ==================================================================== 打开标志 */
/* `br_open` 的 `flags`(append-only; 语义对齐 POSIX 的 O_*, 但**不复用其数值**)。 */
#define BR_O_RDONLY    0x0001u
#define BR_O_WRONLY    0x0002u
#define BR_O_RDWR      0x0003u
#define BR_O_ACCMODE   0x0003u   /* 访问模式掩码 */
#define BR_O_CREAT     0x0010u   /* 不存在则创建(只对文件; 目录用 br_mkdir) */
#define BR_O_EXCL      0x0020u   /* 与 O_CREAT 同用: 已存在 ⇒ -EEXIST */
#define BR_O_TRUNC     0x0040u   /* 打开即截断到 0(需写权限) */
#define BR_O_APPEND    0x0080u   /* 每次写前定位到末尾 */
#define BR_O_NONBLOCK  0x0100u   /* 不阻塞: 无数据 ⇒ -EAGAIN(设备类; v1 口径见 ADR-0009 §3) */

#define BR_O_WRITABLE(f)  (((f) & BR_O_ACCMODE) != BR_O_RDONLY)

/* ==================================================================== lseek */
#define BR_SEEK_SET    0
#define BR_SEEK_CUR    1
#define BR_SEEK_END    2

/* ==================================================================== inode 类型 */
#define BR_INODE_NONE  0u
#define BR_INODE_DIR   1u
#define BR_INODE_FILE  2u

/* ==================================================================== poll 事件位 */
#define BR_POLLIN      0x01u
#define BR_POLLOUT     0x02u
#define BR_POLLERR     0x04u

/* 目录项/设备名的静态上界(devfs 节点名 = 路径分量, 设计 `7-01` §1: `[a-z][a-z0-9]*`;
 * 对 tmpfs 的文件名它是**静态上界** —— 超长 ⇒ -ENAMETOOLONG 的替代口径 = -ENOSPC, 见 ADR-0009)。 */
#define BR_NAME_MAX    32u

/* ==================================================================== 前向声明 */
typedef struct br_file       br_file_t;       /* 不透明打开句柄(D14) */
typedef struct br_inode      br_inode_t;      /* 瞬态走查产物: 公共头 + FS 私有尾(CA-2) */
typedef struct br_dir        br_dir_t;        /* 目录迭代句柄 */
typedef struct br_file_ops   br_file_ops_t;
typedef struct br_inode_ops  br_inode_ops_t;
typedef struct br_dentry_ops br_dentry_ops_t; /* v1 全 NULL(D23 预留) */
typedef struct br_fs_ops     br_fs_ops_t;

/* ==================================================================== stat / dirent */

/* 属性(设计 `7-01` §2 的 getattr/setattr 载体)。v1 **只实现 size**(截断);
 * 其余字段与 valid 位按 append-only 预留 —— 有 valid 掩码, 才能诚实表达
 * "这次 setattr 想改哪些字段", 而不是靠哨兵值猜。 */
#define BR_STAT_SIZE   0x0001u

typedef struct br_stat {
    br_u32 valid;   /* BR_STAT_* 位: 本次调用/本次回答里**有效**的字段 */
    br_u32 type;    /* BR_INODE_DIR / BR_INODE_FILE */
    br_u64 size;    /* 字节数(目录 = 0) */
} br_stat_t;

/* 目录项(design `7-01` §2: 目录 inode 的 file 面 read 产出)。定长记录 ——
 * `read()` 一次填一条并返回 `sizeof(br_dirent_t)`, 穷尽返回 0(`7-01` §2 的迭代模型)。 */
typedef struct br_dirent {
    br_u32 type;                /* BR_INODE_DIR / BR_INODE_FILE */
    br_u32 name_len;            /* 不含结尾 '\0' */
    char   name[BR_NAME_MAX];
} br_dirent_t;

/* ==================================================================== 层 3: file */

/*
 * 文件面 ops(设计 `7-01` §1/§2 的层 3)。**不支持的槽位 = BR_NULL ⇒ 调用方得 -ENOTSUP**。
 * `open` 在会话建立时调用(D23: 自 fs 级移入 file 级); `close` 与之配对, 负责释放
 * `open` 经 `br_file_set_fpriv()` 放进去的东西。
 *
 * 目录也有一份 `br_file_ops`(其 `read` 产出 `br_dirent_t`)—— 于是 `br_opendir/br_readdir`
 * 在 vfs-core 里是**纯泛型**代码, 不需要任何"目录专用 ops"(D23 的分层收益)。
 */
struct br_file_ops {
    int     (*open) (br_file_t *f, br_u32 flags);
    br_s64  (*read) (br_file_t *f, void *buf, br_size_t n);
    br_s64  (*write)(br_file_t *f, const void *buf, br_size_t n);
    br_s64  (*lseek)(br_file_t *f, br_s64 off, int whence);
    int     (*ioctl)(br_file_t *f, br_u32 cmd, void *arg);
    int     (*fsync)(br_file_t *f);
    int     (*poll) (br_file_t *f, br_u32 *events);
    int     (*poll_attach)(br_file_t *f, void *wait);   /* v2 预留(wait-queue); v1 恒 BR_NULL */
    int     (*close)(br_file_t *f);
};

/* ---- 打开句柄的访问器(fops 实现者用; `br_file_t` 本身不透明) ----
 * 存在的理由: `br_file_ops` 的槽位只拿到 `br_file_t*`, 而实现需要知道
 * "我在哪个 inode 上 / 我的私有数据 / 当前偏移 / 打开 flags" 四件事。
 * vfs-core 独占结构布局(D14) ⇒ 只经访问器读写, 插件不许碰字段。 */
br_inode_t *br_file_inode(br_file_t *f);
void       *br_file_fpriv(br_file_t *f);
void        br_file_set_fpriv(br_file_t *f, void *p);
br_u32      br_file_flags(br_file_t *f);
br_s64      br_file_offset(br_file_t *f);
void        br_file_set_offset(br_file_t *f, br_s64 off);

/* ==================================================================== 层 2: inode */

/*
 * inode **公共头**(设计 `7-01` §2 的对象模型; FS 私有尾紧跟其后 —— CA-2 同型)。
 *
 * ★ 与设计原样的**唯一差别**: 多了 `fpriv` 字段(设计写"文件 inode 携带 {fops, fpriv}
 *   二元组", 但只把 `fops` 放进了公共头)。理由: vfs-core 必须在 `open` 时把 `fpriv`
 *   **交给**文件句柄, 而它不许知道任何 FS 的私有尾布局; 少了这个字段, 每个 FS 都得再补
 *   一个"取 fpriv"的 op(即又一处接口面)。登记为 ADR-0009 §3 裁定 2 的增量。
 *
 * ★ `type` 是**语义唯一真值**, 不是 `iops == BR_NULL`:
 *   vfs-core 判"是不是目录"一律看 `type`(见 src/br_vfs.c 的 walk_to)。原因是各 FS 对
 *   `iops` 的填法不同 —— 例如 fs/tmpfs 让**所有**节点共用一张带 lookup 的表(槽位里再按
 *   类型自校验), 而 fs/devfs 只给目录填。若 vfs 靠 `iops == NULL` 判目录, 同一个"文件
 *   当目录走查"的调用在两种 FS 下会得到不同 errno(-ENOENT vs -ENOTDIR)。
 *   所以 `iops == BR_NULL` 只表示"这个名字空间操作集不存在", 不表示"不是目录"。
 *
 * ★ `getattr`/`setattr` 属**本层**(inode 级), 不是 file 级 —— 于是**每个**支持 `br_stat`
 *   的 inode 都必须带一张 iops(哪怕只填 getattr): 设备文件节点就是这样(devfs 给它一张
 *   只有 getattr 的表, 名字空间变更槽位全空)。给 `iops = BR_NULL` 的节点 `br_stat` 会得
 *   `-ENOTSUP` —— 这是 Linux `i_op`/`f_op` 分工的直接后果, 不是限制。
 */
struct br_inode {
    br_u32                type;    /* BR_INODE_DIR / BR_INODE_FILE */
    const br_inode_ops_t *iops;    /* 名字空间操作集: 目录必填, 非目录可为 BR_NULL */
    const br_file_ops_t  *fops;    /* 文件面(目录也有一份: read = 枚举目录项); 可空 */
    void                 *fpriv;   /* 文件面私有: FS 节点 / 设备条目; open 时交给 br_file_t */
};

/* inode 私有尾入口(FS 用 `struct { br_inode_t head; ... }` 时的便捷取址) */
void *br_inode_priv(br_inode_t *ino);

/*
 * 目录 inode 的操作集(设计 `7-01` §2 的层 2)。**路径走查(逐级下推/名字比较)在 vfs-core**;
 * 本表只回答"这个名字在我这层是什么"。
 * `lookup` 返回**新建的瞬态 inode**, 未命中返回 `BR_NULL`(**不是**错误码 —— 走查要靠它
 * 区分"不存在"与"失败"); 其余槽位返回负 errno。
 * `symlink`/`readlink`/`mknod` 属 v2(槽位未占; 到 v2 再 append)。
 */
struct br_inode_ops {
    br_inode_t *(*lookup)(br_inode_t *dir, const char *name);
    int  (*create) (br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out);
    int  (*unlink)(br_inode_t *dir, const char *name);
    int  (*mkdir)(br_inode_t *dir, const char *name);
    int  (*rmdir)(br_inode_t *dir, const char *name);
    int  (*rename)(br_inode_t *dir, const char *name, br_inode_t *ndir, const char *nname);
    int  (*getattr)(br_inode_t *ino, br_stat_t *st);
    int  (*setattr)(br_inode_t *ino, const br_stat_t *st);   /* v1 只认 BR_STAT_SIZE(截断) */
};

/* ==================================================================== 层 4: dentry */
/* v1 **全 BR_NULL**(SD-3: 无 dcache); 槽位先占免得 v2 改布局(D14/D23)。 */
struct br_dentry_ops {
    int (*revalidate)(void *dentry);
    int (*compare)(void *dentry, const char *name);
};

/* ==================================================================== 层 1: super */

/* 挂载配置(设计 `7-01` §2 的 `br_fs_cfg_t`)。`dev`: 介质类 FS(littlefs/EROFS)的设备名
 * (来自 manifest 挂载计划); tmpfs/devfs = BR_NULL。`opts`: FS 自有配置(预留, v1 恒 BR_NULL)。 */
typedef struct br_fs_cfg {
    const char *path;
    const char *dev;
    const void *opts;
} br_fs_cfg_t;

/*
 * FS 级 ops(层 1)。`mount` 在**挂载注册时**调用一次, 产出根 inode(挂载表持有, 不属瞬态);
 * `free_inode` 释放走查产出的**瞬态** inode(挂载表持有的根 inode 不会被传进来 —— 见
 * `br_mount_register` 的契约)。
 * `unmount`/`sync` 是 v1 的**签名先行**槽位(运行时挂载 = O-S3, 不早于 v3)⇒ 可 BR_NULL。
 */
struct br_fs_ops {
    int  (*mount)(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
    void (*free_inode)(br_inode_t *ino);
    int  (*unmount)(void *fs_priv);
    int  (*sync)(void *fs_priv);
};

/*
 * 挂载一个 FS(设计 `7-01` §2 的签名)。
 *   `path` 必须是绝对路径且**已被规范化**(无尾 '/', 除根外无连续 '/'): `/`、`/dev`、`/data`。
 *   语义: 建 cfg{path} → `ops->mount(fs_priv, cfg, &root)` → 槽位记录 {path, ops, fs_priv, root};
 *         若 `path != "/"`, 在**父挂载**里自动 mkdir 挂载点(设计 `7-03` §6: "挂载点在父 FS
 *         缺失时自动 mkdir", 中间分量逐级建; 已存在则忽略)。
 *   错误: `-EINVAL`(path/ops 非法或未规范化)/ `-EEXIST`(同一 path 重复挂载)/
 *         `-ENOSPC`(挂载表满)/ `-ENODEV`(父挂载不存在)/ 其余 = mount 的返回值原样上传。
 */
int br_mount_register(const char *path, const br_fs_ops_t *ops, void *fs_priv);

/* 挂载表观测(诊断/dump; append-only 增量, 与按名查找同型)。 */
br_u32 br_mount_count(void);
const char *br_mount_path_at(br_u32 index);          /* 越界 ⇒ BR_NULL */

/* ==================================================================== 统一入口 */

/*
 * 打开(设计 `7-01` §1 的 SD-1: **只走挂载表**, 最长前缀匹配)。全流程:
 *   规范化路径 → 最长前缀命中挂载 → 根 inode → 逐级 `iops->lookup` → 末级 inode
 *   → `O_CREAT` 且未命中 ⇒ 在父目录 `iops->create` → 分配 `br_file_t`(core 堆)
 *   → `fops->open(f, flags)` 建会话。
 * 返回句柄, 失败返回 `BR_NULL` 并**不设 errno**(原型口径: 返回 NULL; 需要错误码的调用方
 * 用下面的 `br_open_err()` 变体 —— 这是 svc-posix 把 errno 映射回用户态的唯一入口)。
 *
 * `br_open_err(path, flags, int *err)` 是同一个实现: `err` 可空; 成功写 0。
 */
br_file_t *br_open(const char *path, br_u32 flags);
br_file_t *br_open_err(const char *path, br_u32 flags, int *err);

/* 文件面(层 3 的派发; `f == BR_NULL` ⇒ -EINVAL; 槽位缺失 ⇒ -ENOTSUP)。 */
br_s64 br_file_read (br_file_t *f, void *buf, br_size_t n);
br_s64 br_file_write(br_file_t *f, const void *buf, br_size_t n);
br_s64 br_file_lseek(br_file_t *f, br_s64 off, int whence);
int    br_file_ioctl(br_file_t *f, br_u32 cmd, void *arg);
int    br_file_fsync(br_file_t *f);
int    br_file_poll (br_file_t *f, br_u32 *events);   /* v1: 查询就绪位(SD-7) */
int    br_file_close(br_file_t *f);                   /* 释放会话 + 句柄; 幂等(NULL ⇒ 0) */

/* 路径级便捷面(svc-posix 1:1 映射; 全部走"父目录 inode + 名字"两步)。 */
int br_stat    (const char *path, br_stat_t *st);
int br_mkdir   (const char *path);
int br_rmdir   (const char *path);
int br_unlink  (const char *path);
int br_rename  (const char *old_path, const char *new_path);
int br_truncate(const char *path, br_u64 size);

/* ==================================================================== 目录迭代 */

/*
 * 目录迭代(设计 `7-01` §2: "目录 inode 的 file 面(opendir → br_dir_t 迭代)")。
 * `br_opendir` = 对目录 inode 做一次 `O_RDONLY` 打开; `br_readdir` 在 vfs-core 里
 * 泛型地调 `fops->read` 取一条 `br_dirent_t`。
 * 返回: `1` = 取到一条(写满 `out`); `0` = 目录已穷尽; 负数 = errno。
 */
br_dir_t *br_opendir(const char *path);
int       br_readdir(br_dir_t *d, br_dirent_t *out);
int       br_closedir(br_dir_t *d);

/* ==================================================================== 生命周期 */

/* 本插件的生命周期钩子由生成物引用(见 plugin.toml): `vfs_core_early_init/init/start`。
 * `start` 相跑本域的一致性用例(APP 的 start 里也会调一次 `br_vfs_conformance()`; 见 ADR-0009 §3)。 */

/*
 * 一致性用例(自编号 `TC-VFS-*`; 6-01 的用例表里没有这一组 ⇒ 登记在 `br-wa-test-001`)。
 * 打印 `[VFSCONF] PASS/FAIL <tag> <desc>` 并以 `[VFSCONF] SUMMARY pass=N fail=M` 收尾;
 * 返回失败数。**纯泛型**: 它只经本头的公开 API 驱动(挂载表 + 路径走查 + 文件/目录面),
 * 因此对"rootfs 是 tmpfs"这个事实只当作"挂载表里有一个 '/'", 不认任何 FS 名字。
 */
int br_vfs_conformance(void);

#endif /* BR_VFS_BR_VFS_H */
