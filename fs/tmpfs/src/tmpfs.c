/*
 * fs/tmpfs — brickOS 的 RAM rootfs(实现)
 *
 * 设计出处:
 *   - `7-03-concrete-fs.md` §2: tmpfs = RAM 文件系统, 节点与数据均出自 core 堆
 *     (`br_malloc`), 完整文件语义(read/write/lseek/truncate/unlink/rename/mkdir/stat),
 *     挂载为 rootfs("/"), /dev /data /tmp 由预建目录列表生成;
 *   - `7-03-concrete-fs.md` §6: 挂载顺序第 1 条 = fs/tmpfs → "/"(预建三个目录);
 *   - `7-01-vfs.md` §2: 四层 ops(super/inode/file/dentry), **路径走查在 vfs-core**;
 *   - 契约头 `framework/vfs-core/include/br/vfs/br_vfs.h`(本文件的唯一权威)。
 *
 * ============================== 本插件是什么 ==============================
 * tmpfs 是**管理类 FS**(D21): 没有介质、没有块层, 节点与文件数据全部来自 core 堆。
 * 它承担两件事:
 *   ① rootfs 兜底 —— 一切没匹配到更长挂载点的绝对路径落到这里(单路由的最后一跳);
 *   ② 命名空间骨架 —— 无介质产品也能拥有 /、/dev、/data、/tmp 这套完整命名空间。
 * 掉电不保持是**角色分工**而不是缺陷: 数据落介质归 littlefs/EROFS。
 * ========================================================================
 *
 * ★ 与"瞬态 inode"模型的差别(读懂本文件最要紧的一件事)
 *   设计 SD-3 说 inode 是走查的中间产物、`free_inode` 即弃 —— 那是 littlefs 的形状
 *   (节点 = 路径前缀包装, 走查一次就得重建一次)。tmpfs 反过来: **节点是 br_malloc 出来
 *   的持久对象**, 挂在父目录的 child 单链上, 所以
 *     - `lookup()` 直接交回既有节点, **不做任何分配**(走查因此是零分配的);
 *     - `free_inode` 必须是 no-op: vfs 只把瞬态 inode 交给它, 而 tmpfs 没有瞬态 inode;
 *       顺手 br_free 一下就会把活着的目录树撕掉(见 tmpfs_free_inode)。
 *   节点指针持久化在 `head.fpriv` —— 公共头里唯一的"私有"落点: vfs 在 open 时把它交给
 *   `br_file_t`(br_vfs.c 的 `f->fpriv = ino->fpriv`), 文件面靠它找回节点。
 *
 * ★ 已知欠债(v0.2 原型口径)
 *   配额(数据 128 KiB / 节点 48)是**写死的静态上界**, 不是 manifest 预算生成的 ——
 *   与 platform/qemu-aarch64/src/memmap.c 里写死的池比例同源(那里登记为 br-wa-mem-001)。
 *   退出条件: **manifest budget → tmpfs quota**(组合器把 [budget] 变成 FS 配额描述符,
 *   本插件在 mount 时读取而不是编译期常量)。在那之前, 这两个常量取"够跑完 VFS 一致性
 *   用例、又一定不会把 1 MiB 堆吃穿"的值 —— 是**安全上界**, 不是预算真值。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

/* =====================================================================
 * 生命周期钩子(EARLY / CORE / START)
 *
 * 名字 = symbol_prefix(short) + 相, 由生成物 build/gen/fs/tmpfs/plugin_desc.c 引用
 * (设计 1-01 §9 / 3-05 §2)。本插件**不导出任何 API**: 它的行为只经 VFS 契约被泛型地
 * 驱动(见 vfs-core 的 br_vfs_conformance), 所以除这三个钩子外本文件其余函数全是 static;
 * 自带原型满足 -Wmissing-prototypes(钩子名由生成器推导, 不进任何对外头)。
 * ===================================================================== */
int tmpfs_early_init(void);
int tmpfs_init(void);
int tmpfs_start(void);

/* =====================================================================
 * 配额与名字规则(静态上界; 见文件头的欠债说明)
 * ===================================================================== */

/* 全部文件**容量**之和的上界(记账口径 = cap, 见 s_fs 的注释)。 */
#define TMPFS_DATA_MAX   (128u * 1024u)

/* 节点数上界(root + 预建三个目录 + 用例余量)。48 × ~104 B ≈ 5 KiB, 相对 1 MiB 堆可忽略。 */
#define TMPFS_NODE_MAX   48u

/* 首次写入时的最小容量。既不该为 1 字节写去要一个块, 也不该每次 +1 字节地 realloc
 * (那会把顺序追加写成 O(n²) —— 而追加写正是 tmpfs 最典型的形状)。 */
#define TMPFS_MIN_CAP    64u

/*
 * ★ 挂载点是**代码常量**。设计 `7-03` §6 的挂载计划(plugin.toml 的 `[[mount]]`)目前只是
 * 人读的声明面、工具侧还不消费它 ⇒ "声明面 + 本常量"两处真值。已登记
 * `WORKAROUND(br-wa-fs-001)`; 退出条件 = 组合器按挂载计划产出行, 本常量随之消失。
 * (与 fs/devfs 的 BR_DEVFS_MOUNT_PATH 同型; 但 tmpfs **不**按它拒挂 —— 见 tmpfs_mount。)
 */
#define TMPFS_MOUNT_PATH "/"

/* 这个断言是"cap 用 br_u32"的前提: 配额与偏移算术都在 2 GiB 以内 ⇒ 不会绕回。 */
_Static_assert(TMPFS_DATA_MAX <= 0x7fffffffu, "配额必须让 cap(br_u32)与偏移算术都安全");

/* =====================================================================
 * 节点模型(公共头 + 私有尾; CA-2)
 * ===================================================================== */

typedef struct tmpfs_node tmpfs_node_t;

/*
 * `head` 必须首字段 —— CA-2 的全部要求就是"vfs 只认 br_inode_t*, 拿到手就能当节点用"。
 * 反过来取节点走 `head.fpriv`(见 tmpfs_ino_node): fpriv 是"交给文件面"的契约载体,
 * 用它比调 br_inode_priv() 少一个跨插件符号, 两者在首字段布局下指向同一处。
 */
struct tmpfs_node {
    br_inode_t    head;                 /* 公共头(type / iops / fops / fpriv) */
    tmpfs_node_t *parent;               /* 父目录; 根自指(root->parent == root) */
    tmpfs_node_t *child;                /* 子链头(目录用); 文件恒 BR_NULL */
    tmpfs_node_t *next;                 /* 兄弟单链 */
    br_u8        *data;                 /* 文件数据(文件用); 实占 = cap */
    br_u64        size;                 /* 文件字节数; 目录恒 0 */
    br_u32        cap;                  /* data 容量(字节) —— 同时是配额记账口径 */
    char          name[BR_NAME_MAX];    /* 目录项名(根的名字是 "/", 只作诊断, 不参与比较) */
};

/* 节点私有尾的尺寸预算(不是优化目标, 是"48 个节点别把堆吃穿"的上界证据) */
_Static_assert(sizeof(tmpfs_node_t) <= 128u, "节点必须留在 128 B 以内");

/*
 * 目录迭代器: 目录的 open() 分配、close() 释放, 挂在**文件句柄**的 fpriv 上 ——
 * 不动节点 head.fpriv(那是节点身份, 不是会话状态; 一个目录可以同时被多次打开迭代)。
 *
 * 生命周期边界(v1 的已知口子, 不是本文件特有的): 迭代器只**借用**节点。若在迭代中途
 * rmdir 掉该目录, 节点会被释放, 之后的 read 就是 use-after-free。v1 **无引用计数**
 * (br_vfs.h 明说: 单 APP 无 fork, 引用计数归 v2), 所以"边删边读"是调用方的错用;
 * 这里不做防御(记一笔, 免得后来人以为它被保护了)。
 */
typedef struct {
    tmpfs_node_t *dir;      /* 被枚举的目录(借用, 不拥有) */
    br_u32        cursor;   /* 0 = ".", 1 = "..", ≥2 = 第 (cursor-2) 个子节点 */
} tmpfs_dir_iter_t;

/*
 * 记账体。为什么是**文件域静态**而不是从 fs_priv 传进来: inode/file ops 的签名里都没有
 * fs_priv(契约如此), 它们只能看见节点 ⇒ 账只能挂在插件静态上。mount 拿到的 fs_priv
 * 必须就是这一个(自洽检查见 tmpfs_mount)。
 *
 * 记账口径 = Σ node->cap(**容量**), 不是 Σ size: 占堆的是 cap; 若按 size 记账, 几何增长
 * 多要出来的容量就成了"看不见的越额", 而上界的语义恰恰是"从堆里拿了多少"。代价是
 * "文件长度之和"可以远小于 data_used —— 这是如实记账, 不是泄漏: truncate/unlink 都会
 * 把 cap 交还(见 tmpfs_file_release / tmpfs_file_truncate)。
 */
typedef struct {
    br_u32 nodes_used;   /* 已分配节点数(root 在内) */
    br_u32 data_used;    /* 已申请容量之和; 上界 TMPFS_DATA_MAX */
} tmpfs_fs_t;

static tmpfs_fs_t s_fs;

/* =====================================================================
 * 前置声明
 *
 * 顺序说明: ops 表 = 本插件的**契约面**, 放在函数体之前, 于是文件自上而下读作
 * "契约(表) → 实现(函数)"; 表要引用函数名, 函数就得先有声明。
 * ===================================================================== */

static br_inode_t *tmpfs_lookup (br_inode_t *dir, const char *name);
static int  tmpfs_create (br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out);
static int  tmpfs_unlink (br_inode_t *dir, const char *name);
static int  tmpfs_mkdir  (br_inode_t *dir, const char *name);
static int  tmpfs_rmdir  (br_inode_t *dir, const char *name);
static int  tmpfs_rename (br_inode_t *dir, const char *name, br_inode_t *ndir, const char *nname);
static int  tmpfs_getattr(br_inode_t *ino, br_stat_t *st);
static int  tmpfs_setattr(br_inode_t *ino, const br_stat_t *st);

static int   tmpfs_file_open (br_file_t *f, br_u32 flags);
static br_s64 tmpfs_file_read (br_file_t *f, void *buf, br_size_t n);
static br_s64 tmpfs_file_write(br_file_t *f, const void *buf, br_size_t n);
static br_s64 tmpfs_file_lseek(br_file_t *f, br_s64 off, int whence);
static int   tmpfs_file_close(br_file_t *f);

static int   tmpfs_dir_open (br_file_t *f, br_u32 flags);
static br_s64 tmpfs_dir_read (br_file_t *f, void *buf, br_size_t n);
static int   tmpfs_dir_close(br_file_t *f);

static int  tmpfs_mount     (void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
static void tmpfs_free_inode(br_inode_t *ino);

/* =====================================================================
 * ops 表(层 1 / 2 / 3)
 * ===================================================================== */

/*
 * 层 2: inode ops。★ 两种节点**共用**一张表(每个槽位自判类型) —— 与 br_vfs.h 里
 * "iops: 目录 inode 的操作集; 非目录 = BR_NULL"的注释不同。理由: vfs 只对目录派发
 * inode ops(走查/名字空间变更都是"父目录 + 名字"), 于是共用一张表的收益是"类型守卫集中
 * 在一处、表只有一份", 代价只是文件节点多带一个非空指针(≤48 个节点)。
 */
static const br_inode_ops_t s_iops = {
    .lookup  = tmpfs_lookup,
    .create  = tmpfs_create,
    .unlink  = tmpfs_unlink,
    .mkdir   = tmpfs_mkdir,
    .rmdir   = tmpfs_rmdir,
    .rename  = tmpfs_rename,
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
};

/* 层 3: 文件面。不支持的槽位**显式** BR_NULL ⇒ vfs 派发时得 -ENOTSUP(契约如此,
 * 不是"忘了填"的缺省值)。 */
static const br_file_ops_t s_file_fops = {
    .open        = tmpfs_file_open,
    .read        = tmpfs_file_read,
    .write       = tmpfs_file_write,
    .lseek       = tmpfs_file_lseek,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,   /* 数据只在堆里, 没有"落介质"这个动作可做 */
    .poll        = BR_NULL,   /* 内存文件恒可读写; 等待面归 v2(SD-7) */
    .poll_attach = BR_NULL,   /* v2 预留(wait-queue), v1 恒 NULL */
    .close       = tmpfs_file_close,
};

/* 目录也有"文件面"(read = 枚举目录项) —— D23 的分层让 opendir/readdir 在 vfs-core 里
 * 是**纯泛型**代码, 不需要任何"目录专用 ops"。写/定位对目录无语义, 槽位留空。 */
static const br_file_ops_t s_dir_fops = {
    .open        = tmpfs_dir_open,
    .read        = tmpfs_dir_read,
    .write       = BR_NULL,
    .lseek       = BR_NULL,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = tmpfs_dir_close,
};

/* 层 1: super。unmount/sync 是 v1 的**签名先行**槽位(运行时挂载/卸载不早于 v3, O-S3);
 * tmpfs 也没有"卷同步"这件事(没有卷)。 */
static const br_fs_ops_t s_fs_ops = {
    .mount      = tmpfs_mount,
    .free_inode = tmpfs_free_inode,
    .unmount    = BR_NULL,
    .sync       = BR_NULL,
};

/* =====================================================================
 * 小工具(freestanding: 不 include <string.h>, 这里是全部的"字符串库")
 * ===================================================================== */

static int tmpfs_str_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

static br_u32 tmpfs_str_len(const char *s)
{
    br_u32 n = 0u;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* 有界复制(cap **含**结尾 '\0')。调用点两处(节点名 / 目录项名)的源都已被
 * tmpfs_name_ok 保证 ≤ BR_NAME_MAX-1, 所以截断分支是纯防御(不进正常路径)。 */
static void tmpfs_str_copy(char *dst, const char *src, br_u32 cap)
{
    br_u32 i = 0u;

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
 * 块搬运/清零: 直接用编译器内建, 不 include <string.h>。freestanding 下 GCC 本来就会把
 * "整块搬移 / 整块清零"(结构体赋值、可识别的循环模式)降成对 memcpy/memset 的调用, 而这两
 * 个符号由 core/src/string.c 提供并链进镜像(那是本原型能否链接成立的前提) ⇒ 借道内建既
 * 不需要头文件, 也不需要自己再写一份 —— 自己写反而要面对"被降成 memcpy"的递归坑
 * (core/src/string.c 的注释记录了那次实测)。
 */
static void tmpfs_copy(void *dst, const void *src, br_size_t n)
{
    (void)__builtin_memcpy(dst, src, n);
}

static void tmpfs_zero(void *dst, br_size_t n)
{
    (void)__builtin_memset(dst, 0, n);
}

/*
 * 名字规则(**唯一**执法点; create/mkdir/rename 都走它):
 *   非空、不含 '/'、不是 "." / ".."、长度 ≤ BR_NAME_MAX-1。
 *
 * 为什么长度判定写成"逐字节扫到 NUL 或扫满 BR_NAME_MAX"而不是先 tmpfs_str_len: 名字来自
 * 调用方(vfs/svc-posix), 未终止的串在这里也**只读 BR_NAME_MAX 字节** —— 越界读是比"返回
 * -EINVAL"严重得多的错。'.'/'..' 只对**变更用**的名字拒绝; 走查(lookup)在更前面就地解析
 * 它们(见 tmpfs_lookup), 所以 "/." 与 "/.." 是合法的走查路径。
 */
static br_bool tmpfs_name_ok(const char *name)
{
    br_u32 i;

    if (name == BR_NULL) {
        return BR_FALSE;
    }
    if (name[0] == '\0') {
        return BR_FALSE;
    }
    if ((name[0] == '.') &&
        ((name[1] == '\0') || ((name[1] == '.') && (name[2] == '\0')))) {
        return BR_FALSE;
    }
    for (i = 0u; i < BR_NAME_MAX; i++) {
        if (name[i] == '\0') {
            return BR_TRUE;                  /* 长度 = i ≤ BR_NAME_MAX-1 */
        }
        if (name[i] == '/') {
            return BR_FALSE;
        }
    }
    return BR_FALSE;                         /* 扫满窗口仍无 NUL ⇒ 超长 */
}

/* 节点身份: 用 head.fpriv(节点建立时 = 自己), 不用 br_inode_priv()。两者在"head 首字段"
 * 的布局下指向同一处, 但 fpriv 是 vfs 交给文件面的契约载体, 且不引入额外的跨插件符号。
 * 注意: fops->open() 覆写的是 **br_file_t 的** fpriv(目录迭代器), 不是这里的。 */
static tmpfs_node_t *tmpfs_ino_node(const br_inode_t *ino)
{
    return (tmpfs_node_t *)ino->fpriv;
}

/* 文件面: 会话的 fpriv 就是节点(文件 open 不覆写它, 目录 open 才换成迭代器) */
static tmpfs_node_t *tmpfs_session_node(br_file_t *f)
{
    return (tmpfs_node_t *)br_file_fpriv(f);
}

/*
 * 文件面槽位的类型守卫。vfs 按节点类型派发表, 走到这里本不该类型不符; 但"永远不崩"
 * 比"相信上层"划算 —— 一个被踩坏的 inode 应该得到 -EISDIR(对目录做文件操作), 而不是
 * 让内核在解引用里翻车。
 */
static int tmpfs_require_file(const tmpfs_node_t *n)
{
    if (n == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    return (n->head.type == BR_INODE_FILE) ? BR_OK : BR_ERR(BR_EISDIR);
}

/* 子链查找/摘链。线性即可: 目录项上界 = TMPFS_NODE_MAX = 48, 没有做哈希/排序的理由。 */
static tmpfs_node_t *tmpfs_child_find(tmpfs_node_t *dir, const char *name)
{
    tmpfs_node_t *c;

    for (c = dir->child; c != BR_NULL; c = c->next) {
        if (tmpfs_str_eq(c->name, name)) {
            return c;
        }
    }
    return BR_NULL;
}

static void tmpfs_child_unlink(tmpfs_node_t *dir, tmpfs_node_t *victim)
{
    tmpfs_node_t **pp;

    for (pp = &dir->child; *pp != BR_NULL; pp = &(*pp)->next) {
        if (*pp == victim) {
            *pp = victim->next;
            victim->next = BR_NULL;
            return;
        }
    }
}

/* 初始化一个节点(不改账: 记账归调用方)。所有节点共用 s_iops, 只有文件面按类型分。 */
static void tmpfs_node_init(tmpfs_node_t *n, br_u32 type, tmpfs_node_t *parent, const char *name)
{
    n->head.type  = type;
    n->head.iops  = &s_iops;
    n->head.fops  = (type == BR_INODE_DIR) ? &s_dir_fops : &s_file_fops;
    n->head.fpriv = n;                    /* 节点身份: vfs 在 open 时把它交给 br_file_t */
    n->parent     = parent;
    n->child      = BR_NULL;
    n->next       = BR_NULL;
    n->data       = BR_NULL;
    n->size       = 0u;
    n->cap        = 0u;
    tmpfs_str_copy(n->name, name, BR_NAME_MAX);
}

static br_bool tmpfs_node_room(void)
{
    return (s_fs.nodes_used < TMPFS_NODE_MAX) ? BR_TRUE : BR_FALSE;
}

/* 造一个节点并链进 dir(create 与 mkdir 共用: 名字校验/配额/入链只有一处实现)。 */
static int tmpfs_node_add(tmpfs_node_t *dir, const char *name, br_u32 type,
                          tmpfs_node_t **out)
{
    tmpfs_node_t *n;

    if (tmpfs_node_room() == BR_FALSE) {
        return BR_ERR(BR_ENOSPC);         /* 节点配额用尽 */
    }
    n = (tmpfs_node_t *)br_malloc(sizeof *n);
    if (n == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }
    tmpfs_node_init(n, type, dir, name);

    /* 头插: 目录项顺序**无契约**(readdir 只保证 "." + ".." + 名字集合), 头插省一次遍历。
     * 若将来要求"创建顺序 = readdir 顺序", 把这里改成尾插即可(上限 48, 代价可忽略)。 */
    n->next    = dir->child;
    dir->child = n;
    s_fs.nodes_used++;
    if (out != BR_NULL) {
        *out = n;
    }
    return 0;
}

/* 填一条定长目录项。整条先清零: 不然 name[] 的尾部会把上一轮的残留漏给调用方。 */
static void tmpfs_dirent_fill(br_dirent_t *de, br_u32 type, const char *name, br_u32 name_len)
{
    tmpfs_zero(de, (br_size_t)sizeof *de);
    de->type     = type;
    de->name_len = name_len;
    tmpfs_str_copy(de->name, name, BR_NAME_MAX);
}

/* =====================================================================
 * 文件数据(堆上的可增长缓冲 + 配额)
 * ===================================================================== */

/*
 * 把数据缓冲还给堆, size/cap 归零。不变量: s_fs.data_used == Σ node->cap —— 因为账是
 * "加多少减多少", 这个减法是安全的(没有任何路径能不记账就改 cap)。
 */
static void tmpfs_file_release(tmpfs_node_t *n)
{
    if (n->data != BR_NULL) {
        br_free(n->data);
        n->data = BR_NULL;
    }
    s_fs.data_used -= n->cap;
    n->cap  = 0u;
    n->size = 0u;
}

/*
 * 备好 ≥ need 的容量(**不动 size**, 调用方随后自己设)。返回 0 / -ENOSPC / -ENOMEM。
 * 增长 = 几何翻倍(自 TMPFS_MIN_CAP 起): 顺序追加写是 tmpfs 最典型的写形状, 每次只扩到
 * need 会把它退化成 O(n²)。多要出来的容量照样记账(见 s_fs 的口径说明)。
 */
static int tmpfs_file_reserve(tmpfs_node_t *n, br_u32 need)
{
    br_u32 new_cap;
    br_u8 *p;

    if (need <= n->cap) {
        return 0;
    }
    if (need > TMPFS_DATA_MAX) {
        return BR_ERR(BR_ENOSPC);         /* 单次请求本身就超上界 */
    }

    new_cap = (n->cap > 0u) ? n->cap : TMPFS_MIN_CAP;
    while (new_cap < need) {
        if (new_cap > (TMPFS_DATA_MAX / 2u)) {
            new_cap = TMPFS_DATA_MAX;     /* 防"乘 2"绕回: 直接顶到上界 */
            break;
        }
        new_cap *= 2u;
    }

    /* 配额执法: 换上 new_cap 之后的**全 FS 容量和**必须仍在界内 */
    if ((s_fs.data_used - n->cap + new_cap) > TMPFS_DATA_MAX) {
        return BR_ERR(BR_ENOSPC);
    }

    p = (br_u8 *)br_realloc(n->data, (br_size_t)new_cap);
    if (p == BR_NULL) {
        return BR_ERR(BR_ENOMEM);         /* 原块保持有效(br_realloc 的 POSIX 语义) */
    }
    s_fs.data_used = s_fs.data_used - n->cap + new_cap;
    n->data = p;
    n->cap  = new_cap;
    return 0;
}

/*
 * 截断/扩张到 size(POSIX truncate 语义)。两处非显然的取舍:
 *   ① 扩张要**显式清零**中间的空洞 —— br_realloc 不清零, 留着会让 read 把堆里上一个块的
 *      残留当文件内容返回(信息泄漏, 而且不是 POSIX 的"空洞读回 0");
 *   ② 收缩取**精确**容量(把额度还给记账体), 而不是留着老 cap —— 记账口径是 cap,
 *      留着就是留着一笔不再需要的额度占用。代价是收缩多一次 realloc, 但收缩不频繁。
 */
static int tmpfs_file_truncate(tmpfs_node_t *n, br_u64 size)
{
    br_u8 *p;
    int rc;

    if (size == n->size) {
        return 0;
    }

    if (size > n->size) {                              /* 扩张(可能带空洞) */
        if (size > (br_u64)TMPFS_DATA_MAX) {
            return BR_ERR(BR_ENOSPC);
        }
        rc = tmpfs_file_reserve(n, (br_u32)size);
        if (rc != 0) {
            return rc;
        }
        tmpfs_zero(n->data + (br_size_t)n->size, (br_size_t)(size - n->size));
        n->size = size;
        return 0;
    }

    if (size == 0u) {                                  /* 收缩到 0: 整块还给堆 */
        tmpfs_file_release(n);                         /* 不走 br_realloc(p,0): 它会 free 并返回 NULL */
        return 0;
    }
    p = (br_u8 *)br_realloc(n->data, (br_size_t)size);
    if (p == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }
    s_fs.data_used = s_fs.data_used - n->cap + (br_u32)size;
    n->data = p;
    n->cap  = (br_u32)size;
    n->size = size;
    return 0;
}

/* =====================================================================
 * 层 3 实现: 文件面
 * ===================================================================== */

static int tmpfs_file_open(br_file_t *f, br_u32 flags)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int rc = tmpfs_require_file(node);

    if (rc != BR_OK) {
        return rc;
    }

    /* O_TRUNC: 打开即截断到 0。**权限不在这里执法**(O_TRUNC 需写权限 / 只读 fd 不得写):
     * 那是 vfs/svc-posix 的 fd 层职责(契约头也把这条写在 flags 的语义里)。同一策略在 FS
     * 侧再来一遍只会造出两个真值, 迟早不一致。 */
    if ((flags & BR_O_TRUNC) != 0u) {
        rc = tmpfs_file_truncate(node, 0u);
        if (rc != BR_OK) {
            return rc;
        }
    }

    if ((flags & BR_O_APPEND) != 0u) {
        br_file_set_offset(f, (br_s64)node->size);
    }
    return 0;
}

static br_s64 tmpfs_file_read(br_file_t *f, void *buf, br_size_t n)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int    rc  = tmpfs_require_file(node);
    br_s64 off;
    br_u64 avail;
    br_size_t cnt;

    if (rc != BR_OK) {
        return rc;
    }
    if ((buf == BR_NULL) && (n > 0u)) {
        return BR_ERR(BR_EINVAL);
    }
    if ((node->size > 0u) && (node->data == BR_NULL)) {
        return BR_ERR(BR_EIO);            /* 不变量被破坏(size>0 ⇒ data 非空): 报错而不是解引用 */
    }

    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(BR_EINVAL);
    }
    if ((br_u64)off >= node->size) {
        return 0;                         /* 读到/越过 EOF: 0 字节是**成功**(不是 -EIO) */
    }

    avail = node->size - (br_u64)off;
    cnt = (n > (br_size_t)avail) ? (br_size_t)avail : n;   /* avail ≤ 128 KiB ⇒ 收窄安全 */
    if (cnt > 0u) {
        tmpfs_copy(buf, node->data + (br_size_t)off, cnt);
    }
    br_file_set_offset(f, off + (br_s64)cnt);
    return (br_s64)cnt;
}

static br_s64 tmpfs_file_write(br_file_t *f, const void *buf, br_size_t n)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int    rc  = tmpfs_require_file(node);
    br_s64 off;
    br_u64 need;

    if (rc != BR_OK) {
        return rc;
    }
    if ((buf == BR_NULL) && (n > 0u)) {
        return BR_ERR(BR_EINVAL);
    }

    /* BR_O_APPEND 的契约是"**每次写前**定位到末尾", 所以零长度写也要先定位(与 Linux 的
     * 实现细节不同, 这里跟契约文字走)。 */
    if ((br_file_flags(f) & BR_O_APPEND) != 0u) {
        br_file_set_offset(f, (br_s64)node->size);
    }
    if (n == 0u) {
        return 0;
    }

    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(BR_EINVAL);
    }
    need = (br_u64)off + (br_u64)n;
    if (need < (br_u64)off) {
        return BR_ERR(BR_EINVAL);         /* 偏移算术绕回 */
    }
    if (need > (br_u64)TMPFS_DATA_MAX) {
        return BR_ERR(BR_ENOSPC);         /* 写越 128 KiB 配额: 这里就是那条判据的落点 */
    }

    rc = tmpfs_file_reserve(node, (br_u32)need);
    if (rc != BR_OK) {
        return rc;
    }
    if ((br_u64)off > node->size) {
        /* 空洞写(lseek 越过 EOF 后再写): 中间必须读回 0, 见 tmpfs_file_truncate 的 ① */
        tmpfs_zero(node->data + (br_size_t)node->size, (br_size_t)((br_u64)off - node->size));
    }
    tmpfs_copy(node->data + (br_size_t)off, buf, n);
    if (need > node->size) {
        node->size = need;
    }
    br_file_set_offset(f, (br_s64)need);
    return (br_s64)n;
}

static br_s64 tmpfs_file_lseek(br_file_t *f, br_s64 off, int whence)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int    rc   = tmpfs_require_file(node);
    br_s64 base;
    br_s64 pos;

    if (rc != BR_OK) {
        return rc;
    }
    switch (whence) {
    case BR_SEEK_SET: base = 0; break;
    case BR_SEEK_CUR: base = br_file_offset(f); break;
    case BR_SEEK_END: base = (br_s64)node->size; break;
    default:          return BR_ERR(BR_EINVAL);
    }
    if (base < 0) {
        return BR_ERR(BR_EINVAL);
    }
    /* 只允许落在 br_s64 的非负区间; 用内建判溢出, 免得有符号加溢出(UB)。
     * 允许越过 EOF(POSIX): 之后读得 0, 写则形成空洞。 */
    if (__builtin_add_overflow(base, off, &pos) || (pos < 0)) {
        return BR_ERR(BR_EINVAL);
    }
    br_file_set_offset(f, pos);
    return pos;
}

static int tmpfs_file_close(br_file_t *f)
{
    /* 文件会话没有要归还的东西: fpriv 仍是节点(持久对象), 由 vfs 释放 br_file_t 本身。
     * 与目录面成对(那里必须 br_free 迭代器) —— 这一对正是"open 放进去的, close 负责收" */
    (void)f;
    return 0;
}

/* =====================================================================
 * 层 3 实现: 目录面(read = 一条 br_dirent_t)
 * ===================================================================== */

static int tmpfs_dir_open(br_file_t *f, br_u32 flags)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    tmpfs_dir_iter_t *it;

    if ((node == BR_NULL) || (node->head.type != BR_INODE_DIR)) {
        return BR_ERR(BR_ENOTSUP);        /* 目录面落在非目录节点上: 没有对应 errno, 按"不支持" */
    }
    /* "目录写打开 ⇒ -EISDIR"由 vfs 在 br_open 里就挡下(见 br_vfs.c); 这里不重复执法,
     * 于是 flags 本槽位只读不判(参数保留: 契约签名如此)。 */
    (void)flags;

    it = (tmpfs_dir_iter_t *)br_malloc(sizeof *it);
    if (it == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }
    it->dir    = node;
    it->cursor = 0u;
    br_file_set_fpriv(f, it);             /* 把会话状态放进**文件句柄**, 不动节点身份 */
    return 0;
}

static br_s64 tmpfs_dir_read(br_file_t *f, void *buf, br_size_t n)
{
    tmpfs_dir_iter_t *it = (tmpfs_dir_iter_t *)br_file_fpriv(f);
    br_dirent_t *de = (br_dirent_t *)buf;
    tmpfs_node_t *c;
    br_u32 idx;

    if ((it == BR_NULL) || (it->dir == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    /* 定长记录不可截断: 装不下一条就 -EINVAL(返回 0 会被 br_readdir 读成"目录已穷尽",
     * 那是撒谎; 返回半条则被 br_readdir 判 -EIO 的 FS 违约)。vfs 恒传 sizeof(br_dirent_t)。 */
    if ((buf == BR_NULL) || (n < (br_size_t)sizeof(br_dirent_t))) {
        return BR_ERR(BR_EINVAL);
    }

    /* "." 与 ".." 不是真目录项, 由枚举**就地合成**(与 lookup 的解析口径一致) */
    if (it->cursor == 0u) {
        it->cursor = 1u;
        tmpfs_dirent_fill(de, BR_INODE_DIR, ".", 1u);
        return (br_s64)sizeof(br_dirent_t);
    }
    if (it->cursor == 1u) {
        it->cursor = 2u;
        tmpfs_dirent_fill(de, BR_INODE_DIR, "..", 2u);
        return (br_s64)sizeof(br_dirent_t);
    }

    /* 子节点: 用**游标重走**而不是在迭代器里缓存 next 指针 —— rename/unlink 会改链,
     * 缓存的指针会悬空; 节点上界 48, 重走的代价可忽略(正确性优先)。 */
    idx = 2u;
    for (c = it->dir->child; c != BR_NULL; c = c->next) {
        if (idx == it->cursor) {
            it->cursor = idx + 1u;
            tmpfs_dirent_fill(de, c->head.type, c->name, tmpfs_str_len(c->name));
            return (br_s64)sizeof(br_dirent_t);
        }
        idx++;
    }
    return 0;                             /* 穷尽 */
}

static int tmpfs_dir_close(br_file_t *f)
{
    tmpfs_dir_iter_t *it = (tmpfs_dir_iter_t *)br_file_fpriv(f);

    if (it != BR_NULL) {
        br_free(it);
        br_file_set_fpriv(f, BR_NULL);    /* 让二次 close 成为空操作 */
    }
    return 0;
}

/* =====================================================================
 * 层 2 实现: inode ops(每个槽位自判节点类型)
 * ===================================================================== */

/*
 * lookup: **不分配**(与 littlefs 的"瞬态路径前缀包装"正相反 —— 见文件头)。
 * "." / ".." 就地解析: 根的父亲 = 根, 于是 "/.." == "/"(POSIX 口径, 也避免根成为特例)。
 * 返回类型是 inode 指针, 没有错误码通道 ⇒ "未命中"与"调错表(非目录)"都收敛为 BR_NULL;
 * vfs 的 walk_to 正是靠 BR_NULL 区分"不存在(-ENOENT)"与"失败"。
 */
static br_inode_t *tmpfs_lookup(br_inode_t *dir, const char *name)
{
    tmpfs_node_t *d;
    tmpfs_node_t *c;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR) || (name == BR_NULL)) {
        return BR_NULL;
    }
    d = tmpfs_ino_node(dir);
    if (d == BR_NULL) {
        return BR_NULL;
    }
    if (tmpfs_str_eq(name, ".") == BR_TRUE) {
        return &d->head;
    }
    if (tmpfs_str_eq(name, "..") == BR_TRUE) {
        return &d->parent->head;          /* d->parent 恒非空(根自指) */
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_NULL;                   /* 非法名字: 走查里它就是"不存在" */
    }
    for (c = d->child; c != BR_NULL; c = c->next) {
        if (tmpfs_str_eq(c->name, name) == BR_TRUE) {
            return &c->head;
        }
    }
    return BR_NULL;
}

static int tmpfs_create(br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out)
{
    tmpfs_node_t *d;
    tmpfs_node_t *ex;
    tmpfs_node_t *n = BR_NULL;
    int rc;

    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    *out = BR_NULL;
    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(BR_ENOTSUP);        /* 只有目录能建目录项 */
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(BR_EINVAL);
    }
    d = tmpfs_ino_node(dir);

    ex = tmpfs_child_find(d, name);
    if (ex != BR_NULL) {
        if ((flags & BR_O_EXCL) != 0u) {
            return BR_ERR(BR_EEXIST);
        }
        /* O_CREAT 无 O_EXCL 命中已存在: 复用(Linux 口径)。vfs 的走查已先 lookup 过一次,
         * 这里再查是"create 自己也自洽"的防御 —— 语义在两层都成立, 单看任一层都不缺。 */
        if (ex->head.type != BR_INODE_FILE) {
            return BR_ERR(BR_EISDIR);     /* create 只产文件; 目录要走 mkdir */
        }
        *out = &ex->head;
        return 0;
    }

    rc = tmpfs_node_add(d, name, BR_INODE_FILE, &n);
    if (rc != BR_OK) {
        return rc;
    }
    *out = &n->head;
    return 0;
}

static int tmpfs_unlink(br_inode_t *dir, const char *name)
{
    tmpfs_node_t *d;
    tmpfs_node_t *victim;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(BR_ENOTSUP);
    }
    /* 变更用的名字走完整规则: "." / ".." / 含 '/' / 空 / 超长 ⇒ -EINVAL。
     * (Linux 的 unlink(".") 是 -EISDIR, 这里取与 rmdir/rename 的显式条款同一口径。) */
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(BR_EINVAL);
    }
    d = tmpfs_ino_node(dir);
    victim = tmpfs_child_find(d, name);
    if (victim == BR_NULL) {
        return BR_ERR(BR_ENOENT);
    }
    if (victim->head.type != BR_INODE_FILE) {
        return BR_ERR(BR_EISDIR);         /* 目录要 rmdir(删树的语义归调用方) */
    }
    tmpfs_child_unlink(d, victim);
    tmpfs_file_release(victim);           /* 数据缓冲还给堆(账随之交还) */
    br_free(victim);
    s_fs.nodes_used--;
    return 0;
}

static int tmpfs_mkdir(br_inode_t *dir, const char *name)
{
    tmpfs_node_t *d;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(BR_ENOTSUP);
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(BR_EINVAL);
    }
    d = tmpfs_ino_node(dir);
    if (tmpfs_child_find(d, name) != BR_NULL) {
        return BR_ERR(BR_EEXIST);         /* 已存在(含"是文件"的情形): 都是 EEXIST */
    }
    /* out = BR_NULL: mkdir 只回答 rc(vfs 会自己再 lookup 一次拿 inode, 见 br_vfs.c) */
    return tmpfs_node_add(d, name, BR_INODE_DIR, BR_NULL);
}

static int tmpfs_rmdir(br_inode_t *dir, const char *name)
{
    tmpfs_node_t *d;
    tmpfs_node_t *victim;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(BR_ENOTSUP);
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(BR_EINVAL);         /* "." / ".." 在这一并被挡下 */
    }
    d = tmpfs_ino_node(dir);
    victim = tmpfs_child_find(d, name);
    if (victim == BR_NULL) {
        return BR_ERR(BR_ENOENT);
    }
    if (victim->head.type != BR_INODE_DIR) {
        return BR_ERR(BR_ENOTDIR);        /* 对文件用 rmdir = 用错工具(不是"非空") */
    }
    if (victim->child != BR_NULL) {
        /* "非空目录"在 SD-10 子集里没有对应的码时曾取 -EBUSY, 但那是"挂载点不可删"的语义
         * (vfs 的 TC-VFS-012 正是这么判的)⇒ 会撞码。core 头已补入 -ENOTEMPTY(编号 39),
         * 这里用它 —— 排障与 svc-posix 的 errno 映射都要这个区分。 */
        return BR_ERR(BR_ENOTEMPTY);
    }
    /* 根**不可能**被 rmdir: 它在任何父目录里都没有目录项(根没有父), 于是没有名字能指向它;
     * "." / ".." 上面已被名字规则挡下 ⇒ "不许删根"这条不需要特判代码。 */
    tmpfs_child_unlink(d, victim);
    br_free(victim);
    s_fs.nodes_used--;
    return 0;
}

static int tmpfs_rename(br_inode_t *dir, const char *name, br_inode_t *ndir, const char *nname)
{
    tmpfs_node_t *d;
    tmpfs_node_t *nd;
    tmpfs_node_t *victim;
    tmpfs_node_t *t;

    if ((dir == BR_NULL) || (ndir == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    if ((dir->type != BR_INODE_DIR) || (ndir->type != BR_INODE_DIR)) {
        return BR_ERR(BR_ENOTSUP);
    }
    /* 目标目录必须是**本插件**的节点: child/next/parent 是 tmpfs 的私有尾语义, 把别家 FS 的
     * 节点链进自己的链 = 用错布局解释内存。vfs 已在挂载层面挡下跨挂载 rename(-ENOTSUP),
     * 这里是"私有尾只能由本 FS 解释"的第二道(也是将来多 tmpfs 挂载时的等价保证)。 */
    if (ndir->iops != &s_iops) {
        return BR_ERR(BR_ENOTSUP);
    }
    /* "." / ".." 显式拒绝(-EINVAL), 与 rmdir/unlink 同口径 */
    if ((tmpfs_name_ok(name) == BR_FALSE) || (tmpfs_name_ok(nname) == BR_FALSE)) {
        return BR_ERR(BR_EINVAL);
    }

    d = tmpfs_ino_node(dir);
    nd = tmpfs_ino_node(ndir);
    victim = tmpfs_child_find(d, name);
    if (victim == BR_NULL) {
        return BR_ERR(BR_ENOENT);
    }
    if (tmpfs_child_find(nd, nname) != BR_NULL) {
        /* v1 不做"覆盖已存在目标"(Linux 允许覆盖文件、拒绝非空目录): 目标存在即 -EEXIST,
         * 一个码把所有情形说清楚; 也顺带把"同名重命名"判成调用方错误(POSIX 的 no-op 语义
         * 在这里没有需要, 反而会掩盖脚本 bug)。 */
        return BR_ERR(BR_EEXIST);
    }
    /* 拒绝把目录搬进自己的子树: 那会做出 a→…→a 的环, 之后**任何**走查都不会终止
     * (lookup("..") 链与 child 链互咬)。Linux 对同一情形报 -EINVAL。上溯 ≤ 节点数。 */
    if (victim->head.type == BR_INODE_DIR) {
        for (t = nd; t != BR_NULL; t = t->parent) {
            if (t == victim) {
                return BR_ERR(BR_EINVAL);
            }
            if (t->parent == t) {
                break;                    /* 到根 */
            }
        }
    }

    /* 先摘后插: 同一父目录下也走这一条(先摘再插, 不会把自己插到自己后面成环) */
    tmpfs_child_unlink(d, victim);
    tmpfs_str_copy(victim->name, nname, BR_NAME_MAX);
    victim->parent = nd;
    victim->next   = nd->child;
    nd->child      = victim;
    return 0;
}

static int tmpfs_getattr(br_inode_t *ino, br_stat_t *st)
{
    tmpfs_node_t *n;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    if ((ino->type != BR_INODE_DIR) && (ino->type != BR_INODE_FILE)) {
        return BR_ERR(BR_ENOTSUP);
    }
    n = tmpfs_ino_node(ino);
    if (n == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    /* getattr 是"取属性", 不看入参的 valid(那是 setattr 的输入面)。
     * 出参 valid 只报 BR_STAT_SIZE: 头里只有这一位, `type` 是**恒有效**字段(不是可选项),
     * 于是"v1 只实现 size"这句话在 valid 上如实体现。 */
    st->valid = BR_STAT_SIZE;
    st->type  = ino->type;
    st->size  = (ino->type == BR_INODE_DIR) ? 0u : n->size;
    return 0;
}

static int tmpfs_setattr(br_inode_t *ino, const br_stat_t *st)
{
    tmpfs_node_t *n;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    /* v1 只认 size(截断)。不认的位**必须**报 -ENOTSUP 而不是装作成功 —— 假装成功会让调用方
     * 以为属性真的生效了(比拒绝危险得多)。 */
    if ((st->valid & ~(br_u32)BR_STAT_SIZE) != 0u) {
        return BR_ERR(BR_ENOTSUP);
    }
    if ((st->valid & BR_STAT_SIZE) == 0u) {
        return 0;                         /* 没要求改任何东西: 空操作是正确答案 */
    }
    n = tmpfs_ino_node(ino);
    if ((n == BR_NULL) || (ino->type != BR_INODE_FILE)) {
        return BR_ERR(BR_EISDIR);         /* 目录没有"文件长度"可截断 */
    }
    return tmpfs_file_truncate(n, st->size);
}

/* =====================================================================
 * 层 1 实现: super(mount / free_inode)
 * ===================================================================== */

/* mount 回滚: 那时树里只可能是"根 + 刚预建的目录"(深度 1, 无文件数据), 所以不做通用递归
 * 释放; 通用释放(给将来的 unmount 用)属 v2 —— unmount 槽位在 v1 本来就是 BR_NULL。 */
static void tmpfs_drop_tree(tmpfs_node_t *root)
{
    tmpfs_node_t *c = root->child;

    while (c != BR_NULL) {
        tmpfs_node_t *next = c->next;
        br_free(c);
        s_fs.nodes_used--;
        c = next;
    }
    br_free(root);
    s_fs.nodes_used--;
}

static int tmpfs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    static const char *const pre[] = { "dev", "data", "tmp" };   /* 设计 7-03 §6 的预建目录 */
    tmpfs_node_t *r;
    br_size_t     i;
    int           rc = 0;

    if ((cfg == BR_NULL) || (root == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    *root = BR_NULL;

    /* 挂载点**不**在此校验(与 fs/devfs 刻意不同): tmpfs 的节点与路径无关 —— 它没有
     * devfs 那种"节点前缀 = 挂载点"的耦合, 挂到别的点上也完全自洽(v2 的运行时挂载会
     * 用到)。v1 的"只挂 /"由 manifest 的挂载计划与拓扑序保证, 校验归组合期。 */

    /* 记账体只有一份(节点/inode ops 的签名里没有 fs_priv, 它们只能走文件域静态), 于是要求
     * vfs 交回来的**就是**那一个。两套访问路径指向不同对象时日志会撒谎 —— 与其静默不如报错。 */
    if (fs_priv != (void *)&s_fs) {
        return BR_ERR(BR_EINVAL);
    }

    /* mount 是**单次**的(同一 path 二次注册由 vfs 以 -EEXIST 挡下)⇒ 记账清零 = "从这里开始
     * 只有一棵树"。若 v2 支持运行时挂载/卸载, 这里必须换成 unmount 配对释放整棵树。 */
    s_fs.nodes_used = 0u;
    s_fs.data_used  = 0u;

    r = (tmpfs_node_t *)br_malloc(sizeof *r);
    if (r == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }
    tmpfs_node_init(r, BR_INODE_DIR, r, "/");   /* 根的 parent = 自己: "/.." == "/" */
    s_fs.nodes_used = 1u;

    /* 预建 /dev /data /tmp: 走**同一条** mkdir 路径(名字校验/节点配额/入链只有一处实现)。
     * 这也是 fs/devfs 能把 /dev 挂上的前提(挂载点已存在 ⇒ vfs 不再自动 mkdir)。 */
    for (i = 0u; i < BR_ARRAY_SIZE(pre); i++) {
        rc = tmpfs_mkdir(&r->head, pre[i]);
        if (rc != 0) {
            break;
        }
    }
    if (rc != 0) {
        br_log_error("tmpfs: mount %s failed at pre-create (%d)", cfg->path, rc);
        tmpfs_drop_tree(r);              /* 半成品不留: vfs 不会记录这次挂载, 留着就是纯泄漏 */
        return rc;
    }

    *root = &r->head;
    br_log_info("tmpfs: mounted %s (nodes<=%u, data<=%u KiB)",
                cfg->path, TMPFS_NODE_MAX, TMPFS_DATA_MAX / 1024u);
    return 0;
}

/*
 * free_inode —— **no-op**, 这是本 FS 与 littlefs/devfs 形状上的关键差别:
 * vfs 只把**瞬态** inode 交给 free_inode(见 br_vfs.c 的 ino_put: 挂载表的根 inode 与
 * 临时走查产物分得很清)。而 tmpfs 的节点是显式分配、持久存活的对象 —— lookup() 交回的
 * 就是链上的它本身, 没有任何"用完即弃"的产物。删/改节点只经 unlink/rmdir/rename(那里
 * 才 br_free)。所以这里真去 br_free 一下, 会把活着的目录树撕掉。
 */
static void tmpfs_free_inode(br_inode_t *ino)
{
    (void)ino;
}

/* =====================================================================
 * 生命周期实现
 * ===================================================================== */

/* EARLY 相: 堆还没认领(`br_mem_init` 在 core.init 才建池), 线程也没有 —— 本相只允许
 * "声明", 不允许分配。tmpfs 没有静态声明要做, 于是空手返回。 */
int tmpfs_early_init(void)
{
    return 0;
}

/*
 * CORE 相: 挂 rootfs。堆此时已就绪(这就是"绝不在 init 之前 br_malloc"的由来:
 * mount 里所有分配都发生在 br_mount_register → s_fs_ops.mount 这条链上)。
 *
 * init 自己**不建树**: 建根 inode 与预建目录是 super 层 mount 槽位的职责, 由
 * `br_mount_register` 在检查参数/规范化路径之后调用(契约如此: 建 cfg → ops->mount →
 * 记录槽位)。这里只负责"注册"这一件事, 失败原样上传负 errno(首败即停机由插件管理器执行)。
 */
int tmpfs_init(void)
{
    const int rc = br_mount_register(TMPFS_MOUNT_PATH, &s_fs_ops, (void *)&s_fs);

    if (rc != 0) {
        br_log_error("tmpfs: register %s failed (%d)", TMPFS_MOUNT_PATH, rc);
        return rc;
    }
    return 0;
}

/* START 相: 一行用量摘要(启动证据, 不是判据)。数字口径: 节点 = 已分配/上界;
 * 数据 = 已向堆申请的**容量**之和/上界(不是文件长度之和, 见 s_fs 的记账说明)。 */
int tmpfs_start(void)
{
    br_log_info("tmpfs: usage nodes %u/%u, data %u/%u B",
                s_fs.nodes_used, TMPFS_NODE_MAX, s_fs.data_used, TMPFS_DATA_MAX);
    return 0;
}
