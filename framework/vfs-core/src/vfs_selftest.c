/*
 * brickOS prototype v0.2.0 — vfs-core 的**自检套件**(framework/vfs-core/src/vfs_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与核心代码分离)。
 *
 * ## 它测什么, 与生产代码的边界在哪
 *   存储域**总套件**(自编号 `TC-VFS-*`): 只经 `br/vfs/br_vfs.h` 的**公开 API** 驱动 ——
 *   它不认任何 FS 的名字, 也不调任何 FS/设备的内部函数。"rootfs 是 tmpfs"这件事对它只是
 *   "挂载表里有一个 '/'"; "/dev 下能列出设备"对它只是"某个挂载里有一些文件节点"。
 *   ⇒ 套件随组合自动伸缩: 不选 fs/devfs 就没有 /dev 那几条(如实报"不适用", 不是假绿)。
 *
 *   **例外只有一处**: TC-VFS-002 要断言"每个挂载点的路径经最长前缀匹配都命中它自己",
 *   而被测对象就是匹配算法本身 ⇒ 它经 `src/vfs_internal.h` 的访问器读挂载表/调匹配函数。
 *   那些访问器一律**转调生产实现**(不复制逻辑), 理由见该头文件。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[VFSCONF] PASS/FAIL ...` + `SUMMARY`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[VFSCONF] FAIL`)。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "vfs_internal.h"

/* 生命周期钩子的原型在生产文件里(它们不属自检); 本文件只实现 `vfs_core_selftest`。 */
int vfs_core_selftest(void);

/* ==================================================================== 一致性用例
 *
 * 存储域**总套件**: 只经 `br_vfs.h` 的公开 API 驱动 —— 它不认任何 FS 的名字, 也不
 * 直接调任何 FS/设备的内部函数。"rootfs 是 tmpfs"这件事对它只是"挂载表里有一个 '/'";
 * "/dev 下能列出设备"对它只是"某个挂载里有一些文件节点"。
 * ⇒ 套件随组合自动伸缩: 不选 fs/devfs 就没有 /dev 那几条(如实报"不适用", 不是假绿)。
 */
static br_u32 s_pass;
static br_u32 s_fail;

static void pc(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[VFSCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[VFSCONF] FAIL %s %s", tag, what);
    }
}

/* 拼 "base + '/' + name"(base = "/" 时不产生 "//") */
static void conf_path(char *dst, br_size_t cap, const char *base, const char *name)
{
    br_size_t n = 0u;
    for (br_size_t i = 0u; base[i] != '\0' && n + 1u < cap; i++) {
        dst[n++] = base[i];
    }
    if (n > 0u && dst[n - 1u] != '/' && n + 1u < cap) {
        dst[n++] = '/';
    }
    for (br_size_t i = 0u; name[i] != '\0' && n + 1u < cap; i++) {
        dst[n++] = name[i];
    }
    dst[n] = '\0';
}

/* 统计一个目录的条目数; `skip_dots` = 跳过 "." / ".." */
static int conf_count_dir(const char *path, br_u32 *total, br_u32 *files)
{
    br_dir_t *d = br_opendir(path);
    if (d == BR_NULL) {
        return BR_ERR(BR_ENOENT);
    }
    br_u32 n = 0u;
    br_u32 nf = 0u;
    for (;;) {
        br_dirent_t de;
        const int rc = br_readdir(d, &de);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            (void)br_closedir(d);
            return rc;
        }
        n++;
        if (de.type == BR_INODE_FILE) {
            nf++;
        }
    }
    (void)br_closedir(d);
    if (total != BR_NULL) {
        *total = n;
    }
    if (files != BR_NULL) {
        *files = nf;
    }
    return BR_OK;
}

int vfs_core_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[VFSCONF] vfs-core conformance (挂载表 / 走查链 / 文件面 / 目录面)");

    /* ---- TC-VFS-001: 挂载表非空, 且有一个根挂载(单路由的兜底, 设计 §1) ---- */
    br_u32 root_idx = (br_u32)-1;
    {
        br_bool have_root = BR_FALSE;
        br_bool paths_ok  = BR_TRUE;
        for (br_u32 i = 0u; i < br_mount_count(); i++) {
            const char *p = br_mount_path_at(i);
            if (p == BR_NULL || p[0] != '/') {
                paths_ok = BR_FALSE;
                break;
            }
            if (br_vfs_internal_str_eq(p, "/") == BR_TRUE) {
                have_root = BR_TRUE;
            }
        }
        pc((br_mount_count() > 0u) && have_root && paths_ok &&
           (br_mount_path_at(br_mount_count()) == BR_NULL),
           "TC-VFS-001", "挂载表非空 / 含根挂载 '/' / 路径均绝对 / 越界取路径 ⇒ NULL");

        /* 找到根挂载供后续用例使用; 找不到就别往下跑(否则会级联出一片 FAIL) */
        for (br_u32 i = 0u; i < br_mount_count(); i++) {
            const char *p = br_mount_path_at(i);
            if (p != BR_NULL && br_vfs_internal_str_eq(p, "/") == BR_TRUE) {
                root_idx = i;
                break;
            }
        }
    }
    if (root_idx == (br_u32)-1) {
        br_log_info("[VFSCONF] note: 没有根挂载 ⇒ 依赖 rootfs 的用例全部不适用(不假绿: 已记 FAIL)");
        br_log_info("[VFSCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
        return (int)s_fail;
    }
    const char *base = "/";     /* 上面已断言存在 '/' 挂载 */

    /* ---- TC-VFS-002: 最长前缀匹配 + 重复挂载 ⇒ -EEXIST ---- */
    {
        br_bool ok = BR_TRUE;
        /* 逐条: 每个挂载点的路径经**最长前缀匹配**必须命中它自己(没人比它更长)。
         * 这两条都走内部访问器 —— 被测对象就是匹配算法本身, 见 src/vfs_internal.h。 */
        br_u32 n = 0u;
        const vfs_mount_internal_t *ms = br_vfs_internal_mounts(&n);
        for (br_u32 i = 0u; i < n; i++) {
            if (br_vfs_internal_mount_match(ms[i].path) != i) {
                ok = BR_FALSE;
            }
        }
        /* 重复挂载同一路径: 用**已经不成功**的方式验证(不污染表) —— 拿现有挂载的
         * fs_ops 再注册一次, 期望 -EEXIST。 */
        const int r = br_mount_register("/", br_vfs_internal_mounts(BR_NULL)[root_idx].ops,
                                        br_vfs_internal_mounts(BR_NULL)[root_idx].priv);
        if (r != BR_ERR(BR_EEXIST)) {
            ok = BR_FALSE;
        }
        /* 非法参数: 空 ops / 相对路径 */
        if (br_mount_register("/x", BR_NULL, BR_NULL) != BR_ERR(BR_EINVAL) ||
            br_mount_register(BR_NULL, br_vfs_internal_mounts(BR_NULL)[root_idx].ops, BR_NULL) != BR_ERR(BR_EINVAL) ||
            br_mount_register("relative", br_vfs_internal_mounts(BR_NULL)[root_idx].ops, BR_NULL) != BR_ERR(BR_EINVAL)) {
            ok = BR_FALSE;
        }
        pc(ok, "TC-VFS-002", "最长前缀匹配(自个路径命中自个); 重复挂载 -EEXIST; 非法参数 -EINVAL");
    }

    /* ---- 建一个自用的临时目录(不依赖 tmpfs 预建的 /dev /data /tmp) ---- */
    char dir[BR_VFS_PATH_MAX_INTERNAL];
    char file[BR_VFS_PATH_MAX_INTERNAL];
    char file2[BR_VFS_PATH_MAX_INTERNAL];
    conf_path(dir,   (br_size_t)sizeof(dir),   base, "vfsconf.d");
    conf_path(file,  (br_size_t)sizeof(file),  dir,  "a.bin");
    conf_path(file2, (br_size_t)sizeof(file2), dir,  "b.bin");

    (void)br_unlink(file);          /* 清上次运行的残留(失败无所谓: 本来就可能不存在) */
    (void)br_unlink(file2);
    (void)br_unlink(dir);
    (void)br_rmdir(dir);

    /* ---- TC-VFS-003: mkdir / 幂等冲突 / 目录上做文件操作 ---- */
    {
        br_bool ok = (br_mkdir(dir) == BR_OK) && (br_mkdir(dir) == BR_ERR(BR_EEXIST));
        br_stat_t st;
        ok = ok && (br_stat(dir, &st) == BR_OK) && (st.type == BR_INODE_DIR);
        /* 目录写打开 ⇒ -EISDIR */
        int err = BR_OK;
        br_file_t *f = br_open_err(dir, BR_O_WRONLY, &err);
        ok = ok && (f == BR_NULL) && (err == BR_ERR(BR_EISDIR));
        /* 把文件当目录走查 ⇒ -ENOTDIR(先造一个文件) */
        int err2 = BR_OK;
        br_file_t *fa = br_open_err(file, BR_O_CREAT | BR_O_RDWR, &err2);
        ok = ok && (fa != BR_NULL);
        if (fa != BR_NULL) {
            (void)br_file_close(fa);
            char deep[BR_VFS_PATH_MAX_INTERNAL];
            conf_path(deep, (br_size_t)sizeof(deep), file, "x");
            int err3 = BR_OK;
            br_file_t *fd = br_open_err(deep, BR_O_RDONLY, &err3);
            ok = ok && (fd == BR_NULL) && (err3 == BR_ERR(BR_ENOTDIR));
        }
        pc(ok, "TC-VFS-003", "mkdir/重复 -EEXIST; stat 报 DIR; 目录写打开 -EISDIR; 文件当目录 -ENOTDIR");
    }

    /* ---- TC-VFS-004: 文件往返(create → write×3 → lseek → read → stat) ---- */
    {
        static const char p1[] = "brickOS ";
        static const char p2[] = "vfs ";
        static const char p3[] = "roundtrip";
        const br_size_t  want = (br_size_t)(sizeof(p1) - 1u + sizeof(p2) - 1u + sizeof(p3) - 1u);

        int err = BR_OK;
        br_file_t *f = br_open_err(file, BR_O_CREAT | BR_O_RDWR | BR_O_TRUNC, &err);
        br_bool ok = (f != BR_NULL);
        if (ok == BR_TRUE) {
            ok = (br_file_write(f, p1, sizeof(p1) - 1u) == (br_s64)(sizeof(p1) - 1u)) &&
                 (br_file_write(f, p2, sizeof(p2) - 1u) == (br_s64)(sizeof(p2) - 1u)) &&
                 (br_file_write(f, p3, sizeof(p3) - 1u) == (br_s64)(sizeof(p3) - 1u));
            /* SEEK_END ⇒ 文件长度 */
            ok = ok && (br_file_lseek(f, 0, BR_SEEK_END) == (br_s64)want);
            ok = ok && (br_file_lseek(f, 0, BR_SEEK_SET) == 0);
            ok = ok && (br_file_offset(f) == 0);

            char   back[32];
            const br_s64 n = br_file_read(f, back, (br_size_t)sizeof(back));
            ok = ok && (n == (br_s64)want);
            if (n == (br_s64)want) {
                static const char whole[] = "brickOS vfs roundtrip";
                ok = ok && br_vfs_internal_mem_eq(back, whole, want);
            }
            /* 读到 EOF 之后再读 ⇒ 0(而不是 -EIO / 死循环) */
            ok = ok && (br_file_read(f, back, 1u) == 0);
            pc(ok, "TC-VFS-004", "文件往返: 三段写 / SEEK_END / SEEK_SET / 读回逐字节一致 / EOF 返回 0");

            ok = (br_file_close(f) == BR_OK);
            br_stat_t st;
            ok = ok && (br_stat(file, &st) == BR_OK) &&
                 (st.type == BR_INODE_FILE) && (st.size == (br_u64)want);
            pc(ok, "TC-VFS-005", "close 返回 0; stat 的类型/长度与写入一致");
        } else {
            pc(BR_FALSE, "TC-VFS-004", "文件往返: 打开失败(见上一行 err)");
            pc(BR_FALSE, "TC-VFS-005", "close/stat: 前置失败");
        }
    }

    /* ---- TC-VFS-006: 打开语义的负例(O_EXCL / 不存在 / 空参数 / 相对路径) ---- */
    {
        int e1 = BR_OK, e2 = BR_OK, e3 = BR_OK, e4 = BR_OK;
        br_file_t *f1 = br_open_err(file, BR_O_CREAT | BR_O_EXCL | BR_O_RDWR, &e1);
        br_file_t *f2 = br_open_err("/vfsconf_no_such_file", BR_O_RDONLY, &e2);
        br_file_t *f3 = br_open_err(BR_NULL, BR_O_RDONLY, &e3);
        br_file_t *f4 = br_open_err("relative/path", BR_O_RDONLY, &e4);
        const br_bool ok = (f1 == BR_NULL) && (e1 == BR_ERR(BR_EEXIST)) &&
                           (f2 == BR_NULL) && (e2 == BR_ERR(BR_ENOENT)) &&
                           (f3 == BR_NULL) && (e3 == BR_ERR(BR_EINVAL)) &&
                           (f4 == BR_NULL) && (e4 == BR_ERR(BR_EINVAL));
        pc(ok, "TC-VFS-006", "O_CREAT|O_EXCL 已存在 -EEXIST; 不存在 -ENOENT; NULL/相对路径 -EINVAL");
    }

    /* ---- TC-VFS-007: 目录迭代(枚举到刚建的文件, 且条目定长可读尽) ---- */
    {
        br_u32 total = 0u, files = 0u;
        const int rc = conf_count_dir(dir, &total, &files);
        br_bool ok = (rc == BR_OK) && (total == 3u) && (files == 1u);
        pc(ok, "TC-VFS-007", "readdir: '.' + '..' + 1 个文件 = 3 条, 其中 FILE 类 1 条");
    }

    /* ---- TC-VFS-008: 重命名 + 截断 ---- */
    {
        br_bool ok = (br_rename(file, file2) == BR_OK);
        br_stat_t st;
        ok = ok && (br_stat(file2, &st) == BR_OK) && (st.size > 0u);
        ok = ok && (br_stat(file, &st) == BR_ERR(BR_ENOENT));

        ok = ok && (br_truncate(file2, 7u) == BR_OK) &&
             (br_stat(file2, &st) == BR_OK) && (st.size == 7u);
        int err = BR_OK;
        br_file_t *f = br_open_err(file2, BR_O_RDONLY, &err);
        if (ok == BR_TRUE && f != BR_NULL) {
            char back[8];
            const br_s64 n = br_file_read(f, back, (br_size_t)sizeof(back));
            ok = (n == 7) && br_vfs_internal_mem_eq(back, "brickOS", 7u);
        } else {
            ok = BR_FALSE;
        }
        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
        /* O_TRUNC 打开 ⇒ 长度归零 */
        int et = BR_OK;
        br_file_t *ft = br_open_err(file2, BR_O_WRONLY | BR_O_TRUNC, &et);
        ok = ok && (ft != BR_NULL);
        if (ft != BR_NULL) {
            (void)br_file_close(ft);
        }
        ok = ok && (br_stat(file2, &st) == BR_OK) && (st.size == 0u);
        pc(ok, "TC-VFS-008", "rename 旧名消失/新名在; truncate(7) 内容保留; O_TRUNC 归零");
    }

    /* ---- TC-VFS-009: lseek 边界 + 句柄访问器 ---- */
    {
        int err = BR_OK;
        br_file_t *f = br_open_err(file2, BR_O_RDWR, &err);
        br_bool ok = (f != BR_NULL);
        if (f != BR_NULL) {
            ok = ok && (br_file_lseek(f, -1, BR_SEEK_SET) == BR_ERR(BR_EINVAL));
            ok = ok && (br_file_lseek(f, 0, BR_SEEK_END) == 0);
            ok = ok && (br_file_lseek(f, 1, 99 /* 非法 whence */) == BR_ERR(BR_EINVAL));
            ok = ok && (br_file_inode(f) != BR_NULL) &&
                 (br_file_inode(f)->type == BR_INODE_FILE) &&
                 ((br_file_flags(f) & BR_O_ACCMODE) == BR_O_RDWR);
            ok = ok && (br_file_close(f) == BR_OK);
        }
        pc(ok, "TC-VFS-009", "lseek: 负偏移 -EINVAL / 非法 whence -EINVAL; 访问器与打开参数一致");
    }

    /* ---- TC-VFS-010: unlink / rmdir(含负例) ---- */
    {
        br_bool ok = (br_unlink(file2) == BR_OK) &&
                     (br_unlink(file2) == BR_ERR(BR_ENOENT)) &&
                     (br_stat(file2, &(br_stat_t){0}) == BR_ERR(BR_ENOENT)) &&
                     (br_rmdir(dir) == BR_OK) &&
                     (br_rmdir(dir) == BR_ERR(BR_ENOENT));
        pc(ok, "TC-VFS-010", "unlink/rmdir 成功且幂等语义正确(重复操作 ⇒ -ENOENT)");
    }

    /* ---- TC-VFS-011: 空句柄/空参数健壮性(不崩、返回契约错误码) ---- */
    {
        char byte = 0;
        const br_bool ok =
            (br_file_read(BR_NULL, &byte, 1u) == BR_ERR(BR_EINVAL)) &&
            (br_file_write(BR_NULL, &byte, 1u) == BR_ERR(BR_EINVAL)) &&
            (br_file_lseek(BR_NULL, 0, BR_SEEK_SET) == BR_ERR(BR_EINVAL)) &&
            (br_file_ioctl(BR_NULL, 0u, BR_NULL) == BR_ERR(BR_EINVAL)) &&
            (br_file_fsync(BR_NULL) == BR_ERR(BR_EINVAL)) &&
            (br_file_close(BR_NULL) == BR_OK) &&
            (br_readdir(BR_NULL, BR_NULL) == BR_ERR(BR_EINVAL)) &&
            (br_closedir(BR_NULL) == BR_OK) &&
            (br_opendir("/vfsconf_no_such_dir") == BR_NULL) &&
            (br_stat(BR_NULL, &(br_stat_t){0}) == BR_ERR(BR_EINVAL)) &&
            (br_mkdir(BR_NULL) == BR_ERR(BR_EINVAL)) &&
            (br_truncate(BR_NULL, 0u) == BR_ERR(BR_EINVAL));
        pc(ok, "TC-VFS-011", "空句柄/空路径: 返回 -EINVAL; close(NULL)/closedir(NULL) 幂等 0");
    }

    /* ---- TC-VFS-012: 挂载点不可删除(v1: 那需要卸载语义, O-S3/v3) ---- */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 i = 0u; i < br_mount_count(); i++) {
            const char *p = br_mount_path_at(i);
            if (p == BR_NULL || br_vfs_internal_str_eq(p, "/") == BR_TRUE) {
                continue;               /* 根挂载没?"/.." 的父; 单独跳过 */
            }
            if (br_rmdir(p) != BR_ERR(BR_EBUSY) || br_unlink(p) != BR_ERR(BR_EBUSY)) {
                ok = BR_FALSE;
            }
        }
        pc(ok, "TC-VFS-012", "挂载点本身不可 rmdir/unlink ⇒ -EBUSY(卸载不早于 v3)");
    }

    /* ---- TC-VFS-013: 非根挂载(/dev)的目录面 + "看得见打不开"的诚实回答 ----
     * 这条是 devfs + cdev 适配层的**泛型**验证: 只经 br_opendir/br_stat/br_open。
     * 不认任何设备名 —— 认设备名是用例该在**器件插件**里做的事(见 io/uart-pl011)。 */
    {
        const char *dev = BR_NULL;
        for (br_u32 i = 0u; i < br_mount_count(); i++) {
            const char *p = br_mount_path_at(i);
            if (p != BR_NULL && br_vfs_internal_str_eq(p, "/") == BR_FALSE) {
                dev = p;
                break;
            }
        }
        if (dev == BR_NULL) {
            br_log_info("[VFSCONF] note: 组合里没有非根挂载 ⇒ TC-VFS-013 不适用");
        } else {
            br_u32 total = 0u, files = 0u;
            br_bool ok = (conf_count_dir(dev, &total, &files) == BR_OK) && (total >= 2u);
            br_dir_t *d = br_opendir(dev);
            br_u32 checked = 0u;
            if (d != BR_NULL) {
                for (;;) {
                    br_dirent_t de;
                    const int rc = br_readdir(d, &de);
                    if (rc <= 0) {
                        break;
                    }
                    if (br_vfs_internal_str_eq(de.name, ".") == BR_TRUE || br_vfs_internal_str_eq(de.name, "..") == BR_TRUE) {
                        continue;
                    }
                    char p[BR_VFS_PATH_MAX_INTERNAL];
                    conf_path(p, (br_size_t)sizeof(p), dev, de.name);

                    br_stat_t st;
                    ok = ok && (br_stat(p, &st) == BR_OK);

                    int err = BR_OK;
                    br_file_t *f = br_open_err(p, BR_O_RDONLY | BR_O_NONBLOCK, &err);
                    if (f != BR_NULL) {
                        ok = ok && (br_file_close(f) == BR_OK);     /* 有文件面: 打开/关闭成对 */
                    } else {
                        /* 没有文件面(如 v1 的 raw flash)必须是 -ENOTSUP —— 别的错误码
                         * 说明适配层或节点构造有问题, 判红(不放宽)。 */
                        ok = ok && (err == BR_ERR(BR_ENOTSUP));
                    }
                    checked++;
                }
                (void)br_closedir(d);
            } else {
                ok = BR_FALSE;
            }
            br_log_info("[VFSCONF] %s 下具体设备节点 %u 个(open: 成功=有文件面 / -ENOTSUP=只看不打)",
                        dev, checked);
            pc(ok, "TC-VFS-013", "非根挂载: readdir 可枚举, 每个节点 stat 成功且 open 只回 0/-ENOTSUP");
        }
    }

    br_log_info("[VFSCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
