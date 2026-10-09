/*
 * brickOS prototype v0.2.0 — 设备文件系统(fs/devfs): 把 dev-core 注册表投影成 /dev 节点
 *
 * 设计依据: `docs/7-storage/7-03-concrete-fs.md` §3(devfs: "所有设备经 devfs 接入 VFS
 * 管理" —— Linux devtmpfs / rCore DeviceFS 同型)与 `docs/8-device/8-01-device.md`
 * §2/§3(D21/D23 的 open_file 钩子 + cdev-core 的通用会话适配)。
 * 逐条裁定见 `docs/decisions/0009-vfs-storage-stack.md`。
 *
 * ## 三条硬边界(这个插件的全部内容就是守住它们)
 *   ① **只投影, 不拥有设备**: 节点 = dev-core 注册表的**实时枚举**(注册即上线);
 *      devfs 没有"设备表", 也没有生命周期 API —— 设备的存在与否由驱动注册决定。
 *   ② **不认识任何子分类形状**(设计 SD-13/D21): 本文件**不 include** `br_cdev.h`,
 *      也不出现 `br_cdev_ops` —— 打开设备只经条目里的 `open_file` 钩子拿 {fops, fpriv}。
 *      这正是 D21 撤销 "vfs-core→dev-core" 之后留给 devfs 的位置。
 *   ③ **名字空间变更一律 -ENOTSUP**(设计 `7-03` §3: "unlink/mkdir → -ENOTSUP"):
 *      设备的上线/下线归驱动注册, 不归文件系统 —— 于是那些 iops 槽位**留空**(NULL),
 *      由 vfs-core 翻译成 -ENOTSUP(不需要在这里写一行"返回不支持")。
 *
 * ## inode 的形态(D23 的"根 inode = 注册表投影")
 *   根: 一个**静态**目录 inode(挂载表持有, 永不释放——`free_inode` 对它特判)。
 *   设备节点: `lookup` 时**现造**一个瞬态 inode(`br_malloc`), 携带 {fops, fpriv};
 *   打开它的会话由 cdev-core 的适配层建/销, devfs 只管把二元组塞进 inode。
 *   ⇒ 设备节点 inode 的 `free_inode` 是 `br_free`(与 tmpfs 的"节点即 inode"不同)。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>

#include <br/dev/br_dev.h>
#include <br/vfs/br_vfs.h>

/* 生命周期钩子原型(名 = symbol_prefix + 相; 生成物引用; 不进 [[export]]) */
int devfs_early_init(void);
int devfs_init(void);
int devfs_start(void);

/* 挂载点。★ 这个字符串与 `plugin.toml` 里的挂载计划是**两处真值** —— 原型还没有
 * "manifest → 挂载计划代码"的生成链路(与 `br-wa-mem-001` 的预算→region 同源),
 * 已登记 `WORKAROUND(br-wa-fs-001)`; 退出条件 = 生成器产出行, 本常量随之消失。 */
#define BR_DEVFS_MOUNT_PATH   "/dev"

/* ==================================================================== 小工具(无 libc) */

static br_bool devfs_str_eq(const char *a, const char *b)
{
    if (a == BR_NULL || b == BR_NULL) {
        return (a == b) ? BR_TRUE : BR_FALSE;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

static void devfs_str_copy(char *dst, const char *src, br_size_t cap)
{
    br_size_t i = 0u;
    for (; src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ==================================================================== inode 形态 */

/* 设备节点 inode 的公共头之后的私有尾: devfs 只需要"它投影的是哪个名字"。
 * (fops/fpriv 已经在公共头里 —— 那是 vfs 必须能读到的东西, 见 br_vfs.h 的说明。) */
typedef struct devfs_node {
    br_inode_t head;
    char       name[BR_DEV_NAME_MAX];
} devfs_node_t;

static br_inode_t s_root;   /* 根目录(挂载表持有; free_inode 对它特判) */

/* 前向声明: 两张 iops 表在下面定义, 但 `make_dev_node` 先用到它们(表里的 getattr 也在
 * 后面)。C 的单遍声明规则要求这里给一行 —— 比"把整个 inode 段落上下翻转"更少扰动。 */
static const br_inode_ops_t s_node_iops;
static const br_inode_ops_t s_root_iops;
static int devfs_getattr(br_inode_t *ino, br_stat_t *st);

/* 读取目录迭代游标(每次 open 一份, 放在 fpriv 里) */
typedef struct devfs_iter {
    br_u32 cursor;   /* 下一个要枚举的注册表下标(0 = 还没产出过设备) */
    br_u32 phase;    /* 0 = 还没给 "."; 1 = 还没给 ".."; 2 = 正在枚举设备 */
} devfs_iter_t;

/* ==================================================================== 目录面 */

static br_inode_t *make_dev_node(const char *name, const br_dev_class_entry_t *e)
{
    devfs_node_t *n = (devfs_node_t *)br_malloc((br_size_t)sizeof(devfs_node_t));
    if (n == BR_NULL) {
        return BR_NULL;
    }
    devfs_str_copy(n->name, name, (br_size_t)sizeof(n->name));

    n->head.type    = BR_INODE_FILE;
    /* ★ 设备节点**不是** iops = BR_NULL: `getattr/setattr` 属 inode 层(D23 的四层里
     *   inode 级管属性, file 级管读写), 而每个 inode 都需要属性面 —— Linux 的
     *   `i_op`/`f_op` 分工正是如此(文件 inode 的 i_op 只填 getattr/setattr)。
     *   给 BR_NULL 的后果实测过: `br_stat("/dev/uart0")` 得 -ENOTSUP(TC-IO-001 红),
     *   因为 vfs 的 br_stat 走 `iops->getattr`。
     *   名字空间变更(create/unlink/mkdir/rename)仍**全部留空** ⇒ 由 vfs 翻成 -ENOTSUP
     *   (设计 `7-03` §3: "unlink/mkdir → -ENOTSUP, 设备生命周期归驱动注册")。 */
    n->head.iops    = &s_node_iops;
    n->head.fops    = BR_NULL;          /* 由钩子给(没有文件面就是 NULL ⇒ open 得 -ENOTSUP) */
    n->head.fpriv   = BR_NULL;

    /* ★ 唯一的"设备知识"入口: 条目自带的钩子。
     * 钩子缺失 = 本子分类不提供文件面(v1 的 raw flash/bdev)⇒ 节点照样在, 打不开。 */
    if (e->open_file != BR_NULL) {
        const br_file_ops_t *fops = BR_NULL;
        void                *fpriv = BR_NULL;
        if (e->open_file(e->dev_priv, &fops, &fpriv) != BR_OK) {
            fops  = BR_NULL;            /* 钩子报错 ⇒ 退化成"看得见打不开", 不编造文件面 */
            fpriv = BR_NULL;
        }
        n->head.fops  = fops;
        n->head.fpriv = fpriv;
    }
    return &n->head;
}

static br_inode_t *devfs_lookup(br_inode_t *dir, const char *name)
{
    (void)dir;      /* devfs 只有一层: 任何 dir 都是根 */

    if (name == BR_NULL) {
        return BR_NULL;
    }
    if (devfs_str_eq(name, ".") == BR_TRUE || devfs_str_eq(name, "..") == BR_TRUE) {
        /* 根的上层还是根(设备树不挂到别处去)。返回根指针本身 —— 它是**非瞬态**的,
         * `free_inode` 会特判它, 所以调用方照常"谁拿谁放"也不会把它释放掉。 */
        return &s_root;
    }

    const br_dev_class_entry_t *e = br_dev_lookup(name);
    if (e == BR_NULL) {
        return BR_NULL;     /* 未注册 ⇒ 未命中(vfs 翻成 -ENOENT) */
    }
    return make_dev_node(name, e);
}

static int devfs_getattr(br_inode_t *ino, br_stat_t *st)
{
    if (ino == BR_NULL || st == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    st->valid = BR_STAT_SIZE;
    st->type  = ino->type;
    st->size  = 0u;         /* 设备节点没有"字节数"这回事(尺寸语义属会话, 不属节点) */
    return BR_OK;
}

/* 设备节点 inode 的 iops: **只有 getattr**(属性面); 名字空间变更全空。
 * 与根 iops 分开是刻意的 —— 两个对象的**能力面**不同, 合成一张表会让
 * "能否在设备节点上 mkdir" 这类问题在代码里看不出答案。 */
static const br_inode_ops_t s_node_iops = {
    .lookup  = BR_NULL,
    .create  = BR_NULL,
    .unlink  = BR_NULL,
    .mkdir   = BR_NULL,
    .rmdir   = BR_NULL,
    .rename  = BR_NULL,
    .getattr = devfs_getattr,
    .setattr = BR_NULL,     /* 设备节点的属性不可改(尺寸语义属会话, 不属节点) */
};

/* 根 inode 的 iops: **只有 lookup + getattr** —— mkdir/unlink/rmdir/rename/create 全部
 * 留空 ⇒ vfs 回 -ENOTSUP(设计 `7-03` §3 的原话:"unlink/mkdir → -ENOTSUP,
 * 设备生命周期归驱动注册")。 */
static const br_inode_ops_t s_root_iops = {
    .lookup  = devfs_lookup,
    .create  = BR_NULL,
    .unlink  = BR_NULL,
    .mkdir   = BR_NULL,
    .rmdir   = BR_NULL,
    .rename  = BR_NULL,
    .getattr = devfs_getattr,
    .setattr = BR_NULL,
};

/* ---- 根目录的"文件面" = 目录项流(设计 `7-01` §2: 目录 inode 的 file 面) ---- */

static int devfs_dir_open(br_file_t *f, br_u32 flags)
{
    (void)flags;
    devfs_iter_t *it = (devfs_iter_t *)br_malloc((br_size_t)sizeof(devfs_iter_t));
    if (it == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }
    it->cursor = 0u;
    it->phase  = 0u;
    br_file_set_fpriv(f, it);
    return BR_OK;
}

static br_s64 devfs_dir_read(br_file_t *f, void *buf, br_size_t n)
{
    if (buf == BR_NULL || n < (br_size_t)sizeof(br_dirent_t)) {
        return BR_ERR(BR_EINVAL);
    }
    devfs_iter_t *it = (devfs_iter_t *)br_file_fpriv(f);
    if (it == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    br_dirent_t *de = (br_dirent_t *)buf;
    de->name_len = 0u;

    if (it->phase == 0u) {
        it->phase = 1u;
        de->type = BR_INODE_DIR;
        devfs_str_copy(de->name, ".", (br_size_t)sizeof(de->name));
        de->name_len = 1u;
        return (br_s64)sizeof(*de);
    }
    if (it->phase == 1u) {
        it->phase = 2u;
        de->type = BR_INODE_DIR;
        devfs_str_copy(de->name, "..", (br_size_t)sizeof(de->name));
        de->name_len = 2u;
        return (br_s64)sizeof(*de);
    }

    /* 设备枚举:**每次调用都问一遍注册表**(实时投影, 设计 `7-03` §3)。
     * v1 无注销 ⇒ 表只增不减 ⇒ 用下标游标不会漏项也不会重项; 若将来有注销,
     * 这里必须改成"按名字续读"(届时注册表要给出稳定的迭代键)。
     * (ADR-0009 §5 的遗留项清单里还没有这一条 —— 它属于"设备注销 API 落地时"的伴生工作。) */
    const br_u32 total = br_dev_count();
    while (it->cursor < total) {
        const char *name = br_dev_name_at(it->cursor);
        it->cursor++;
        if (name == BR_NULL) {
            continue;
        }
        de->type = BR_INODE_FILE;
        devfs_str_copy(de->name, name, (br_size_t)sizeof(de->name));
        br_size_t len = 0u;
        while (de->name[len] != '\0') {
            len++;
        }
        de->name_len = (br_u32)len;
        return (br_s64)sizeof(*de);
    }
    return 0;       /* 穷尽 */
}

static int devfs_dir_close(br_file_t *f)
{
    devfs_iter_t *it = (devfs_iter_t *)br_file_fpriv(f);
    if (it != BR_NULL) {
        br_free(it);
        br_file_set_fpriv(f, BR_NULL);
    }
    return BR_OK;
}

static const br_file_ops_t s_root_fops = {
    .open        = devfs_dir_open,
    .read        = devfs_dir_read,
    .write       = BR_NULL,     /* 设备树只读(设备的上线走驱动注册) */
    .lseek       = BR_NULL,     /* 目录项流不支持定位 */
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = devfs_dir_close,
};

/* ==================================================================== 层 1: super */

static int devfs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    (void)fs_priv;
    if (cfg == BR_NULL || root == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (devfs_str_eq(cfg->path, BR_DEVFS_MOUNT_PATH) == BR_FALSE) {
        /* 挂载点与代码里的常量不符 ⇒ 拒挂。为什么不是"按 cfg 走":
         * 那会让 devfs 的节点前缀(它自己不关心)与用例/文档里的 /dev 漂移;
         * v1 的挂载点是编译期事实, 对不上就是接线错误(等生成链路落地后这条会自然消失)。 */
        br_log_error("devfs: 挂载点 %s 与预期 %s 不符", cfg->path, BR_DEVFS_MOUNT_PATH);
        return BR_ERR(BR_EINVAL);
    }

    s_root.type  = BR_INODE_DIR;
    s_root.iops  = &s_root_iops;
    s_root.fops  = &s_root_fops;
    s_root.fpriv = BR_NULL;
    *root = &s_root;
    return BR_OK;
}

static void devfs_free_inode(br_inode_t *ino)
{
    if (ino == BR_NULL || ino == &s_root) {
        return;     /* 根是静态对象(挂载表持有); lookup("."/"..") 也会把它交出去 */
    }
    br_free(ino);
}

static const br_fs_ops_t s_fs_ops = {
    .mount      = devfs_mount,
    .free_inode = devfs_free_inode,
    .unmount    = BR_NULL,      /* v2/v3(运行时卸载属 O-S3) */
    .sync       = BR_NULL,      /* 无持久化状态, 没有"落介质"这回事 */
};

/* ==================================================================== 生命周期 */

int devfs_early_init(void)
{
    return 0;       /* EARLY: 无线程无堆; 挂载在 CORE(那时堆已就绪) */
}

/*
 * CORE 相: 挂载 /dev。
 * 顺序前提(挂载计划, 设计 `7-03` §6): tmpfs 先挂 "/", 于是挂载点 /dev 由 vfs-core
 * 在 `br_mount_register` 里**自动 mkdir**(本插件不做这件事, 也不该做 —— 那是挂载表
 * 的职责)。这条前提由 manifest 的 init 边 `fs/devfs → fs/tmpfs` 保证(拓扑序)。
 */
int devfs_init(void)
{
    const int rc = br_mount_register(BR_DEVFS_MOUNT_PATH, &s_fs_ops, BR_NULL);
    if (rc != BR_OK) {
        br_log_error("devfs: 挂载 %s 失败 rc=%d", BR_DEVFS_MOUNT_PATH, rc);
        return rc;      /* 首败即停机由插件管理器执行(裁定 G6) */
    }
    br_log_info("devfs: %s mounted (%u device(s) visible so far)",
                BR_DEVFS_MOUNT_PATH, br_dev_count());
    return 0;
}

/* START 相: 设备注册全部完成后, 报一行"看得见几个设备"(启动证据, 不是判据)。 */
int devfs_start(void)
{
    br_log_info("devfs: %u node(s) in %s (registry is live, not a snapshot)",
                br_dev_count(), BR_DEVFS_MOUNT_PATH);
    return 0;
}
