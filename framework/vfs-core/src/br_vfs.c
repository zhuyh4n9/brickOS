/*
 * brickOS prototype v0.2.0 — VFS 核心(framework/vfs-core)
 *
 * 设计依据: `docs/7-storage/7-01-vfs.md`(§1 统一可打开模型 SD-1/D21、§2 挂载与 ops
 * 四层分层 SD-15/D23、§3 poll 分期 SD-7)、`docs/8-device/8-01-device.md` §2(open_file 钩子
 * 的 {fops, fpriv})、`docs/3-os-core/3-04-memory.md`(句柄出自 core 堆)。
 * 契约: `include/br/vfs/br_vfs.h`; 逐条裁定与偏离: `docs/decisions/0009-vfs-storage-stack.md`。
 *
 * ## 本文件实现的**全部**东西
 *   ① 挂载表 + **最长前缀匹配**(§1 的单路由: `br_open` 只走这张表);
 *   ② 路径规范化 + **逐级 lookup 走查链**(§2 "路径走查在 vfs-core")+ 瞬态 inode 的
 *      释放时机管理(v1 无 cache, 走查即弃 —— SD-3);
 *   ③ 文件句柄(`br_file_t`)的分配/释放 + 层 3 派发(层 1/2/4 的 ops 表由 FS 填);
 *   ④ 目录迭代(泛型: 走文件面的 `read` 取定长目录项);
 *   ⑤ 路径级便捷面(stat/mkdir/rmdir/unlink/rename/truncate —— runtime/posix 的 1:1 映射口);
 *   ⑥ **不含**一致性用例: 存储域总套件(TC-VFS-*)已搬到 `src/vfs_selftest.c`,
 *      由 core 的自检 pass 驱动(ADR-0010; 测试入口不进 [[export]])。
 *
 * ## 本文件**不**做什么(D21 的"纯 VFS")
 *   零设备知识: 没有任何 `br_cdev`/`br_dev`/设备名分支 —— 设备经 fs/devfs 以文件节点
 *   出现, vfs 看到的只是"某个 FS 的某个 inode 带了 fops/fpriv"。这是 D21 撤销
 *   "vfs-core 依赖 dev-core" 之后的形态。
 *
 * ## inode 所有权规则(唯一容易错的地方, 所以写在文件头)
 *   - **挂载表持有的根 inode 不属瞬态**: vfs 永不把它交给 `free_inode`;
 *   - `lookup`/`create` 返回的 inode **属瞬态**: 谁拿到谁负责 —— 走查中间层用完即弃,
 *     末级 inode 交给 `br_file_t` 并由 `br_file_close` 释放;
 *   - `br_stat`/`br_mkdir`/… 这类"用完就走"的路径**立即**释放末级 inode。
 *   全文件用一个 `ino_put()` 收敛这条规则, 所有释放点都经它 —— 不存在第二处判断。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "vfs_internal.h"      /* 自检用的内部视图(见该头; 生产路径不用它) */

/* 生命周期钩子原型(名 = symbol_prefix + 相; 生成物 `plugin_desc.c` 引用; 不进 [[export]]) */
int vfs_core_early_init(void);
int vfs_core_init(void);
int vfs_core_start(void);

/* ==================================================================== 静态上界 */

#define BR_VFS_MAX_MOUNTS    8u    /* 挂载表上界(设计 SD-4: v1 静态挂载) */
#define BR_VFS_PATH_MAX      128u  /* 规范化路径缓冲(含结尾 '\0'); ADR-0013 起 64 → 128
                                    * —— 符号链接的"前缀 + 目标 + 剩余分量"拼接要放得下 */
#define BR_VFS_SYMLINK_DEPTH 8u    /* 符号链接展开深度上界(超 ⇒ -ELOOP) */

/* 允许的 flags 全集之外的位置位 ⇒ -EINVAL(静默忽略会让"我请求了 O_EXCL 却没人管"藏起来) */
#define BR_VFS_KNOWN_FLAGS   (BR_O_ACCMODE | BR_O_CREAT | BR_O_EXCL | \
                              BR_O_TRUNC | BR_O_APPEND | BR_O_NONBLOCK | BR_O_NOFOLLOW)

/* ==================================================================== 挂载表 */

typedef struct vfs_mount {
    br_bool            used;
    char               path[BR_VFS_PATH_MAX];   /* 规范化: 无尾 '/'(根除外)/无 "//" */
    br_u32             len;                     /* strlen(path); 根 = 1 */
    const br_fs_ops_t *ops;
    void              *priv;
    br_inode_t        *root;                    /* 挂载表持有, **永不**交给 free_inode */
} vfs_mount_t;

static vfs_mount_t s_mounts[BR_VFS_MAX_MOUNTS];

/* ★ 布局守卫(ADR-0013 补): `br_vfs_internal_mounts()` 是把 `s_mounts` **强转**成
 *   `vfs_mount_internal_t*` 给自检看的(见 src/vfs_internal.h 的说明)。两处声明一旦
 *   不一致, 自检读到的就是错位的字节 —— 那是一次真实踩坑(只改生产侧的路径上界,
 *   TC-VFS-002 立刻红, 现象却是"所有挂载都指向 /"), 所以把"必须逐字段一致"从注释
 *   升级成编译期断言。 */
_Static_assert(sizeof(vfs_mount_t) == sizeof(vfs_mount_internal_t),
               "vfs_mount_t 与 vfs_mount_internal_t 布局必须一致(内部视图靠强转)");
_Static_assert(BR_VFS_PATH_MAX == BR_VFS_PATH_MAX_INTERNAL,
               "BR_VFS_PATH_MAX 与 BR_VFS_PATH_MAX_INTERNAL 必须一致");
static br_u32      s_mount_n;   /* 成功挂载条数(等于表内 used 的条数) */

/* ==================================================================== 句柄模型
 * `br_file_t` 对插件**不透明**(br_vfs.h 只前向声明)⇒ 布局只活在本文件里;
 * 插件一律经 `br_file_*` 访问器读写(D14 的"句柄不透明" + CA-5 的面预算)。 */
struct br_file {
    const vfs_mount_t   *m;              /* 所属挂载(close 时要经它的 free_inode) */
    const br_file_ops_t *fops;
    br_inode_t          *ino;
    br_bool              ino_transient;  /* 末级 inode 是否该在 close 时释放 */
    void                *fpriv;
    br_s64               offset;
    br_u32               flags;
};

struct br_dir {
    br_file_t *f;
};

/* ==================================================================== 小工具 */

static br_bool vfs_str_eq(const char *a, const char *b)
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

static br_size_t vfs_str_len(const char *s)
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

static void vfs_str_copy(char *dst, const char *src, br_size_t cap)
{
    br_size_t i = 0u;
    if (cap == 0u) {
        return;
    }
    for (; src != BR_NULL && src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

br_bool br_vfs_internal_mem_eq(const void *a, const void *b, br_size_t n)
{
    const br_u8 *x = (const br_u8 *)a;
    const br_u8 *y = (const br_u8 *)b;
    for (br_size_t i = 0u; i < n; i++) {
        if (x[i] != y[i]) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

/*
 * 路径规范化(设计 §2 "native API 只认绝对路径"): 必须绝对; 折叠重复 '/'; 去掉尾部 '/'
 * (根除外)。**不解析 "." / ".."** —— 那是 FS 的 lookup 语义(每个 FS 对"根的上层"
 * 回答不同, 例如 devfs 的 ".." 仍是它自己), vfs 不替它决定。
 * 返回 0 / `-EINVAL`(相对或空) / `-ENOSPC`(超缓冲)。
 */
static int path_norm(const char *in, char *out, br_size_t cap)
{
    if (in == BR_NULL || in[0] != '/') {
        return BR_ERR(BR_EINVAL);
    }

    br_size_t n = 0u;
    br_bool   prev_slash = BR_FALSE;

    for (br_size_t i = 0u; in[i] != '\0'; i++) {
        if (in[i] == '/') {
            if (prev_slash == BR_TRUE) {
                continue;               /* "//" ⇒ "/" */
            }
            prev_slash = BR_TRUE;
        } else {
            prev_slash = BR_FALSE;
        }
        if (n + 1u >= cap) {
            return BR_ERR(BR_ENOSPC);
        }
        out[n++] = in[i];
    }

    while (n > 1u && out[n - 1u] == '/') {
        n--;                            /* 尾部 '/' 去掉; "/" 保留 */
    }
    out[n] = '\0';
    return BR_OK;
}

/* 挂载点前缀比较(**分量边界**, 不是裸字符串前缀 —— 否则 "/devx" 会错配 "/dev") */
static br_bool mount_prefix_eq(const vfs_mount_t *m, const char *norm)
{
    if (m->len == 1u) {
        return BR_TRUE;                 /* 根挂载匹配一切 */
    }
    for (br_u32 i = 0u; i < m->len; i++) {
        if (norm[i] == '\0' || norm[i] != m->path[i]) {
            return BR_FALSE;
        }
    }
    return (norm[m->len] == '\0' || norm[m->len] == '/') ? BR_TRUE : BR_FALSE;
}

/* **最长前缀匹配**(设计 §1: "/data/xxx" → littlefs, 不是 rootfs) */
static const vfs_mount_t *mount_match(const char *norm)
{
    const vfs_mount_t *best = BR_NULL;

    for (br_u32 i = 0u; i < s_mount_n; i++) {
        const vfs_mount_t *m = &s_mounts[i];
        if (m->used == BR_FALSE || mount_prefix_eq(m, norm) == BR_FALSE) {
            continue;
        }
        if (best == BR_NULL || m->len > best->len) {
            best = m;
        }
    }
    return best;
}

/* 挂载点之后的相对路径(**无前导 '/'**; 目标就是挂载点时返回 "") */
static const char *mount_rest(const vfs_mount_t *m, const char *norm)
{
    if (m->len == 1u) {
        return (norm[1] == '\0') ? "" : (norm + 1);
    }
    if (norm[m->len] == '\0') {
        return "";
    }
    return norm + m->len + 1u;
}

/* 瞬态 inode 释放的**唯一**入口(见文件头的所有权规则) */
static void ino_put(const vfs_mount_t *m, br_inode_t *ino, br_bool is_root)
{
    if (ino == BR_NULL || is_root == BR_TRUE) {
        return;                          /* 挂载表的根 inode 不属瞬态 */
    }
    if (m->ops->free_inode != BR_NULL) {
        m->ops->free_inode(ino);
    }
}

/* ==================================================================== 走查链(SD-3, ADR-0013 加符号链接)
 *
 * **谁解析符号链接**: vfs-core(不是 FS)。目标串可能是绝对路径(要重启挂载表匹配)或相对
 * 路径(要相对"链接所在目录"), 两件事都只有掌握挂载表与完整路径的 vfs 做得到; FS 只提供
 * `readlink`(给目标串)与 `symlink`/`link`(建链接)。
 *
 * **展开用"重启"而不是递归**: 本函数会被 APP 线程以 4 KiB 栈调用, 递归 8 层会把栈压穿。
 * 重启的代价是"从挂载根重走一遍", 在 v1 的单 APP 文件规模下可忽略(SD-3 已把"无 cache"
 * 的性能上限登记为 R-S3)。
 *
 * **`..` 不跨挂载**: 走查始终在**一个挂载内**下推(`mount_match` 定挂载, `lookup` 解 `..`),
 * 于是相对目标里的 `..` 到挂载根就停住 —— 这是 v1 有意的边界(不是 Linux 的语义),
 * 登记在 ADR-0013 §2.4。
 */

/* 往拼接缓冲里推一个字符(统一的长度执法点 ⇒ 超长一律 -ENAMETOOLONG)。 */
static int np_push(char *np, br_size_t *n, char c)
{
    if (*n + 1u >= (br_size_t)BR_VFS_PATH_MAX) {
        return BR_ERR(BR_ENAMETOOLONG);
    }
    np[(*n)++] = c;
    return BR_OK;
}

/*
 * 从挂载表出发解析一条**规范化**的绝对路径, 返回末级 inode。
 * `*is_root_out` = 末级是否就是"挂载表的根"(调用方据此决定能不能 free)。
 * `nofollow_last` = 末级是符号链接时**不**展开(stat/readlink/link/unlink 类用)。
 * 错误: `-ENOENT`(未命中)/ `-ENOTDIR`(中间级不是目录)/ `-ENOSPC`(分量超上界)/
 *       `-ENAMETOOLONG`(展开后超路径缓冲)/ `-ELOOP`(展开深度超上界)/ readlink 的错误。
 *
 * ★ 每级都 lookup(不缓存)是 SD-3 的直接后果: v1 **有 inode ops, 无 inode cache**。
 */
static int resolve_norm(const char *norm_in, br_bool nofollow_last,
                        const vfs_mount_t **m_out, br_inode_t **ino_out, br_bool *is_root_out,
                        char *final_out, br_size_t final_cap)
{
    char path[BR_VFS_PATH_MAX];
    vfs_str_copy(path, norm_in, (br_size_t)sizeof(path));

    for (br_u32 depth = 0u; depth <= (br_u32)BR_VFS_SYMLINK_DEPTH; depth++) {
        /* 记下"这次实际在走哪条路径"(展开后会变)。`-ENOENT` 的调用方要它:
         * `O_CREAT` 撞上悬空链接时, 该建的是**目标**, 不是链接名(ADR-0013 §2.5)。 */
        if (final_out != BR_NULL && final_cap > 0u) {
            vfs_str_copy(final_out, path, final_cap);
        }
        const vfs_mount_t *m = mount_match(path);
        if (m == BR_NULL) {
            return BR_ERR(BR_ENOENT);
        }

        const char   *rest     = mount_rest(m, path);
        const br_size_t rest_off = (br_size_t)(rest - path);
        br_inode_t   *cur      = m->root;
        br_bool       cur_is_root = BR_TRUE;
        br_size_t     i        = 0u;
        br_bool       expanded = BR_FALSE;

        while (rest[i] != '\0') {
            br_size_t j = i;
            while (rest[j] != '\0' && rest[j] != '/') {
                j++;
            }
            const br_bool   last = (rest[j] == '\0') ? BR_TRUE : BR_FALSE;
            const br_size_t clen = j - i;

            /* ★ 用 `type` 判"是不是目录", **不**用 `iops == NULL`:
             *   iops 是否为空是各 FS 的**形态约定**(tmpfs 让所有节点共一张表, 表里有
             *   lookup 槽位), 而 type 是语义事实。靠形态约定判语义 ⇒ 同一个错误在不同 FS
             *   下会得到不同的 errno(文件当目录走查会变成 -ENOENT 而不是 -ENOTDIR)。 */
            if (cur->type != BR_INODE_DIR || cur->iops == BR_NULL || cur->iops->lookup == BR_NULL) {
                ino_put(m, cur, cur_is_root);
                return BR_ERR(BR_ENOTDIR);
            }
            if (clen == 0u || clen >= (br_size_t)BR_NAME_MAX) {
                ino_put(m, cur, cur_is_root);
                return BR_ERR(BR_ENOSPC);
            }

            char comp[BR_NAME_MAX];
            for (br_size_t k = 0u; k < clen; k++) {
                comp[k] = rest[i + k];
            }
            comp[clen] = '\0';

            br_inode_t *next = cur->iops->lookup(cur, comp);
            if (next == BR_NULL) {
                ino_put(m, cur, cur_is_root);
                return BR_ERR(BR_ENOENT);
            }

            /* ---- 符号链接展开(末级 nofollow 时跳过) ---- */
            if ((next->type == BR_INODE_SYMLINK) &&
                !((last == BR_TRUE) && (nofollow_last == BR_TRUE))) {
                char tgt[BR_SYMLINK_MAX + 1u];
                int  rl = BR_ERR(BR_ENOTSUP);

                if (next->iops != BR_NULL && next->iops->readlink != BR_NULL) {
                    rl = next->iops->readlink(next, tgt, (br_size_t)BR_SYMLINK_MAX);
                }
                ino_put(m, next, BR_FALSE);
                if (rl < 0) {
                    ino_put(m, cur, cur_is_root);
                    return rl;
                }
                if ((br_size_t)rl > (br_size_t)BR_SYMLINK_MAX) {
                    rl = (int)BR_SYMLINK_MAX;          /* 契约: 截断不报错, 返回全长 */
                }
                tgt[rl] = '\0';

                char      np[BR_VFS_PATH_MAX];
                br_size_t n  = 0u;
                int       pr = BR_OK;

                if (tgt[0] != '/') {
                    /* 相对目标: 前缀 = 链接**所在目录**的路径
                     * (i == 0 时就是挂载路径本身; 否则是 '/' 之前的那一段) */
                    const br_size_t plen = (i == 0u) ? (br_size_t)m->len : (rest_off + i - 1u);
                    for (br_size_t k = 0u; k < plen && pr == BR_OK; k++) {
                        pr = np_push(np, &n, path[k]);
                    }
                    if (pr == BR_OK && (plen == 0u || path[plen - 1u] != '/')) {
                        pr = np_push(np, &n, '/');
                    }
                }
                for (int k = 0; (k < rl) && (pr == BR_OK); k++) {
                    pr = np_push(np, &n, tgt[k]);
                }
                for (br_size_t k = j; (rest[k] != '\0') && (pr == BR_OK); k++) {
                    pr = np_push(np, &n, rest[k]);      /* 剩余分量(自带前导 '/') */
                }
                if (pr == BR_OK) {
                    np[n] = '\0';
                    pr = path_norm(np, path, (br_size_t)sizeof(path));
                    if (pr != BR_OK) {
                        pr = BR_ERR(BR_ENAMETOOLONG);   /* 拼接后超缓冲 ⇒ 语义上的名字过长 */
                    }
                }

                ino_put(m, cur, cur_is_root);
                if (pr != BR_OK) {
                    return pr;
                }
                expanded = BR_TRUE;
                break;                                  /* 重启走查 */
            }

            ino_put(m, cur, cur_is_root);              /* 中间层用完即弃 */
            cur         = next;
            cur_is_root = BR_FALSE;

            if (last == BR_TRUE) {
                break;
            }
            i = j + 1u;
        }

        if (expanded == BR_TRUE) {
            continue;                                   /* 换一条路径重来 */
        }
        *m_out       = m;
        *ino_out     = cur;
        *is_root_out = cur_is_root;
        return BR_OK;
    }

    return BR_ERR(BR_ELOOP);
}

/*
 * 解析"父目录 inode + 末级名字"(mkdir/unlink/rmdir/rename/create/symlink/link 共用)。
 *   `buf`/`cap`: 调用方给的**可写**缓冲(末级名字会被复制进去; `*leaf_out` 指向它)。
 * 中间分量**跟随**符号链接(父目录是链接 ⇒ 落在目标目录里) —— 这是 POSIX 的语义。
 * 错误: `-EINVAL`(路径非法)/ `-EBUSY`(目标是挂载点本身: 没有"末级名字")/
 *       `-ENOENT`(父目录不存在)/ `-ENOTDIR`(父路径上有非目录)/ `-ENOSPC`(名字超长)。
 */
static int resolve_parent(const char *path,
                          char *buf, br_size_t cap,
                          const vfs_mount_t **m_out,
                          br_inode_t **parent_out, br_bool *parent_is_root_out,
                          char **leaf_out)
{
    char norm[BR_VFS_PATH_MAX];
    int  rc = path_norm(path, norm, (br_size_t)sizeof(norm));
    if (rc != BR_OK) {
        return rc;
    }

    const vfs_mount_t *m = mount_match(norm);
    if (m == BR_NULL) {
        return BR_ERR(BR_ENOENT);
    }

    /* 目标是**挂载点本身** ⇒ 没有可操作的"末级名字"。
     * 挂载根 vs 子目录要分清: "/dev/x" 的末级是 x(合法), "/dev" 是挂载点(非法)。 */
    const char *rest = mount_rest(m, norm);
    if (rest[0] == '\0') {
        return BR_ERR(BR_EBUSY);
    }

    const br_size_t rest_off = (br_size_t)(rest - norm);
    br_size_t slash = 0u;
    br_bool   found = BR_FALSE;
    for (br_size_t k = 0u; rest[k] != '\0'; k++) {
        if (rest[k] == '/') {
            slash = k;
            found = BR_TRUE;
        }
    }

    char        parent[BR_VFS_PATH_MAX];
    br_size_t   plen     = 0u;
    const char *leaf_src = BR_NULL;
    br_size_t   llen     = 0u;

    if (found == BR_FALSE) {
        plen     = (br_size_t)m->len;               /* 父 = 挂载路径本身 */
        leaf_src = rest;
    } else {
        plen     = rest_off + slash;                /* 父 = 最后一个 '/' 之前的那一段 */
        leaf_src = rest + slash + 1u;
    }
    for (br_size_t k = 0u; k < plen; k++) {
        parent[k] = norm[k];
    }
    parent[plen] = '\0';
    llen = vfs_str_len(leaf_src);

    rc = resolve_norm(parent, BR_FALSE, m_out, parent_out, parent_is_root_out, BR_NULL, 0u);
    if (rc != BR_OK) {
        return rc;
    }

    if (llen == 0u || llen >= (br_size_t)BR_NAME_MAX) {
        ino_put(*m_out, *parent_out, *parent_is_root_out);
        return BR_ERR(BR_ENOSPC);
    }
    if (llen + 1u > cap) {
        ino_put(*m_out, *parent_out, *parent_is_root_out);
        return BR_ERR(BR_ENOSPC);
    }
    for (br_size_t k = 0u; k < llen; k++) {
        buf[k] = leaf_src[k];
    }
    buf[llen] = '\0';
    *leaf_out = buf;
    return BR_OK;
}

/* 解析到"末级 inode"(stat/lstat/truncate/open/link/readlink 用) */
static int resolve_leaf(const char *path, br_bool nofollow_last,
                        const vfs_mount_t **m_out, br_inode_t **ino_out, br_bool *is_root_out,
                        char *final_out, br_size_t final_cap)
{
    char norm[BR_VFS_PATH_MAX];
    int  rc = path_norm(path, norm, (br_size_t)sizeof(norm));
    if (rc != BR_OK) {
        return rc;
    }
    return resolve_norm(norm, nofollow_last, m_out, ino_out, is_root_out, final_out, final_cap);
}

/* ==================================================================== 挂载点自动建目录
 * 设计 `7-03` §6: "**挂载点在父 FS 缺失时自动 mkdir**" —— 静态组合的便利性优先于
 * 显式 mkdir 仪式(manifest 组合期已校验)。这里**逐级**建(`/a/b` 先建 `/a`)。
 *
 * ADR-0013 起按**规范化全路径的前缀**逐级解析(而不是"从父挂载根下推 rest"): 这样中间
 * 分量上的符号链接也被展开(前一个挂载点可能是一只链接)。挂载点自身**尚未注册**, 所以
 * `mount_match` 命中的是父挂载 —— 正是我们要它落进去的那一个。 */
static int mkdir_p(const char *full_norm)
{
    br_size_t i = 0u;
    while (full_norm[i] == '/') {
        i++;                                            /* 跳过根 '/' */
    }

    while (full_norm[i] != '\0') {
        br_size_t j = i;
        while (full_norm[j] != '\0' && full_norm[j] != '/') {
            j++;
        }

        char pfx[BR_VFS_PATH_MAX];
        for (br_size_t k = 0u; k < j; k++) {
            pfx[k] = full_norm[k];
        }
        pfx[j] = '\0';

        const vfs_mount_t *mm = BR_NULL;
        br_inode_t        *ino = BR_NULL;
        br_bool            ir = BR_FALSE;
        int rc = resolve_norm(pfx, BR_FALSE, &mm, &ino, &ir, BR_NULL, 0u);

        if (rc == BR_ERR(BR_ENOENT)) {
            char        pbuf[BR_VFS_PATH_MAX];
            const vfs_mount_t *pm = BR_NULL;
            br_inode_t *parent = BR_NULL;
            br_bool     pir = BR_FALSE;
            char       *leaf = BR_NULL;

            rc = resolve_parent(pfx, pbuf, (br_size_t)sizeof(pbuf), &pm, &parent, &pir, &leaf);
            if (rc == BR_OK) {
                if (parent->type != BR_INODE_DIR) {
                    rc = BR_ERR(BR_ENOTDIR);
                } else if (parent->iops == BR_NULL || parent->iops->mkdir == BR_NULL) {
                    rc = BR_ERR(BR_ENOTSUP);
                } else {
                    rc = parent->iops->mkdir(parent, leaf);
                    if (rc == BR_ERR(BR_EEXIST)) {
                        rc = BR_OK;                     /* 竞态/已存在 ⇒ 不是错误(原语义) */
                    }
                }
                ino_put(pm, parent, pir);
            }
        } else if (rc == BR_OK) {
            if (ino->type != BR_INODE_DIR) {
                rc = BR_ERR(BR_ENOTDIR);
            }
            ino_put(mm, ino, ir);
        }

        if (rc != BR_OK) {
            return rc;
        }
        if (full_norm[j] == '\0') {
            break;
        }
        i = j + 1u;
    }
    return BR_OK;
}

/* ==================================================================== 层 1: 挂载注册 */

int br_mount_register(const char *path, const br_fs_ops_t *ops, void *fs_priv)
{
    if (ops == BR_NULL || ops->mount == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    char norm[BR_VFS_PATH_MAX];
    int  rc = path_norm(path, norm, (br_size_t)sizeof(norm));
    if (rc != BR_OK) {
        return rc;
    }
    if (s_mount_n >= (br_u32)BR_VFS_MAX_MOUNTS) {
        return BR_ERR(BR_ENOSPC);
    }
    /* 重复挂载同一路径(规范化后比较)⇒ -EEXIST。为什么不是"覆盖": 覆盖会让旧 FS 的
     * 根 inode 与它的 fs_priv 失去唯一持有者 —— 静默泄漏比报错糟。 */
    for (br_u32 i = 0u; i < s_mount_n; i++) {
        if (s_mounts[i].used == BR_TRUE && vfs_str_eq(s_mounts[i].path, norm) == BR_TRUE) {
            return BR_ERR(BR_EEXIST);
        }
    }

    /* ① 挂载点必须落在**父挂载**里, 缺失时逐级建(设计 `7-03` §6) */
    if (vfs_str_eq(norm, "/") == BR_FALSE) {
        const vfs_mount_t *parent = mount_match(norm);
        if (parent == BR_NULL) {
            return BR_ERR(BR_ENODEV);          /* 没有父 FS 承接挂载点 */
        }
        const char *rest = mount_rest(parent, norm);
        if (rest[0] == '\0') {
            return BR_ERR(BR_EEXIST);          /* 与已有挂载点同路径(上面已挡, 纯防御) */
        }
        rc = mkdir_p(norm);                    /* ADR-0013: 按全路径前缀逐级建(展开链接) */
        if (rc != BR_OK) {
            return rc;
        }
    }

    /* ② FS 侧 mount(建根 inode)。cfg.path 给 FS 一份规范化后的自身路径。 */
    const br_fs_cfg_t cfg = {
        .path = norm,
        .dev  = BR_NULL,     /* 介质类 FS(littlefs/EROFS)用; tmpfs/devfs 不需要 */
        .opts = BR_NULL,
    };
    br_inode_t *root = BR_NULL;
    rc = ops->mount(fs_priv, &cfg, &root);
    if (rc != BR_OK) {
        return rc;
    }
    if (root == BR_NULL) {
        return BR_ERR(BR_EINVAL);              /* mount 成功却不给根 inode: FS 违约 */
    }

    vfs_mount_t *m = &s_mounts[s_mount_n];
    m->used = BR_TRUE;
    vfs_str_copy(m->path, norm, (br_size_t)sizeof(m->path));
    m->len  = (br_u32)vfs_str_len(m->path);
    m->ops  = ops;
    m->priv = fs_priv;
    m->root = root;
    s_mount_n++;

    br_log_info("vfs: mount %s (root type=%u)", m->path, root->type);
    return BR_OK;
}

br_u32 br_mount_count(void)
{
    return s_mount_n;
}

const char *br_mount_path_at(br_u32 index)
{
    if (index >= s_mount_n || s_mounts[index].used == BR_FALSE) {
        return BR_NULL;
    }
    return s_mounts[index].path;
}

/* ==================================================================== 统一入口 */

br_file_t *br_open_err(const char *path, br_u32 flags, int *err)
{
    if (err != BR_NULL) {
        *err = BR_OK;
    }

    const br_u32 acc = flags & BR_O_ACCMODE;
    if (acc != BR_O_RDONLY && acc != BR_O_WRONLY && acc != BR_O_RDWR) {
        if (err != BR_NULL) {
            *err = BR_ERR(BR_EINVAL);
        }
        return BR_NULL;
    }
    if ((flags & ~(br_u32)BR_VFS_KNOWN_FLAGS) != 0u) {
        if (err != BR_NULL) {
            *err = BR_ERR(BR_EINVAL);
        }
        return BR_NULL;
    }

    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *ino = BR_NULL;
    br_bool            ino_is_root = BR_FALSE;
    char open_final[BR_VFS_PATH_MAX];
    int rc = resolve_leaf(path, (flags & BR_O_NOFOLLOW) != 0u, &m, &ino, &ino_is_root,
                          open_final, (br_size_t)sizeof(open_final));

    if (rc != BR_OK) {
        /* 未命中 + O_CREAT ⇒ 在父目录上 create(设计 §2 的"未命中且 O_CREAT") */
        if (rc != BR_ERR(BR_ENOENT) || (flags & BR_O_CREAT) == 0u) {
            if (err != BR_NULL) {
                *err = rc;
            }
            return BR_NULL;
        }

        char        buf[BR_VFS_PATH_MAX];
        br_inode_t *parent = BR_NULL;
        br_bool     parent_is_root = BR_FALSE;
        char       *leaf = BR_NULL;

        /* ★ 建在**展开后**的路径上, 不是原始路径上: 若末级是一条**悬空符号链接**,
         *   POSIX 的 `open(link, O_CREAT)` 建的是**目标**(`link` 自己已经存在, 建不了)。
         *   `open_final` 由 resolve_leaf 写下"这次实际走到哪条路径"。 */
        rc = resolve_parent(open_final, buf, (br_size_t)sizeof(buf), &m, &parent,
                            &parent_is_root, &leaf);
        if (rc != BR_OK) {
            if (err != BR_NULL) {
                *err = rc;
            }
            return BR_NULL;
        }
        if (parent->type != BR_INODE_DIR) {
            ino_put(m, parent, parent_is_root);
            if (err != BR_NULL) {
                *err = BR_ERR(BR_ENOTDIR);     /* "父"其实是文件 ⇒ 与"中间级不是目录"同码 */
            }
            return BR_NULL;
        }
        if (parent->iops == BR_NULL || parent->iops->create == BR_NULL) {
            ino_put(m, parent, parent_is_root);
            if (err != BR_NULL) {
                *err = BR_ERR(BR_ENOTSUP);     /* 该 FS 不支持创建(只读 / 设备投影) */
            }
            return BR_NULL;
        }

        br_inode_t *created = BR_NULL;
        rc = parent->iops->create(parent, leaf, flags, &created);
        ino_put(m, parent, parent_is_root);
        if (rc != BR_OK) {
            if (err != BR_NULL) {
                *err = rc;
            }
            return BR_NULL;
        }
        ino         = created;
        ino_is_root = BR_FALSE;
    } else if ((flags & (BR_O_CREAT | BR_O_EXCL)) == (BR_O_CREAT | BR_O_EXCL)) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(BR_EEXIST);
        }
        return BR_NULL;
    }

    /* 末级仍是符号链接 ⇒ 只有一种可能: 调用方给了 O_NOFOLLOW。POSIX 的答复是 -ELOOP
     * (**不是** -ENOTSUP) —— 链接本身没有文件面, 不该被 open 打开。 */
    if (ino->type == BR_INODE_SYMLINK) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(BR_ELOOP);
        }
        return BR_NULL;
    }

    /* 目录只许只读打开(它的"文件面"是目录项流); 写打开 ⇒ -EISDIR。
     * 文件/设备必须有文件面, 否则是"看得见打不开"(v2 的 raw flash/bdev 正属这类)。 */
    if (ino->type == BR_INODE_DIR && (flags & BR_O_ACCMODE) != BR_O_RDONLY) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(BR_EISDIR);
        }
        return BR_NULL;
    }
    if (ino->fops == BR_NULL) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(BR_ENOTSUP);
        }
        return BR_NULL;
    }

    br_file_t *f = (br_file_t *)br_malloc((br_size_t)sizeof(br_file_t));
    if (f == BR_NULL) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(BR_ENOMEM);
        }
        return BR_NULL;
    }
    f->m             = m;
    f->fops          = ino->fops;
    f->ino           = ino;
    f->ino_transient = (ino_is_root == BR_TRUE) ? BR_FALSE : BR_TRUE;
    f->fpriv         = ino->fpriv;      /* inode 携带的 {fops, fpriv}(D21/D23) */
    f->offset        = 0;
    f->flags         = flags;

    if (f->fops->open != BR_NULL) {
        rc = f->fops->open(f, flags);
        if (rc != BR_OK) {
            ino_put(m, ino, ino_is_root);
            br_free(f);
            if (err != BR_NULL) {
                *err = rc;              /* 独占设备的 -EBUSY 从这里上传(策略归驱动) */
            }
            return BR_NULL;
        }
    }

    return f;
}

br_file_t *br_open(const char *path, br_u32 flags)
{
    return br_open_err(path, flags, BR_NULL);
}

/* ==================================================================== 访问器 */

br_inode_t *br_file_inode(br_file_t *f)
{
    return (f != BR_NULL) ? f->ino : BR_NULL;
}

void *br_file_fpriv(br_file_t *f)
{
    return (f != BR_NULL) ? f->fpriv : BR_NULL;
}

void br_file_set_fpriv(br_file_t *f, void *p)
{
    if (f != BR_NULL) {
        f->fpriv = p;
    }
}

br_u32 br_file_flags(br_file_t *f)
{
    return (f != BR_NULL) ? f->flags : 0u;
}

br_s64 br_file_offset(br_file_t *f)
{
    return (f != BR_NULL) ? f->offset : 0;
}

void br_file_set_offset(br_file_t *f, br_s64 off)
{
    if (f != BR_NULL) {
        f->offset = off;
    }
}

/* ==================================================================== 层 3 派发 */

br_s64 br_file_read(br_file_t *f, void *buf, br_size_t n)
{
    if (f == BR_NULL || buf == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (f->fops->read == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return f->fops->read(f, buf, n);
}

br_s64 br_file_write(br_file_t *f, const void *buf, br_size_t n)
{
    if (f == BR_NULL || buf == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (f->fops->write == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return f->fops->write(f, buf, n);
}

br_s64 br_file_lseek(br_file_t *f, br_s64 off, int whence)
{
    if (f == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (f->fops->lseek == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return f->fops->lseek(f, off, whence);
}

int br_file_ioctl(br_file_t *f, br_u32 cmd, void *arg)
{
    if (f == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (f->fops->ioctl == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return f->fops->ioctl(f, cmd, arg);
}

int br_file_fsync(br_file_t *f)
{
    if (f == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (f->fops->fsync == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return f->fops->fsync(f);
}

int br_file_poll(br_file_t *f, br_u32 *events)
{
    if (f == BR_NULL || events == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (f->fops->poll == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    *events = 0u;
    return f->fops->poll(f, events);
}

int br_file_close(br_file_t *f)
{
    if (f == BR_NULL) {
        return BR_OK;                   /* 幂等: 关一个空句柄不是错误 */
    }
    int rc = BR_OK;
    if (f->fops->close != BR_NULL) {
        rc = f->fops->close(f);
    }
    ino_put(f->m, f->ino, (f->ino_transient == BR_TRUE) ? BR_FALSE : BR_TRUE);
    br_free(f);
    return rc;
}

/* ==================================================================== 目录迭代 */

br_dir_t *br_opendir(const char *path)
{
    int err = BR_OK;
    br_file_t *f = br_open_err(path, BR_O_RDONLY, &err);
    if (f == BR_NULL) {
        return BR_NULL;
    }
    if (br_file_inode(f)->type != BR_INODE_DIR) {
        (void)br_file_close(f);
        return BR_NULL;
    }

    br_dir_t *d = (br_dir_t *)br_malloc((br_size_t)sizeof(br_dir_t));
    if (d == BR_NULL) {
        (void)br_file_close(f);
        return BR_NULL;
    }
    d->f = f;
    return d;
}

int br_readdir(br_dir_t *d, br_dirent_t *out)
{
    if (d == BR_NULL || d->f == BR_NULL || out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    const br_s64 rc = br_file_read(d->f, out, (br_size_t)sizeof(*out));
    if (rc < 0) {
        return (int)rc;
    }
    if (rc == 0) {
        return 0;                       /* 目录已穷尽 */
    }
    if (rc != (br_s64)sizeof(*out)) {
        return BR_ERR(BR_EIO);          /* 目录项是定长记录: 半条就是 FS 违约 */
    }
    return 1;
}

int br_closedir(br_dir_t *d)
{
    if (d == BR_NULL) {
        return BR_OK;
    }
    const int rc = br_file_close(d->f);
    br_free(d);
    return rc;
}

/* ==================================================================== 路径级便捷面
 *
 * 为什么要有这一层(设计 `7-01` §1 只写了 `br_open`/`br_file_*`): 目录的名字空间变更
 * (create/mkdir/unlink/rename/getattr)在 ops 表里是"父目录 inode + 名字"两段式,
 * 而**消费者拿不到 inode**(那是瞬态走查产物)。所以必须由 vfs 提供"路径 → 两段式"的
 * 翻译层 —— runtime/posix 的 POSIX 面就是它的 1:1 映射(ADR-0009 §3 裁定 3)。 */

int br_fstat(br_file_t *f, br_stat_t *st)
{
    if (f == BR_NULL || f->ino == BR_NULL || st == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (f->ino->iops == BR_NULL || f->ino->iops->getattr == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return f->ino->iops->getattr(f->ino, st);
}

/* `stat` 跟随末级符号链接, `lstat` 不跟随(POSIX)。中间分量两者都跟随。 */
static int stat_common(const char *path, br_stat_t *st, br_bool nofollow_last)
{
    if (st == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *ino = BR_NULL;
    br_bool            is_root = BR_FALSE;
    int rc = resolve_leaf(path, nofollow_last, &m, &ino, &is_root, BR_NULL, 0u);
    if (rc != BR_OK) {
        return rc;
    }
    if (ino->iops == BR_NULL || ino->iops->getattr == BR_NULL) {
        ino_put(m, ino, is_root);
        return BR_ERR(BR_ENOTSUP);
    }
    rc = ino->iops->getattr(ino, st);
    ino_put(m, ino, is_root);
    return rc;
}

int br_stat(const char *path, br_stat_t *st)
{
    return stat_common(path, st, BR_FALSE);
}

int br_lstat(const char *path, br_stat_t *st)
{
    return stat_common(path, st, BR_TRUE);
}

/* mkdir/rmdir/unlink/rename 的公共骨架: 解析父目录 → 调对应槽位 → 释放父 inode。 */
static int parent_op(const char *path, int (*fn)(br_inode_t *, const char *))
{
    char        buf[BR_VFS_PATH_MAX];
    const vfs_mount_t *m = BR_NULL;
    br_inode_t *parent = BR_NULL;
    br_bool     parent_is_root = BR_FALSE;
    char       *leaf = BR_NULL;

    int rc = resolve_parent(path, buf, (br_size_t)sizeof(buf), &m, &parent,
                            &parent_is_root, &leaf);
    if (rc != BR_OK) {
        return rc;
    }
    if (parent->type != BR_INODE_DIR) {
        ino_put(m, parent, parent_is_root);
        return BR_ERR(BR_ENOTDIR);
    }
    if (parent->iops == BR_NULL || fn == BR_NULL) {
        ino_put(m, parent, parent_is_root);
        return BR_ERR(BR_ENOTSUP);
    }
    rc = fn(parent, leaf);
    ino_put(m, parent, parent_is_root);
    return rc;
}

/* 三个薄适配(只为把 iops 里的函数指针取出来 —— 避免在 parent_op 里做成员判空散落多处) */
static int call_mkdir(br_inode_t *d, const char *name)
{
    return (d->iops->mkdir != BR_NULL) ? d->iops->mkdir(d, name) : BR_ERR(BR_ENOTSUP);
}
static int call_rmdir(br_inode_t *d, const char *name)
{
    return (d->iops->rmdir != BR_NULL) ? d->iops->rmdir(d, name) : BR_ERR(BR_ENOTSUP);
}
static int call_unlink(br_inode_t *d, const char *name)
{
    return (d->iops->unlink != BR_NULL) ? d->iops->unlink(d, name) : BR_ERR(BR_ENOTSUP);
}

int br_mkdir(const char *path)
{
    return parent_op(path, call_mkdir);
}

int br_rmdir(const char *path)
{
    return parent_op(path, call_rmdir);
}

int br_unlink(const char *path)
{
    return parent_op(path, call_unlink);
}

int br_rename(const char *old_path, const char *new_path)
{
    char buf_old[BR_VFS_PATH_MAX];
    char buf_new[BR_VFS_PATH_MAX];
    const vfs_mount_t *m_old = BR_NULL;
    const vfs_mount_t *m_new = BR_NULL;
    br_inode_t *d_old = BR_NULL;
    br_inode_t *d_new = BR_NULL;
    br_bool     root_old = BR_FALSE;
    br_bool     root_new = BR_FALSE;
    char       *leaf_old = BR_NULL;
    char       *leaf_new = BR_NULL;

    int rc = resolve_parent(old_path, buf_old, (br_size_t)sizeof(buf_old), &m_old,
                            &d_old, &root_old, &leaf_old);
    if (rc != BR_OK) {
        return rc;
    }
    rc = resolve_parent(new_path, buf_new, (br_size_t)sizeof(buf_new), &m_new,
                        &d_new, &root_new, &leaf_new);
    if (rc != BR_OK) {
        ino_put(m_old, d_old, root_old);
        return rc;
    }

    /* 跨挂载 rename: v1 不支持(需要"跨 FS 搬运"语义, 而那是 copy+unlink 的组合,
     * 属 v2/v3 的范围)。用 -ENOTSUP 而不是编造 -EXDEV(不在 SD-10 子集里)。 */
    if (m_old != m_new) {
        ino_put(m_old, d_old, root_old);
        ino_put(m_new, d_new, root_new);
        return BR_ERR(BR_ENOTSUP);
    }
    if (d_old->type != BR_INODE_DIR || d_new->type != BR_INODE_DIR) {
        ino_put(m_old, d_old, root_old);
        ino_put(m_new, d_new, root_new);
        return BR_ERR(BR_ENOTDIR);
    }
    if (d_old->iops == BR_NULL || d_old->iops->rename == BR_NULL) {
        ino_put(m_old, d_old, root_old);
        ino_put(m_new, d_new, root_new);
        return BR_ERR(BR_ENOTSUP);
    }

    rc = d_old->iops->rename(d_old, leaf_old, d_new, leaf_new);
    ino_put(m_old, d_old, root_old);
    ino_put(m_new, d_new, root_new);
    return rc;
}

int br_truncate(const char *path, br_u64 size)
{
    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *ino = BR_NULL;
    br_bool            is_root = BR_FALSE;

    int rc = resolve_leaf(path, BR_FALSE, &m, &ino, &is_root, BR_NULL, 0u);   /* truncate 跟随链接 */
    if (rc != BR_OK) {
        return rc;
    }
    if (ino->iops == BR_NULL || ino->iops->setattr == BR_NULL) {
        ino_put(m, ino, is_root);
        return BR_ERR(BR_ENOTSUP);
    }
    const br_stat_t st = { .valid = BR_STAT_SIZE, .type = ino->type, .size = size };
    rc = ino->iops->setattr(ino, &st);
    ino_put(m, ino, is_root);
    return rc;
}

/* ==================================================================== 链接(ADR-0013)

 * 三个入口的共同形状: 名字空间变更 ⇒ 都要"父目录 + 名字"(`resolve_parent`), 于是
 * **建链接时父路径上的符号链接会被展开**(把链接建到链接指向的目录里), 这是 POSIX 语义。
 */

int br_symlink(const char *target, const char *path)
{
    if (target == BR_NULL || target[0] == '\0') {
        return BR_ERR(BR_EINVAL);
    }
    /* 目标**不校验存在性**(悬空链接是合法状态); 但长度有静态上界。 */
    if (vfs_str_len(target) > (br_size_t)BR_SYMLINK_MAX) {
        return BR_ERR(BR_ENAMETOOLONG);
    }

    char               buf[BR_VFS_PATH_MAX];
    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *parent = BR_NULL;
    br_bool            pir = BR_FALSE;
    char              *leaf = BR_NULL;

    int rc = resolve_parent(path, buf, (br_size_t)sizeof(buf), &m, &parent, &pir, &leaf);
    if (rc != BR_OK) {
        return rc;
    }
    if (parent->type != BR_INODE_DIR) {
        ino_put(m, parent, pir);
        return BR_ERR(BR_ENOTDIR);
    }
    if (parent->iops == BR_NULL || parent->iops->symlink == BR_NULL) {
        ino_put(m, parent, pir);
        return BR_ERR(BR_ENOTSUP);           /* 该 FS 不支持符号链接(devfs 等) */
    }
    rc = parent->iops->symlink(parent, leaf, target);
    ino_put(m, parent, pir);
    return rc;
}

int br_readlink(const char *path, char *buf, br_size_t cap)
{
    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *ino = BR_NULL;
    br_bool            ir = BR_FALSE;

    int rc = resolve_leaf(path, BR_TRUE, &m, &ino, &ir, BR_NULL, 0u);   /* readlink **不**跟随末级 */
    if (rc != BR_OK) {
        return rc;
    }
    if (ino->type != BR_INODE_SYMLINK) {
        ino_put(m, ino, ir);
        return BR_ERR(BR_EINVAL);            /* POSIX: 非符号链接 ⇒ EINVAL */
    }
    if (ino->iops == BR_NULL || ino->iops->readlink == BR_NULL) {
        ino_put(m, ino, ir);
        return BR_ERR(BR_ENOTSUP);
    }
    rc = ino->iops->readlink(ino, buf, cap);
    ino_put(m, ino, ir);
    return rc;
}

int br_link(const char *old_path, const char *new_path)
{
    /* POSIX `link(2)`: 末级**不**跟随(链接到链接本身, 而不是链接的目标)。 */
    const vfs_mount_t *m_old = BR_NULL;
    br_inode_t        *target = BR_NULL;
    br_bool            tir = BR_FALSE;

    int rc = resolve_leaf(old_path, BR_TRUE, &m_old, &target, &tir, BR_NULL, 0u);
    if (rc != BR_OK) {
        return rc;
    }
    if (target->type == BR_INODE_DIR) {
        ino_put(m_old, target, tir);
        return BR_ERR(BR_EPERM);             /* 目录不可硬链接(POSIX: EPERM) */
    }

    char               buf[BR_VFS_PATH_MAX];
    const vfs_mount_t *m_new = BR_NULL;
    br_inode_t        *parent = BR_NULL;
    br_bool            pir = BR_FALSE;
    char              *leaf = BR_NULL;

    rc = resolve_parent(new_path, buf, (br_size_t)sizeof(buf), &m_new, &parent, &pir, &leaf);
    if (rc != BR_OK) {
        ino_put(m_old, target, tir);
        return rc;
    }

    /* 跨挂载的硬链接不存在(v1 无"跨 FS inode"概念)⇒ -EXDEV。
     * ★ 这是 `BR_EXDEV`(18)的**首个启用点** —— 它一直在错误码表里, 到这里才有语义。 */
    if (m_old != m_new) {
        ino_put(m_old, target, tir);
        ino_put(m_new, parent, pir);
        return BR_ERR(BR_EXDEV);
    }
    if (parent->type != BR_INODE_DIR) {
        ino_put(m_old, target, tir);
        ino_put(m_new, parent, pir);
        return BR_ERR(BR_ENOTDIR);
    }
    if (parent->iops == BR_NULL || parent->iops->link == BR_NULL) {
        ino_put(m_old, target, tir);
        ino_put(m_new, parent, pir);
        return BR_ERR(BR_ENOTSUP);
    }

    rc = parent->iops->link(parent, leaf, target);
    ino_put(m_old, target, tir);
    ino_put(m_new, parent, pir);
    return rc;
}

/* ==================================================================== 自检用的内部视图
 *
 * 见 `src/vfs_internal.h` 的说明: 这几条**只**为 `vfs_selftest.c` 存在 —— 它们的被测
 * 对象就是这里的机制本身(挂载表 / 最长前缀匹配 / 路径规范化), 用公开 API 表达不出来。
 * ★ 一律**转调**生产实现, 不复制逻辑: 用例必须跑真算法, 否则验证的是副本。
 */
const vfs_mount_internal_t *br_vfs_internal_mounts(br_u32 *n)
{
    if (n != BR_NULL) {
        *n = s_mount_n;
    }
    return (const vfs_mount_internal_t *)(const void *)s_mounts;
}

br_u32 br_vfs_internal_mount_match(const char *norm)
{
    const vfs_mount_t *m = mount_match(norm);
    if (m == BR_NULL) {
        return (br_u32)-1;
    }
    return (br_u32)(m - &s_mounts[0]);
}

int br_vfs_internal_path_norm(const char *in, char *out, br_size_t cap)
{
    return path_norm(in, out, cap);
}

br_bool br_vfs_internal_str_eq(const char *a, const char *b)
{
    return vfs_str_eq(a, b);
}

/* ==================================================================== 生命周期 */

int vfs_core_early_init(void)
{
    return 0;   /* EARLY: 静态表是 BSS; 挂载要等各 FS 的 CORE init(那时堆已就绪) */
}

int vfs_core_init(void)
{
    /* vfs-core 自己没有要初始化的状态: 它的"上场"是各 FS 调 `br_mount_register`。
     * 刻意不造空 API(R-S4: 框架件往里的每一行都要有消费者)。 */
    return 0;
}

int vfs_core_start(void)
{
    /* START 相只留一行挂载表摘要作启动证据。
     * ★ 存储域的**总套件**(TC-VFS-*)不在这里跑: 它在 `src/vfs_selftest.c`, 由 core 的
     *   自检 pass 在**全部 start 之后**统一驱动(ADR-0010)。ADR-0009 §2.5 曾把它挂在
     *   APP 的 start 上(那时没有统一的自检时机), 那一版随 ADR-0010 作废。 */
    br_log_info("vfs: %u mount(s) active", s_mount_n);
    for (br_u32 i = 0u; i < s_mount_n; i++) {
        br_log_info("vfs:   [%u] %s", i, s_mounts[i].path);
    }
    return 0;
}

