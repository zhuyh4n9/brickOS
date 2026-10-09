/*
 * brickOS prototype v0.2.0 — POSIX 运行时(runtime/posix 插件)
 *
 * 设计依据(设计仓库 `brickOS-Design`):
 *   - `1-01-architecture.md` §7(v0.5/D18: **POSIX 双角色拆分** —— `runtime/posix` 是实现
 *     (普通服务), `iface/posix` 是薄皮肤; §7.6 三方移植双模式);
 *   - `11-01-service.md` §1/§2(POSIX 运行时服务: fd 表主人 / `errno = -ret` 零转换);
 *   - `docs/decisions/0015-runtime-posix-rename.md`(**本插件为何叫 runtime/posix**);
 *   - `11-02-svc-posix-subset.md`(**本件的覆盖清单**: TR-A/TR-B 是实现范围, TR-C/TR-D
 *     明确不做 —— "子集诚实义务"的落地; 该篇仍用设计侧的名字 `runtime/posix`);
 *   - `7-01-vfs.md` §1/§4.1(fd 表项 = `br_file_t*`; `br_open` 是 fd 表的底层原语);
 *   - `3-01-core-api-list.md` §2/§3/§4/§7(线程 / 同步 / 时间 / 内存 —— 本件只做映射)。
 * 裁定与偏离: `docs/decisions/0014-svc-posix-posix-runtime.md`。
 *
 * ============================== 本件是什么 ==============================
 * 一层**翻译**, 不是第二个内核。每个 POSIX 函数都只有三种形状之一:
 *
 *   ① **零转换委托**   `read()` → `br_ft_get` + `br_file_read`, `errno = -ret`
 *   ② **形状转换**     `struct stat` ← `br_stat_t`; `struct dirent` ← `br_dirent_t`
 *   ③ **本服务自己的状态**  fd 表(主人在 framework/file-table)、**cwd**、errno、
 *                          `FILE*` 池、pthread 的 retval/栈表、记录锁表
 *
 * 第 ③ 类的每一样都在 `11-02` 的 TR-B 行里 —— 它们是设计已明确归 POSIX 运行时的职责
 * (cwd/相对路径、fd 层访问模式执法), 不是本实现顺手多做的。
 *
 * ============================== 三条承重口径 ==============================
 * 1. **`errno = -ret`(零转换)**: core 与 vfs 的错误码**就是**内核编号(ADR-0012 逐码
 *    对拍), 所以本文件里没有 errno 换算表 —— 只有 `-rc`。SD-10 与 D18 的红利。
 * 2. **fd 表不在这里**: 唯一主人在 `framework/file-table`(ADR-0012)。本件只消费它 ⇒
 *    "共享状态单一主人"可以被 grep 出来(`br_ft_alloc` 只出现在本文件里)。
 * 3. **访问模式执法归本件**: vfs **刻意不执法**(ADR-0009 §5 遗留项 5), 所以"只读 fd 上
 *    write ⇒ EBADF"必须在这里有, 否则整个 POSIX 面就没有权限语义。
 *
 * ============================== 已知欠账(诚实清单) ==============================
 * 全部条目见 `docs/decisions/0014-…` 与 `0019-…` §4。最要紧的四条:
 *   - `errno` 已改为**每线程**(本层线程记录里的槽位, `__br_posix_errno()`);
 *   - `struct stat` 的 `st_ino/st_uid/st_gid/st_*time` **恒 0**(vfs 没有这些面);
 *   - `printf` 家族**不在本代**(TR-C: libc 选型未拍, `11-01` §3);
 *   - `pthread_detach` = **惰性回收**(下一次 pthread 调用时 join 掉已退出的 detached
 *     线程; core 只有 join 一条回收路径, ADR-0019)。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <stdarg.h>

#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>
#include <br/ft/br_ft.h>
#include <br/vfs/br_vfs.h>

/* 生命周期钩子(名字 = symbol_prefix(short = "posix") + 相; 由 build/gen 的描述符引用)。 */
int posix_early_init(void);
int posix_init(void);
int posix_start(void);

/* ==================================================================== 静态上界 */

#define SVC_PATH_MAX        128u   /* 与 vfs 的路径缓冲同量级; 超 ⇒ -ENAMETOOLONG */
#define SVC_FILE_MAX        8u     /* 同时打开的 FILE* 上界 */
#define SVC_THREAD_MAX      8u     /* 与 core 的 BR_TASK_MAX 对齐(core 是 TCB 池的真实上界) */
#define SVC_LOCK_MAX        16u    /* 记录锁条目上界 */
#define SVC_SELECT_MAX      16u    /* select 一次能管的 fd 数(nfds 上界) */
#define SVC_KEY_MAX         PTHREAD_KEYS_MAX   /* TLS key 槽位数(与 pthread.h 一致) */

/* `errno` 现在**不是**一个全局 int: 它是 `errno.h` 里的宏, 指向每线程槽位
 * (`__br_posix_errno()`, 实现见下面的 pthread 层)。定义全局 int 会让"线程 A 的失败
 * 被线程 B 读走"复活, 而且与 POSIX"errno 是宏"的规定冲突。 */

/* cwd: POSIX 概念, 归本服务(vfs 只认绝对路径, `7-01` §2)。始终以 '/' 开头。 */
static char s_cwd[SVC_PATH_MAX] = "/";
static mode_t s_umask = 0u;                 /* 只记录(不执法), 见 umask() */

/* ==================================================================== 小工具 */

static size_t svc_strlen(const char *s)
{
    size_t n = 0u;
    if (s == BR_NULL) {
        return 0u;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void svc_copy(char *dst, const char *src, size_t cap)
{
    size_t i = 0u;
    if (cap == 0u) {
        return;
    }
    for (; src != BR_NULL && src[i] != '\0' && (i + 1u) < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 失败统一走这里: `rc` 是 core/vfs 的**负 errno**, 直接取负即 errno(零转换)。 */
static int svc_fail(int rc)
{
    errno = (rc < 0) ? -rc : rc;
    return -1;
}

/*
 * ★ 两个错误通道, 别混(POSIX 自己就不一致, 必须照抄):
 *   - **系统调用族**(open/read/write/…)失败返回 `-1` 并设 `errno`  ⇒ `svc_fail`
 *   - **pthread_* 族**直接把**错误号当返回值**(不碰 errno)          ⇒ `svc_rc`
 *   用错通道的症状很隐蔽: 调用方写 `if (pthread_mutex_lock(m) != 0)`, 若我们返回 -1,
 *   它会把 -1 当成"错误号 -1"去查表 —— 永远查不到, 且 errno 那条路也没人读。
 */
static int svc_rc(int rc)
{
    return (rc < 0) ? -rc : rc;
}

/* ==================================================================== 路径处理
 *
 * 两条路:
 *   - `svc_abs()`: 相对 → 绝对(**不**解析 "."/".."; 那是 FS 的 lookup 语义, 见
 *     br_vfs.c 的 path_norm 说明)。给 vfs 用。
 *   - `svc_clean()`: 只给 `chdir` 用 —— 把 "."/".." 在**字符串层面**折掉, 于是
 *     `getcwd` 报出来的是干净路径(否则会出现 "/a/../.." 这种"合法但难看"的 cwd)。
 */

static int svc_abs(const char *path, char *out, size_t cap)
{
    if (path == BR_NULL || path[0] == '\0') {
        return BR_ERR(BR_EINVAL);
    }
    if (path[0] == '/') {
        if (svc_strlen(path) + 1u > cap) {
            return BR_ERR(BR_ENAMETOOLONG);
        }
        svc_copy(out, path, cap);
        return BR_OK;
    }

    const size_t cl = svc_strlen(s_cwd);
    const size_t pl = svc_strlen(path);
    const int    need_slash = (cl > 0u && s_cwd[cl - 1u] != '/') ? 1 : 0;
    if ((cl + (size_t)need_slash + pl + 1u) > cap) {
        return BR_ERR(BR_ENAMETOOLONG);
    }
    size_t n = 0u;
    for (size_t i = 0u; i < cl; i++) {
        out[n++] = s_cwd[i];
    }
    if (need_slash != 0) {
        out[n++] = '/';
    }
    for (size_t i = 0u; i < pl; i++) {
        out[n++] = path[i];
    }
    out[n] = '\0';
    return BR_OK;
}

/* "." / ".." 的字符串折叠(`chdir` 专用; 根的上层仍是根)。 */
static void svc_clean(const char *in, char *out, size_t cap)
{
    size_t n = 0u;
    size_t i = 0u;

    out[n++] = '/';
    while (in[i] != '\0' && (n + 1u) < cap) {
        while (in[i] == '/') {
            i++;
        }
        size_t start = i;
        while (in[i] != '\0' && in[i] != '/') {
            i++;
        }
        const size_t len = i - start;
        if (len == 0u) {
            break;
        }
        if (len == 1u && in[start] == '.') {
            continue;
        }
        if (len == 2u && in[start] == '.' && in[start + 1u] == '.') {
            if (n > 1u) {
                n--;                        /* 吃掉分隔 '/' */
                while (n > 1u && out[n - 1u] != '/') {
                    n--;
                }
            }
            continue;
        }
        for (size_t k = 0u; k < len && (n + 1u) < cap; k++) {
            out[n++] = in[start + k];
        }
        if ((n + 1u) < cap) {
            out[n++] = '/';                 /* 分隔符; 结尾多余的会在下面去掉 */
        }
    }
    while (n > 1u && out[n - 1u] == '/') {
        n--;
    }
    out[n] = '\0';
}

/* ==================================================================== 标志翻译
 *
 * POSIX 的 `O_*`(Linux 数值) → native 的 `BR_O_*`。**两侧数值不同**, 所以这一层
 * 翻译是必需的(见 fcntl.h 的说明)。未知位置位**不报错**: POSIX 允许忽略
 * (`O_NOCTTY`/`O_LARGEFILE`/`O_CLOEXEC` 在本代没有语义)。
 */
static br_u32 svc_to_br_flags(int oflags)
{
    br_u32 bf;

    switch (oflags & O_ACCMODE) {
        case O_WRONLY: bf = BR_O_WRONLY; break;
        case O_RDWR:   bf = BR_O_RDWR;   break;
        default:       bf = BR_O_RDONLY; break;
    }
    if ((oflags & O_CREAT)    != 0) { bf |= BR_O_CREAT; }
    if ((oflags & O_EXCL)     != 0) { bf |= BR_O_EXCL; }
    if ((oflags & O_TRUNC)    != 0) { bf |= BR_O_TRUNC; }
    if ((oflags & O_APPEND)   != 0) { bf |= BR_O_APPEND; }
    if ((oflags & O_NONBLOCK) != 0) { bf |= BR_O_NONBLOCK; }
    if ((oflags & O_NOFOLLOW) != 0) { bf |= BR_O_NOFOLLOW; }
    return bf;
}

/* ==================================================================== fd 断言
 *
 * ★ 访问模式执法(**本件的职责**, vfs 刻意不做 —— ADR-0009 §5 遗留项 5)。
 */
static br_file_t *svc_fd_file(int fd)
{
    return br_ft_get(fd);
}

static int svc_fd_readable(int fd)
{
    br_u32 fl = 0u;
    if (br_ft_get_flags(fd, &fl) != 0) {
        return 0;
    }
    return ((fl & (br_u32)O_ACCMODE) != (br_u32)O_WRONLY) ? 1 : 0;
}

static int svc_fd_writable(int fd)
{
    br_u32 fl = 0u;
    if (br_ft_get_flags(fd, &fl) != 0) {
        return 0;
    }
    return ((fl & (br_u32)O_ACCMODE) != (br_u32)O_RDONLY) ? 1 : 0;
}

/* ==================================================================== 层: 文件 I/O */

int open(const char *path, int flags, ...)
{
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }

    int        err = 0;
    br_file_t *f = br_open_err(abs, svc_to_br_flags(flags), &err);
    if (f == BR_NULL) {
        return svc_fail(err);                       /* err 已是负 errno */
    }

    /* `O_DIRECTORY`: 打开了但不是目录 ⇒ 关掉再报 -ENOTDIR(照 Linux)。 */
    if (((flags & O_DIRECTORY) != 0) &&
        (br_file_inode(f) != BR_NULL) && (br_file_inode(f)->type != BR_INODE_DIR)) {
        (void)br_file_close(f);
        return svc_fail(BR_ERR(BR_ENOTDIR));
    }

    const int fd = br_ft_alloc(f, (br_u32)flags);
    if (fd < 0) {
        (void)br_file_close(f);                     /* 分配失败要**回滚**已打开的文件 */
        return svc_fail(fd);
    }
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    /* 只支持 `AT_FDCWD`: 相对某个**目录 fd** 的解析需要"由 fd 反查目录路径", 而 vfs
     * 的句柄不携带路径(它可能已被 rename/unlink)。如实返回 -ENOTSUP, 不假装支持。 */
    if (dirfd != AT_FDCWD) {
        return svc_fail(BR_ERR(BR_ENOTSUP));
    }
    return open(path, flags);
}

int creat(const char *path, mode_t mode)
{
    (void)mode;                                     /* 没有权限模型(见 chmod) */
    return open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

ssize_t read(int fd, void *buf, size_t n)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL || svc_fd_readable(fd) == 0) {
        return (ssize_t)svc_fail(BR_ERR(BR_EBADF));
    }
    const br_s64 r = br_file_read(f, buf, n);
    if (r < 0) {
        return (ssize_t)svc_fail((int)r);
    }
    return (ssize_t)r;
}

/*
 * `write` 的两个额外动作(都属"fd 层的职责"):
 *   ① 访问模式执法(只读 fd ⇒ EBADF);
 *   ② **`O_APPEND` 的兜底**: `F_SETFL` 之后才置上的 O_APPEND 到不了 `br_file_t` 的
 *      flags(那是打开时固定的), 所以本层在写前自己定位到末尾。普通文件才做 ——
 *      设备(如 uart)不可定位, 对它们 O_APPEND 本无语义。
 */
static ssize_t svc_write_common(int fd, const void *buf, size_t n, off_t off, int have_off)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL || svc_fd_writable(fd) == 0) {
        return (ssize_t)svc_fail(BR_ERR(BR_EBADF));
    }

    br_u32 fl = 0u;
    (void)br_ft_get_flags(fd, &fl);

    br_s64 saved = br_file_offset(f);
    int    moved = 0;

    if (have_off != 0) {
        if (br_file_lseek(f, (br_s64)off, BR_SEEK_SET) < 0) {
            return (ssize_t)svc_fail(BR_ERR(BR_ESPIPE));
        }
        moved = 1;
    } else if (((fl & (br_u32)O_APPEND) != 0u) &&
               (br_file_inode(f) != BR_NULL) && (br_file_inode(f)->type == BR_INODE_FILE)) {
        if (br_file_lseek(f, 0, BR_SEEK_END) < 0) {
            return (ssize_t)svc_fail(BR_ERR(BR_ESPIPE));
        }
    }

    const br_s64 r = br_file_write(f, buf, n);

    if (moved != 0) {
        br_file_set_offset(f, saved);               /* pread/pwrite 不动偏移 */
    }
    if (r < 0) {
        return (ssize_t)svc_fail((int)r);
    }
    return (ssize_t)r;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    return svc_write_common(fd, buf, n, 0, 0);
}

/*
 * `pread`/`pwrite`: vfs 没有这两个槽位(`11-02` 的 Q-6), 本代用"保存偏移 + lseek +
 * 读写 + 恢复偏移"模拟。★ **非原子**: 两个线程同时 pread 同一 fd 会互相踩偏移。
 * 单 APP 下可接受, 但它是一条**已知语义边界**, 写在 ADR-0014 §4。
 */
ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL || svc_fd_readable(fd) == 0) {
        return (ssize_t)svc_fail(BR_ERR(BR_EBADF));
    }
    const br_s64 saved = br_file_offset(f);
    if (br_file_lseek(f, (br_s64)off, BR_SEEK_SET) < 0) {
        return (ssize_t)svc_fail(BR_ERR(BR_ESPIPE));
    }
    const br_s64 r = br_file_read(f, buf, n);
    br_file_set_offset(f, saved);
    if (r < 0) {
        return (ssize_t)svc_fail((int)r);
    }
    return (ssize_t)r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    return svc_write_common(fd, buf, n, off, 1);
}

int close(int fd)
{
    br_file_t *last = BR_NULL;
    const int  rc   = br_ft_release(fd, &last);
    if (rc != 0) {
        return svc_fail(rc);                        /* -EBADF */
    }
    /* ★ 只有**最后一个引用**消失时才真关文件 —— 这是 `dup` 语义的落脚点
     *   (两个 fd 指向同一个 open file description; 关一个不影响另一个)。 */
    if (last != BR_NULL) {
        const int r2 = br_file_close(last);
        if (r2 != 0) {
            return svc_fail(r2);
        }
    }
    return 0;
}

off_t lseek(int fd, off_t off, int whence)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL) {
        return (off_t)svc_fail(BR_ERR(BR_EBADF));
    }
    /* BR_SEEK_SET/CUR/END 与 POSIX SEEK_SET/CUR/END 同值(0/1/2) —— 直接传。 */
    const br_s64 r = br_file_lseek(f, (br_s64)off, (int)whence);
    if (r < 0) {
        return (off_t)svc_fail((int)r);
    }
    return (off_t)r;
}

int dup(int oldfd)
{
    const int fd = br_ft_dup(oldfd);
    return (fd < 0) ? svc_fail(fd) : fd;
}

int dup2(int oldfd, int newfd)
{
    br_file_t *displaced = BR_NULL;
    const int  r = br_ft_dup2(oldfd, newfd, &displaced);
    if (r < 0) {
        return svc_fail(r);
    }
    /* 被顶掉的句柄若已无其它引用, 必须在这里关掉 —— 表按契约把决定权交给调用方。 */
    if (displaced != BR_NULL) {
        (void)br_file_close(displaced);
    }
    return newfd;
}

int fsync(int fd)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    const int r = br_file_fsync(f);
    /* vfs 的 fsync 槽位对 tmpfs 是 BR_NULL ⇒ -ENOTSUP。POSIX 允许 fsync 对不支持它的
     * 对象返回 EINVAL/ENOTSUP —— 如实上传, **不**假装成功(假装成功会让"数据落盘了"
     * 这个判断失去依据)。 */
    return (r != 0) ? svc_fail(r) : 0;
}

int fdatasync(int fd)
{
    return fsync(fd);
}

int ftruncate(int fd, off_t len)
{
    if (len < 0) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    if (svc_fd_writable(fd) == 0) {
        return svc_fail(BR_ERR(BR_EINVAL));         /* Linux: 只读 fd 上 ftruncate ⇒ EINVAL */
    }
    br_inode_t *ino = br_file_inode(f);
    if (ino == BR_NULL || ino->iops == BR_NULL || ino->iops->setattr == BR_NULL) {
        return svc_fail(BR_ERR(BR_ENOTSUP));
    }
    const br_stat_t st = { .valid = BR_STAT_SIZE, .type = ino->type,
                           .size = (br_u64)len, .nlink = 0u };
    const int rc = ino->iops->setattr(ino, &st);
    return (rc != 0) ? svc_fail(rc) : 0;
}

int truncate(const char *path, off_t len)
{
    if (len < 0) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_truncate(abs, (br_u64)len);
    return (rc != 0) ? svc_fail(rc) : 0;
}

/* 本代没有 tty 层(`br-wa-io-001`)⇒ 如实回"不是终端", 并按 POSIX 置 ENOTTY。 */
int isatty(int fd)
{
    if (svc_fd_file(fd) == BR_NULL) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return 0;                                   /* errno = EBADF, 保持不动 */
    }
    errno = ENOTTY;                                 /* 有效 fd 但不是终端(无 tty 层) */
    return 0;
}

/* ---- 分散/聚集(TR-B: 循环调 read/write, 部分完成语义照 POSIX) ---- */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    if (iov == BR_NULL || iovcnt <= 0) {
        return (ssize_t)svc_fail(BR_ERR(BR_EINVAL));
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0u) {
            continue;
        }
        const ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) {
            return (total > 0) ? total : r;         /* 已读到一些 ⇒ 短读返回 */
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) {
            break;                                  /* 短读: 不再继续(语义与 read 一致) */
        }
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (iov == BR_NULL || iovcnt <= 0) {
        return (ssize_t)svc_fail(BR_ERR(BR_EINVAL));
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0u) {
            continue;
        }
        const ssize_t r = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) {
            return (total > 0) ? total : r;
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

/* ==================================================================== 层: 记录锁
 *
 * `fcntl(F_SETLK/F_GETLK)`。设计已裁定: **单 APP 下退化为进程内互斥**(`1-01` §7.6)。
 * 语义边界(写在 ADR-0014 §4):
 *   - 锁按 `br_file_t*`(即 open file description)区分 —— `dup` 出来的 fd 共享同一把锁;
 *   - `F_SETLKW`(阻塞获取)**不实现** ⇒ -ENOTSUP。阻塞需要等待队列 + 唤醒, 而"给谁唤醒"
 *     在进程内锁里没有答案(没有锁管理器线程); 假装它能阻塞会让调用方死等。
 */
typedef struct {
    br_u32     used;
    br_file_t *f;
    int        type;      /* F_RDLCK / F_WRLCK */
    off_t      start;
    off_t      len;       /* 0 = 到文件末尾 */
    pid_t      pid;
} svc_lock_t;

static svc_lock_t s_locks[SVC_LOCK_MAX];

static void svc_lock_range(const struct flock *fl, off_t *s, off_t *e)
{
    *s = fl->l_start;
    *e = (fl->l_len == 0) ? (off_t)0x7fffffffffffffffL : (fl->l_start + fl->l_len);
}

static int svc_overlap(off_t s1, off_t e1, off_t s2, off_t e2)
{
    return ((s1 < e2) && (s2 < e1)) ? 1 : 0;
}

static int svc_lock_set(int fd, struct flock *fl)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL || fl == BR_NULL) {
        return svc_fail(BR_ERR(BR_EBADF));
    }

    off_t s = 0;
    off_t e = 0;
    svc_lock_range(fl, &s, &e);

    if (fl->l_type == F_UNLCK) {
        for (br_u32 i = 0u; i < SVC_LOCK_MAX; i++) {
            if (s_locks[i].used == 1u && s_locks[i].f == f &&
                svc_overlap(s, e, s_locks[i].start,
                            (s_locks[i].len == 0) ? (off_t)0x7fffffffffffffffL
                                                  : (s_locks[i].start + s_locks[i].len)) != 0) {
                s_locks[i].used = 0u;
            }
        }
        return 0;
    }

    /* 冲突检查: **别的** open file description 持有重叠的写锁(或本请求是写锁) */
    for (br_u32 i = 0u; i < SVC_LOCK_MAX; i++) {
        if (s_locks[i].used == 0u || s_locks[i].f == f) {
            continue;
        }
        const off_t os = s_locks[i].start;
        const off_t oe = (s_locks[i].len == 0) ? (off_t)0x7fffffffffffffffL
                                               : (s_locks[i].start + s_locks[i].len);
        if (svc_overlap(s, e, os, oe) != 0 &&
            (s_locks[i].type == F_WRLCK || fl->l_type == F_WRLCK)) {
            return svc_fail(BR_ERR(BR_EAGAIN));     /* POSIX 允许 EACCES/EAGAIN */
        }
    }

    /* 记一条(先找同 f 同区间的旧条目替换, 否则找空位) */
    br_u32 slot = (br_u32)SVC_LOCK_MAX;
    for (br_u32 i = 0u; i < SVC_LOCK_MAX; i++) {
        if (s_locks[i].used == 1u && s_locks[i].f == f &&
            s_locks[i].start == s && s_locks[i].len == fl->l_len) {
            slot = i;
            break;
        }
    }
    for (br_u32 i = 0u; (i < SVC_LOCK_MAX) && (slot == (br_u32)SVC_LOCK_MAX); i++) {
        if (s_locks[i].used == 0u) {
            slot = i;
        }
    }
    if (slot == (br_u32)SVC_LOCK_MAX) {
        return svc_fail(BR_ERR(BR_ENOLCK));         /* 锁表满 */
    }
    s_locks[slot].used  = 1u;
    s_locks[slot].f     = f;
    s_locks[slot].type  = fl->l_type;
    s_locks[slot].start = s;
    s_locks[slot].len   = fl->l_len;
    s_locks[slot].pid   = 1;
    return 0;
}

static int svc_lock_get(int fd, struct flock *fl)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL || fl == BR_NULL) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    off_t s = 0;
    off_t e = 0;
    svc_lock_range(fl, &s, &e);

    for (br_u32 i = 0u; i < SVC_LOCK_MAX; i++) {
        if (s_locks[i].used == 0u || s_locks[i].f == f) {
            continue;
        }
        const off_t oe = (s_locks[i].len == 0) ? (off_t)0x7fffffffffffffffL
                                               : (s_locks[i].start + s_locks[i].len);
        if (svc_overlap(s, e, s_locks[i].start, oe) != 0) {
            fl->l_type   = s_locks[i].type;
            fl->l_whence = SEEK_SET;
            fl->l_start  = s_locks[i].start;
            fl->l_len    = s_locks[i].len;
            fl->l_pid    = s_locks[i].pid;
            return 0;
        }
    }
    fl->l_type = F_UNLCK;                           /* "没有冲突" */
    return 0;
}

int fcntl(int fd, int cmd, ...)
{
    void *arg = BR_NULL;

    /* ★ 只为**需要参数**的 cmd 取 vararg: 对 F_GETFD 之类的无参命令读 vararg 是 UB
     *   (调用方没压栈, 读到的可能是寄存器残留)。 */
    switch (cmd) {
        case F_SETFL:
        case F_DUPFD:
        case F_SETLK:
        case F_GETLK:
        case F_SETLKW: {
            va_list ap;
            va_start(ap, cmd);
            arg = va_arg(ap, void *);
            va_end(ap);
            break;
        }
        default:
            break;
    }

    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL) {
        return svc_fail(BR_ERR(BR_EBADF));
    }

    switch (cmd) {
        case F_GETFD:
            return 0;                               /* 没有 FD_CLOEXEC 的载体(无 exec) */
        case F_SETFD:
            return 0;
        case F_GETFL: {
            br_u32 fl = 0u;
            if (br_ft_get_flags(fd, &fl) != 0) {
                return svc_fail(BR_ERR(BR_EBADF));
            }
            return (int)fl;
        }
        case F_SETFL: {
            /* 只接受"状态位"的改动; 访问模式不可改(POSIX 亦然)。 */
            br_u32 fl = 0u;
            if (br_ft_get_flags(fd, &fl) != 0) {
                return svc_fail(BR_ERR(BR_EBADF));
            }
            const br_u32 want   = (br_u32)(br_uintptr_t)arg;
            const br_u32 change = (br_u32)(O_APPEND | O_NONBLOCK);
            fl = (fl & ~change) | (want & change);
            const int rc = br_ft_set_flags(fd, fl);
            return (rc != 0) ? svc_fail(rc) : 0;
        }
        case F_DUPFD: {
            /* 复制到 **≥ arg** 的最小可用 fd(POSIX F_DUPFD 的语义) */
            const int lo = (int)(br_intptr_t)arg;
            if (lo < 0) {
                return svc_fail(BR_ERR(BR_EINVAL));
            }
            for (int cand = lo; cand < (int)BR_FT_MAX; cand++) {
                if (br_ft_get(cand) == BR_NULL) {
                    br_file_t *disp = BR_NULL;
                    const int  r = br_ft_dup2(fd, cand, &disp);
                    if (r < 0) {
                        return svc_fail(r);
                    }
                    if (disp != BR_NULL) {
                        (void)br_file_close(disp);  /* 该位本来是空的 ⇒ 理论到不了 */
                    }
                    return cand;
                }
            }
            return svc_fail(BR_ERR(BR_EMFILE));
        }
        case F_SETLK:
            return svc_lock_set(fd, (struct flock *)arg);
        case F_GETLK:
            return svc_lock_get(fd, (struct flock *)arg);
        case F_SETLKW:
            /* 见文件头的说明: 阻塞式记录锁要等待队列, 本代不做。 */
            return svc_fail(BR_ERR(BR_ENOTSUP));
        default:
            return svc_fail(BR_ERR(BR_EINVAL));
    }
}

/* ==================================================================== 层: 元数据 */

static void svc_fill_stat(const br_stat_t *b, struct stat *st)
{
    for (size_t i = 0u; i < sizeof(*st); i++) {
        ((unsigned char *)st)[i] = 0u;
    }
    st->st_nlink = (nlink_t)(((b->valid & BR_STAT_NLINK) != 0u) ? b->nlink : 1u);
    st->st_size  = (off_t)b->size;
    switch (b->type) {
        case BR_INODE_DIR:     st->st_mode = (mode_t)(S_IFDIR | BR_POSIX_MODE_BITS); break;
        case BR_INODE_SYMLINK: st->st_mode = (mode_t)(S_IFLNK | BR_POSIX_MODE_BITS); break;
        default:               st->st_mode = (mode_t)(S_IFREG | BR_POSIX_MODE_BITS); break;
    }
    st->st_blksize = 512;
    st->st_blocks  = (blkcnt_t)((b->size + 511u) / 512u);
    /* st_dev/st_ino/st_uid/st_gid/st_rdev/st_*time 恒 0 —— vfs 没有这些面(ADR-0014 §4)。 */
}

static int svc_stat_common(const char *path, struct stat *st, int nofollow)
{
    if (st == BR_NULL) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    br_stat_t b;
    rc = nofollow ? br_lstat(abs, &b) : br_stat(abs, &b);
    if (rc != 0) {
        return svc_fail(rc);
    }
    svc_fill_stat(&b, st);
    return 0;
}

int stat(const char *path, struct stat *st)
{
    return svc_stat_common(path, st, 0);
}

int lstat(const char *path, struct stat *st)
{
    return svc_stat_common(path, st, 1);
}

int fstat(int fd, struct stat *st)
{
    if (st == BR_NULL) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    br_stat_t b;
    const int rc = br_fstat(f, &b);                 /* vfs 的句柄级原语(ADR-0013) */
    if (rc != 0) {
        return svc_fail(rc);
    }
    svc_fill_stat(&b, st);
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode;                                     /* 无权限模型(见 ADR-0014) */
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_mkdir(abs);
    return (rc != 0) ? svc_fail(rc) : 0;
}

/* ---- 权限面: 按用户裁定 = **空操作返回 0**(见 ADR-0014 §2)----
 * 为什么"空操作"是诚实而不是偷懒: 本原型**没有**权限模型(vfs 没有 mode/uid/gid 存储面,
 * 也没有"当前用户"), 所以"接受并忽略"与"返回 -ENOTSUP"相比, 前者让按 POSIX 写的代码
 * 能跑完(chmod 0644 后功能不受影响), 后者会让它在无关的地方失败。真正的风险是
 * "调用方以为权限生效了" —— 这一点靠文档与 ADR 显式声明来兜, 不靠返回码兜。 */
int chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return 0;
}

int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;
    return 0;
}

int chown(const char *path, uid_t uid, gid_t gid)
{
    (void)path;
    (void)uid;
    (void)gid;
    return 0;
}

int fchown(int fd, uid_t uid, gid_t gid)
{
    (void)fd;
    (void)uid;
    (void)gid;
    return 0;
}

mode_t umask(mode_t mask)
{
    const mode_t old = s_umask;
    s_umask = (mode_t)(mask & 0777u);
    return old;                                     /* 记录 + 返回旧值; 不执法 */
}

/*
 * `access`: 权限位**一律放行**(用户裁定), 但**存在性检查照做** ——
 * 一个"文件不存在却回答可读"的 access 会让调用方的分支逻辑彻底失真, 那不是"空操作",
 * 那是撒谎。所以语义 = "F_OK 照查; R/W/X 恒真"。
 */
int access(const char *path, int mode)
{
    char     abs[SVC_PATH_MAX];
    int      rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    br_stat_t b;
    rc = br_stat(abs, &b);
    if (rc != 0) {
        return svc_fail(rc);
    }
    (void)mode;
    return 0;
}

/* ==================================================================== 层: 目录 */

int unlink(const char *path)
{
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_unlink(abs);
    return (rc != 0) ? svc_fail(rc) : 0;
}

int rmdir(const char *path)
{
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_rmdir(abs);
    return (rc != 0) ? svc_fail(rc) : 0;
}

/* POSIX 的 remove(): 文件走 unlink, 空目录走 rmdir。 */
int remove(const char *path)
{
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_unlink(abs);
    if (rc == BR_ERR(BR_EISDIR)) {
        rc = br_rmdir(abs);
    }
    return (rc != 0) ? svc_fail(rc) : 0;
}

int rename(const char *old_path, const char *new_path)
{
    char a_old[SVC_PATH_MAX];
    char a_new[SVC_PATH_MAX];
    int  rc = svc_abs(old_path, a_old, (size_t)sizeof(a_old));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = svc_abs(new_path, a_new, (size_t)sizeof(a_new));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_rename(a_old, a_new);
    return (rc != 0) ? svc_fail(rc) : 0;
}

int link(const char *old_path, const char *new_path)
{
    char a_old[SVC_PATH_MAX];
    char a_new[SVC_PATH_MAX];
    int  rc = svc_abs(old_path, a_old, (size_t)sizeof(a_old));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = svc_abs(new_path, a_new, (size_t)sizeof(a_new));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_link(a_old, a_new);
    return (rc != 0) ? svc_fail(rc) : 0;
}

int symlink(const char *target, const char *linkpath)
{
    if (target == BR_NULL) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(linkpath, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    /* 目标串**不**做绝对化: 符号链接的语义就是"按链接所在目录解释"(`7-01` 与 vfs 的
     * resolve_norm 都靠这条), 提前拼成绝对路径会把链接钉死在当前 cwd 上。 */
    rc = br_symlink(target, abs);
    return (rc != 0) ? svc_fail(rc) : 0;
}

ssize_t readlink(const char *path, char *buf, size_t cap)
{
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return (ssize_t)svc_fail(rc);
    }
    rc = br_readlink(abs, buf, cap);
    if (rc < 0) {
        return (ssize_t)svc_fail(rc);
    }
    return (ssize_t)rc;                             /* 全长(可能 > cap, 照 POSIX) */
}

/* ---- DIR ---- */

struct br_posix_dir {
    br_dir_t      *d;
    struct dirent  ent;
    char           path[SVC_PATH_MAX];   /* rewinddir 要"关掉再开" ⇒ 必须记得开的是谁 */
};

static br_u32 svc_dtype(br_u32 br_type)
{
    switch (br_type) {
        case BR_INODE_DIR:     return DT_DIR;
        case BR_INODE_SYMLINK: return DT_LNK;
        case BR_INODE_FILE:    return DT_REG;
        default:               return DT_UNKNOWN;
    }
}

DIR *opendir(const char *path)
{
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        (void)svc_fail(rc);
        return BR_NULL;
    }
    br_dir_t *d = br_opendir(abs);
    if (d == BR_NULL) {
        errno = ENOTDIR;                            /* vfs 的 opendir 不设 errno; 见下 */
        return BR_NULL;
    }
    DIR *out = (DIR *)br_malloc((br_size_t)sizeof(DIR));
    if (out == BR_NULL) {
        (void)br_closedir(d);
        errno = ENOMEM;
        return BR_NULL;
    }
    out->d = d;
    svc_copy(out->path, abs, (size_t)sizeof(out->path));
    return out;
}

struct dirent *readdir(DIR *d)
{
    if (d == BR_NULL) {
        errno = EBADF;
        return BR_NULL;
    }
    br_dirent_t de;
    const int   rc = br_readdir(d->d, &de);
    if (rc < 0) {
        errno = -rc;                                /* 真错误: 让调用方看见 */
        return BR_NULL;
    }
    if (rc == 0) {
        return BR_NULL;                             /* 目录穷尽(POSIX: 返回 NULL, 不置 errno) */
    }
    d->ent.d_ino    = 0;                            /* vfs 没有 inode 号 */
    d->ent.d_off    = 0;
    d->ent.d_reclen = (unsigned short)sizeof(struct dirent);
    d->ent.d_type   = (unsigned char)svc_dtype(de.type);
    svc_copy(d->ent.d_name, de.name, (size_t)BR_POSIX_NAME_MAX);
    return &d->ent;
}

int closedir(DIR *d)
{
    if (d == BR_NULL) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    const int rc = br_closedir(d->d);
    br_free(d);
    return (rc != 0) ? svc_fail(rc) : 0;
}

void rewinddir(DIR *d)
{
    if (d == BR_NULL || d->d == BR_NULL) {
        return;
    }
    /* vfs 的目录迭代器没有"重置"槽位 ⇒ 关掉再开一次(语义等价, 代价一次 open)。
     * 这正是"seekdir/telldir 属 TR-C"的另一面(见 dirent.h)。记住路径就是为了这里。 */
    (void)br_closedir(d->d);
    d->d = br_opendir(d->path);
}

/* ---- cwd ---- */

int chdir(const char *path)
{
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    char clean[SVC_PATH_MAX];
    svc_clean(abs, clean, (size_t)sizeof(clean));

    br_stat_t b;
    rc = br_stat(clean, &b);
    if (rc != 0) {
        return svc_fail(rc);
    }
    if (b.type != BR_INODE_DIR) {
        return svc_fail(BR_ERR(BR_ENOTDIR));
    }
    svc_copy(s_cwd, clean, (size_t)sizeof(s_cwd));
    return 0;
}

char *getcwd(char *buf, size_t size)
{
    const size_t n = svc_strlen(s_cwd);
    if (buf == BR_NULL || size < (n + 1u)) {
        errno = ERANGE;
        return BR_NULL;
    }
    svc_copy(buf, s_cwd, size);
    return buf;
}

/* ==================================================================== 层: time */

static void svc_us_to_ts(br_u64 us, struct timespec *ts)
{
    ts->tv_sec  = (time_t)(us / 1000000u);
    ts->tv_nsec = (long)((us % 1000000u) * 1000u);
}

int clock_gettime(clockid_t id, struct timespec *ts)
{
    if (ts == BR_NULL) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    /* ★ 只有 MONOTONIC: 没有墙钟源(P-1), 所以 REALTIME **如实**返回 -ENOTSUP,
     *   而不是编一个假装是"1970 年至今"的数字(那会让排序/超时逻辑静默错)。 */
    if (id != CLOCK_MONOTONIC) {
        return svc_fail(BR_ERR(BR_ENOTSUP));
    }
    svc_us_to_ts(br_clock_now(), ts);
    return 0;
}

int clock_getres(clockid_t id, struct timespec *ts)
{
    if (ts == BR_NULL) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    if (id != CLOCK_MONOTONIC) {
        return svc_fail(BR_ERR(BR_ENOTSUP));
    }
    ts->tv_sec  = 0;
    ts->tv_nsec = BR_POSIX_CLOCK_RES_NS;            /* 如实报一拍 = 1/HZ(周期 tick) */
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req == BR_NULL || req->tv_sec < 0 || req->tv_nsec < 0) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    if (rem != BR_NULL) {
        rem->tv_sec  = 0;                           /* 本代不会被信号打断 ⇒ 没有剩余 */
        rem->tv_nsec = 0;
    }
    const br_u64 us = ((br_u64)req->tv_sec * 1000000u) + ((br_u64)req->tv_nsec / 1000u);
    const int rc = br_task_sleep((br_time_t)us);    /* 保证**不早醒**(INV-2) */
    return (rc != 0) ? svc_fail(rc) : 0;
}

unsigned int sleep(unsigned int seconds)
{
    (void)br_task_sleep((br_time_t)((br_u64)seconds * 1000000u));
    return 0u;                                      /* 不早醒 ⇒ 睡满, 无剩余 */
}

int usleep(useconds_t usec)
{
    const int rc = br_task_sleep((br_time_t)usec);
    return (rc != 0) ? svc_fail(rc) : 0;
}

/* ==================================================================== 层: poll/select
 *
 * v1 语义 = 设计 SD-7 的"轮询 + 粗粒度 sleep"(见 poll.h 的说明)。
 */

/* 一轮查询: 写 revents, 返回就绪个数。 */
static int svc_poll_once(struct pollfd *fds, nfds_t n, int *bad)
{
    int ready = 0;
    *bad = 0;
    for (nfds_t i = 0u; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) {
            continue;                               /* POSIX: 负 fd **被忽略**(revents = 0) */
        }
        br_file_t *f = svc_fd_file(fds[i].fd);
        if (f == BR_NULL) {
            fds[i].revents = POLLNVAL;              /* POSIX: 无效 fd 会让 poll 立刻返回 */
            (*bad)++;
            ready++;
            continue;
        }
        br_u32 ev = 0u;
        const int rc = br_file_poll(f, &ev);
        short     re = 0;
        if (rc == 0) {
            if ((ev & BR_POLLIN) != 0u)  { re = (short)(re | POLLIN); }
            if ((ev & BR_POLLOUT) != 0u) { re = (short)(re | POLLOUT); }
            if ((ev & BR_POLLERR) != 0u) { re = (short)(re | POLLERR); }
        } else if (rc == BR_ERR(BR_ENOTSUP)) {
            /* 没有 poll 槽位(如 tmpfs 的内存文件): POSIX 的答复是"恒就绪" ——
             * 内存文件永远可读写, 这比返回错误更接近事实。 */
            re = (short)(fds[i].events & (POLLIN | POLLOUT));
        } else {
            re = POLLERR;
        }
        fds[i].revents = (short)(re & (short)(fds[i].events | POLLERR | POLLHUP | POLLNVAL));
        if (fds[i].revents != 0) {
            ready++;
        }
    }
    return ready;
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    if (fds == BR_NULL && nfds != 0u) {
        return svc_fail(BR_ERR(BR_EINVAL));
    }
    if (nfds == 0u) {
        /* 纯延时: 照 POSIX 睡满 timeout 再返回 0 */
        if (timeout_ms > 0) {
            (void)br_task_sleep((br_time_t)((br_u64)timeout_ms * 1000u));
        }
        return 0;
    }

    const br_time_t deadline = (timeout_ms < 0)
                             ? BR_TIMEOUT_INF
                             : br_deadline_from_now((br_time_t)((br_u64)timeout_ms * 1000u));

    for (;;) {
        int bad = 0;
        const int ready = svc_poll_once(fds, nfds, &bad);
        if (ready > 0) {
            /* 无效 fd 优先(POSIX 要求它立刻返回, 不等待) */
            return ready;
        }
        if (timeout_ms == 0) {
            return 0;
        }
        if (deadline != BR_TIMEOUT_INF && br_clock_now() >= deadline) {
            return 0;
        }
        /* 一小片再查。★ tickless 未落地 ⇒ 实际睡眠 ≈ 一个 tick(`1/HZ`, 缺省 5 ms),
         *   所以"轮询间隔"取 10 ms 是**声明意图**, 不是承诺(P-2)。 */
        (void)br_task_sleep((br_time_t)10000u);
        if (deadline != BR_TIMEOUT_INF && br_clock_now() >= deadline) {
            return 0;
        }
    }
}

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout)
{
    if (nfds < 0 || nfds > (int)SVC_SELECT_MAX) {
        return svc_fail(BR_ERR(BR_EINVAL));         /* 超界**报错**而不是静默截断 */
    }
    struct pollfd fds[SVC_SELECT_MAX];
    int           map[SVC_SELECT_MAX];
    nfds_t        n = 0u;

    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (rd != BR_NULL && FD_ISSET(fd, rd) != 0ul) { ev = (short)(ev | POLLIN); }
        if (wr != BR_NULL && FD_ISSET(fd, wr) != 0ul) { ev = (short)(ev | POLLOUT); }
        if (ex != BR_NULL && FD_ISSET(fd, ex) != 0ul) { ev = (short)(ev | POLLPRI); }
        if (ev != 0) {
            fds[n].fd      = fd;
            fds[n].events  = ev;
            fds[n].revents = 0;
            map[n]         = fd;
            n++;
        }
    }

    const int tmo = (timeout == BR_NULL)
                  ? -1
                  : (int)((timeout->tv_sec * 1000) + (timeout->tv_usec / 1000));
    const int ready = poll(fds, n, tmo);
    if (ready < 0) {
        return -1;
    }

    /* poll → select 的回写(三个集合都要按结果重写, POSIX 语义) */
    fd_set r_out;
    fd_set w_out;
    fd_set e_out;
    FD_ZERO(&r_out);
    FD_ZERO(&w_out);
    FD_ZERO(&e_out);
    int count = 0;
    for (nfds_t i = 0u; i < n; i++) {
        if (fds[i].revents == 0) {
            continue;
        }
        const int fd = map[i];
        if ((fds[i].revents & POLLIN) != 0)  { FD_SET(fd, &r_out); count++; }
        if ((fds[i].revents & POLLOUT) != 0) { FD_SET(fd, &w_out); count++; }
        if ((fds[i].revents & POLLERR) != 0) { FD_SET(fd, &e_out); count++; }
    }
    if (rd != BR_NULL) { *rd = r_out; }
    if (wr != BR_NULL) { *wr = w_out; }
    if (ex != BR_NULL) { *ex = e_out; }
    return (count > 0) ? count : ready;
}

/* ==================================================================== 层: stdio
 *
 * 只有**流层**(FILE* + 写缓冲 + 定位), 没有格式化层(见 stdio.h 的说明)。
 */

struct br_posix_file {
    br_u32         magic;
    int            fd;
    int            owned;       /* fclose 是否 close(fd) */
    int            eof;
    int            err;
    int            readable;
    int            writable;
    int            append;      /* O_APPEND ⇒ 不做写缓冲(交错写会错序) */
    long           bufmode;     /* _IOFBF / _IOLBF / _IONBF */
    unsigned char *wbuf;
    size_t         wcap;
    size_t         wlen;
};

#define SVC_FILE_MAGIC  0x5046494Cu   /* "PFIL" */

static FILE *s_files[SVC_FILE_MAX];

/* 找空槽; 顺手回收"已被 fclose 但没清零"的僵尸槽(它们 magic 已失效) */
static FILE *svc_file_slot(void)
{
    for (br_u32 i = 0u; i < (br_u32)SVC_FILE_MAX; i++) {
        if (s_files[i] == BR_NULL || s_files[i]->magic != SVC_FILE_MAGIC) {
            if (s_files[i] != BR_NULL) {
                br_free(s_files[i]);
            }
            FILE *f = (FILE *)br_malloc((br_size_t)sizeof(FILE));
            if (f == BR_NULL) {
                s_files[i] = BR_NULL;
                return BR_NULL;
            }
            for (size_t k = 0u; k < sizeof(*f); k++) {
                ((unsigned char *)f)[k] = 0u;
            }
            f->magic = SVC_FILE_MAGIC;
            f->fd    = -1;
            s_files[i] = f;
            return f;
        }
    }
    return BR_NULL;
}

static void svc_file_drop(FILE *f)
{
    for (br_u32 i = 0u; i < (br_u32)SVC_FILE_MAX; i++) {
        if (s_files[i] == f) {
            s_files[i] = BR_NULL;
            break;
        }
    }
    if (f->wbuf != BR_NULL) {
        br_free(f->wbuf);
        f->wbuf = BR_NULL;
    }
    f->magic = 0u;
    br_free(f);
}

static int svc_mode_parse(const char *mode, int *flags, int *rd, int *wr, int *ap)
{
    if (mode == BR_NULL || mode[0] == '\0') {
        return BR_ERR(BR_EINVAL);
    }
    *rd = 0;
    *wr = 0;
    *ap = 0;

    switch (mode[0]) {
        case 'r': *flags = O_RDONLY; *rd = 1; break;
        case 'w': *flags = O_WRONLY | O_CREAT | O_TRUNC; *wr = 1; break;
        case 'a': *flags = O_WRONLY | O_CREAT | O_APPEND; *wr = 1; *ap = 1; break;
        default:  return BR_ERR(BR_EINVAL);
    }
    for (int i = 1; mode[i] != '\0'; i++) {
        if (mode[i] == '+') {
            *flags = (*flags & ~O_ACCMODE) | O_RDWR;
            *rd = 1;
            *wr = 1;
            if (mode[0] == 'a') {
                *flags |= O_APPEND;
                *ap = 1;
            }
        } else if (mode[i] == 'b' || mode[i] == 'e' || mode[i] == 'x') {
            /* 'b' = 二进制(无区分); 'e'/'x' 不在本代 ⇒ 忽略 */
        } else {
            return BR_ERR(BR_EINVAL);
        }
    }
    return BR_OK;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags = 0;
    int rd = 0;
    int wr = 0;
    int ap = 0;
    int rc = svc_mode_parse(mode, &flags, &rd, &wr, &ap);
    if (rc != BR_OK) {
        (void)svc_fail(rc);
        return BR_NULL;
    }
    if (rd != 0 && wr != 0) {
        flags = (flags & ~O_ACCMODE) | O_RDWR;
    }

    const int fd = open(path, flags);
    if (fd < 0) {
        return BR_NULL;                             /* errno 已由 open 设好 */
    }
    FILE *f = fdopen(fd, mode);
    if (f == BR_NULL) {
        (void)close(fd);
        return BR_NULL;
    }
    f->owned = 1;
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    int flags = 0;
    int rd = 0;
    int wr = 0;
    int ap = 0;
    int rc = svc_mode_parse(mode, &flags, &rd, &wr, &ap);
    if (rc != BR_OK) {
        (void)svc_fail(rc);
        return BR_NULL;
    }
    if (svc_fd_file(fd) == BR_NULL) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return BR_NULL;
    }
    FILE *f = svc_file_slot();
    if (f == BR_NULL) {
        errno = ENOMEM;
        return BR_NULL;
    }
    f->fd       = fd;
    f->owned    = 0;
    f->readable = rd;
    f->writable = wr;
    f->append   = ap;
    f->bufmode  = _IOFBF;
    if (wr != 0 && ap == 0) {
        f->wbuf = (unsigned char *)br_malloc((br_size_t)BR_POSIX_BUFSIZ);
        if (f->wbuf != BR_NULL) {
            f->wcap = BR_POSIX_BUFSIZ;
        }
    }
    return f;
}

int fflush(FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    if (f->wlen == 0u) {
        return 0;
    }
    size_t done = 0u;
    while (done < f->wlen) {
        const ssize_t r = write(f->fd, f->wbuf + done, f->wlen - done);
        if (r <= 0) {
            f->err = 1;
            return svc_fail(BR_ERR(BR_EIO));
        }
        done += (size_t)r;
    }
    f->wlen = 0u;
    return 0;
}

static size_t svc_file_push(FILE *f, const unsigned char *src, size_t n)
{
    size_t done = 0u;
    if (f->bufmode == _IONBF || f->append != 0 || f->wbuf == BR_NULL) {
        while (done < n) {
            const ssize_t r = write(f->fd, src + done, n - done);
            if (r <= 0) {
                f->err = 1;
                break;
            }
            done += (size_t)r;
        }
        return done;
    }
    while (done < n) {
        const size_t room = f->wcap - f->wlen;
        size_t       take = n - done;
        if (take > room) {
            take = room;
        }
        for (size_t i = 0u; i < take; i++) {
            f->wbuf[f->wlen + i] = src[done + i];
        }
        f->wlen += take;
        done += take;
        if (f->wlen == f->wcap) {
            if (fflush(f) != 0) {
                break;
            }
        }
    }
    return done;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC || f->writable == 0) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return 0u;
    }
    if (size == 0u || n == 0u) {
        return 0u;                                  /* 零长度写: **不碰** errno(照 POSIX) */
    }
    const size_t total = size * n;
    const size_t done  = svc_file_push(f, (const unsigned char *)buf, total);
    return done / size;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC || f->readable == 0) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return 0u;
    }
    if (size == 0u || n == 0u) {
        return 0u;
    }
    const size_t total = size * n;
    const ssize_t r = read(f->fd, buf, total);
    if (r < 0) {
        f->err = 1;
        return 0u;
    }
    if (r == 0) {
        f->eof = 1;
    }
    return (size_t)r / size;
}

int fseek(FILE *f, long off, int whence)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    if (fflush(f) != 0) {
        return -1;
    }
    const off_t r = lseek(f->fd, (off_t)off, whence);
    if (r < 0) {
        return -1;
    }
    f->eof = 0;                                     /* POSIX: 定位清除 EOF 标志 */
    return 0;
}

long ftell(FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return -1;
    }
    const off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) {
        return -1;
    }
    /* ★ 逻辑位置 = 文件位置 + 还没落盘的字节数(写缓冲让它落后于内核位置) */
    return (long)(pos + (off_t)f->wlen);
}

void rewind(FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        return;
    }
    (void)fseek(f, 0L, SEEK_SET);
    f->eof = 0;
    f->err = 0;
}

int fclose(FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    int rc = fflush(f);
    if (f->owned != 0 && f->fd >= 0) {
        const int r2 = close(f->fd);
        if (r2 != 0 && rc == 0) {
            rc = r2;
        }
    }
    svc_file_drop(f);
    return rc;
}

int feof(FILE *f)
{
    return (f != BR_NULL && f->magic == SVC_FILE_MAGIC && f->eof != 0) ? 1 : 0;
}

int ferror(FILE *f)
{
    return (f != BR_NULL && f->magic == SVC_FILE_MAGIC && f->err != 0) ? 1 : 0;
}

void clearerr(FILE *f)
{
    if (f != BR_NULL && f->magic == SVC_FILE_MAGIC) {
        f->eof = 0;
        f->err = 0;
    }
}

int fileno(FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return -1;
    }
    return f->fd;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        return svc_fail(BR_ERR(BR_EBADF));
    }
    (void)size;
    /* 调用方给的 `buf` **不接管**: 它的生命周期由调用方管, 而本实现要能自己释放
     * 自己的缓冲。POSIX 允许实现忽略这个参数(它只是"建议")。 */
    (void)buf;
    if (mode == _IONBF) {
        if (fflush(f) != 0) {
            return -1;
        }
        if (f->wbuf != BR_NULL) {
            br_free(f->wbuf);
            f->wbuf = BR_NULL;
            f->wcap = 0u;
        }
    }
    f->bufmode = mode;
    return 0;
}

int fputc(int c, FILE *f)
{
    const unsigned char b = (unsigned char)c;
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC || f->writable == 0) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return EOF;
    }
    return (svc_file_push(f, &b, 1u) == 1u) ? (int)b : EOF;
}

int fputs(const char *s, FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC || f->writable == 0) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return EOF;
    }
    const size_t n = svc_strlen(s);
    return (svc_file_push(f, (const unsigned char *)s, n) == n) ? 0 : EOF;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF) {
        return EOF;
    }
    return fputc('\n', stdout);
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int fgetc(FILE *f)
{
    unsigned char b = 0u;
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC || f->readable == 0) {
        (void)svc_fail(BR_ERR(BR_EBADF));
        return EOF;
    }
    const ssize_t r = read(f->fd, &b, 1u);
    if (r <= 0) {
        if (r == 0) {
            f->eof = 1;
        } else {
            f->err = 1;
        }
        return EOF;
    }
    return (int)b;
}

char *fgets(char *buf, int n, FILE *f)
{
    if (buf == BR_NULL || n <= 0 || f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        return BR_NULL;
    }
    int i = 0;
    while (i < (n - 1)) {
        const int c = fgetc(f);
        if (c == EOF) {
            break;
        }
        buf[i] = (char)c;
        i++;
        if (c == '\n') {
            break;
        }
    }
    buf[i] = '\0';
    return (i > 0) ? buf : BR_NULL;
}

/* 标准流符号: 默认 NULL(POSIX 的 0/1/2 属进程模型, 本代没有)。
 * `posix_init` 会把它们接到 fd 0/1/2 上 —— 那时它们才是真的流。 */
FILE *stdin  = BR_NULL;
FILE *stdout = BR_NULL;
FILE *stderr = BR_NULL;

/* ==================================================================== 层: string
 *
 * `mem*` **不在这里**(它们是 core 的符号, 见 string.h 的说明); `str*` 是本件定义的。
 */

size_t strlen(const char *s)
{
    size_t n = 0u;
    if (s == BR_NULL) {
        return 0u;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

char *strcpy(char *dst, const char *src)
{
    size_t i = 0u;
    while (src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0u;
    for (; (i < n) && (src[i] != '\0'); i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';                              /* POSIX: 补满 n 个 '\0' */
    }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    size_t n = strlen(dst);
    (void)strcpy(dst + n, src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    size_t i = strlen(dst);
    size_t k = 0u;
    for (; (k < n) && (src[k] != '\0'); k++) {
        dst[i + k] = src[k];
    }
    dst[i + k] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (int)((unsigned char)*a - (unsigned char)*b);
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)((unsigned char)a[i] - (unsigned char)b[i]);
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (*s == '\0') {
            return BR_NULL;
        }
    }
}

char *strrchr(const char *s, int c)
{
    const char *found = BR_NULL;
    for (;; s++) {
        if (*s == (char)c) {
            found = s;
        }
        if (*s == '\0') {
            return (char *)found;
        }
    }
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0u; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(br_uintptr_t)(p + i);
        }
    }
    return BR_NULL;
}

/* errno → 短名。返回的是**静态串**, 调用方不得释放(POSIX 同此)。 */
char *strerror(int err)
{
    static char buf[BR_POSIX_STRERROR_MAX];
    const char *name;

    switch (err) {
        case 0:              name = "Success"; break;
        case EPERM:          name = "EPERM"; break;
        case ENOENT:         name = "ENOENT"; break;
        case EINTR:          name = "EINTR"; break;
        case EIO:            name = "EIO"; break;
        case EBADF:          name = "EBADF"; break;
        case EAGAIN:         name = "EAGAIN"; break;
        case ENOMEM:         name = "ENOMEM"; break;
        case EACCES:         name = "EACCES"; break;
        case EFAULT:         name = "EFAULT"; break;
        case EBUSY:          name = "EBUSY"; break;
        case EEXIST:         name = "EEXIST"; break;
        case EXDEV:          name = "EXDEV"; break;
        case ENODEV:         name = "ENODEV"; break;
        case ENOTDIR:        name = "ENOTDIR"; break;
        case EISDIR:         name = "EISDIR"; break;
        case EINVAL:         name = "EINVAL"; break;
        case ENFILE:         name = "ENFILE"; break;
        case EMFILE:         name = "EMFILE"; break;
        case ENOTTY:         name = "ENOTTY"; break;
        case EFBIG:          name = "EFBIG"; break;
        case ENOSPC:         name = "ENOSPC"; break;
        case ESPIPE:         name = "ESPIPE"; break;
        case EROFS:          name = "EROFS"; break;
        case ERANGE:         name = "ERANGE"; break;
        case ENAMETOOLONG:   name = "ENAMETOOLONG"; break;
        case ENOSYS:         name = "ENOSYS"; break;
        case ENOTEMPTY:      name = "ENOTEMPTY"; break;
        case ELOOP:          name = "ELOOP"; break;
        case ENOTSUP:        name = "ENOTSUP"; break;
        case ETIMEDOUT:      name = "ETIMEDOUT"; break;
        default:             name = "UNKNOWN"; break;
    }
    (void)svc_copy(buf, name, (size_t)sizeof(buf));
    return buf;
}

/* ==================================================================== 层: stdlib */

void *malloc(size_t n)
{
    return br_malloc((br_size_t)n);
}

void *calloc(size_t n, size_t size)
{
    return br_calloc((br_size_t)n, (br_size_t)size);
}

void *realloc(void *p, size_t n)
{
    return br_realloc(p, (br_size_t)n);
}

void free(void *p)
{
    br_free(p);
}

int abs(int v)
{
    return (v < 0) ? -v : v;
}

long labs(long v)
{
    return (v < 0) ? -v : v;
}

/* 极简整数解析(TR-C 里"自写"的那条路的最划算入口; 完整 strtol 仍待做)。
 * 语义照 POSIX 的退化版: 跳过空白、可选符号、连续数字, 无数字 ⇒ 0, 溢出**不报**(饱和)。 */
static long svc_str_to_long(const char *s)
{
    long v = 0;
    int  neg = 0;

    if (s == BR_NULL) {
        return 0;
    }
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        if (v > (0x7fffffffffffffffL / 10L)) {
            break;                                  /* 饱和, 不绕回 */
        }
        v = (v * 10L) + (long)(*s - '0');
        s++;
    }
    return neg ? -v : v;
}

int atoi(const char *s)
{
    return (int)svc_str_to_long(s);
}

long atol(const char *s)
{
    return svc_str_to_long(s);
}

void abort(void)
{
    br_panic("[posix] abort()");
}

char *getenv(const char *name)
{
    (void)name;
    return BR_NULL;                                 /* 本代没有环境变量模型(如实回答) */
}

/* ==================================================================== 层: pthread */

/*
 * 每线程记录。一条记录同时承载四件事(它们都是 POSIX 要求"每线程一份"的东西):
 *   - **生命周期**: 与 core TCB 的对应 + `join` 的 retval + 自切栈的归属(文件头 ①②);
 *   - **detach 标记**: 惰性回收的判据(文件头 ③);
 *   - **errno 槽位**: `errno` 宏指向这里(文件头 ④);
 *   - **TLS key 槽位**: `pthread_setspecific/getspecific` 落在这里(文件头 ⑤)。
 *
 * 主线程(以及任何不是 `pthread_create` 创建的线程)在**第一次需要时**惰性登记一条
 * 记录 —— 于是"每线程 errno/TLS"对 APP 主线程同样成立, 不需要 core 的通用槽位。
 */
typedef struct {
    br_u32        used;
    br_thread_t  *t;
    void        *(*fn)(void *);
    void         *arg;
    void         *retval;
    void         *stack;        /* 本服务切的栈(join/reap 时归还) */
    int           owns_stack;
    int           detached;     /* PTHREAD_CREATE_DETACHED / pthread_detach() */
    int           err_val;      /* 每线程 errno */
    char          name[BR_PTHREAD_NAME_MAX];
    void         *key_values[SVC_KEY_MAX];
} svc_thread_t;

static svc_thread_t s_threads[SVC_THREAD_MAX];

/* key 注册表: 是否已用(位图)+ 析构函数。key 本身在进程内唯一(不随线程)。 */
static br_u32 s_key_used;
static void (*s_key_dtor[SVC_KEY_MAX])(void *);

/* 极端情形(errno 在内核线程之外/记录表满)的兜底槽位: 保证 `errno` 永远是可写的左值。 */
static int s_errno_fallback;

static svc_thread_t *svc_thread_find(br_thread_t *t)
{
    if (t == BR_NULL) {
        return BR_NULL;
    }
    for (br_u32 i = 0u; i < (br_u32)SVC_THREAD_MAX; i++) {
        if (s_threads[i].used == 1u && s_threads[i].t == t) {
            return &s_threads[i];
        }
    }
    return BR_NULL;
}

static void svc_thread_clear(svc_thread_t *rec)
{
    for (size_t i = 0u; i < sizeof(*rec); i++) {
        ((unsigned char *)rec)[i] = 0u;
    }
}

/* 当前线程的记录: 找不到就**惰性登记**一条(主线程/非 pthread 线程走这条路)。
 * `br_task_self()` 还不可用(极早期)⇒ 返回 NULL, 调用方退化到兜底槽位。 */
static svc_thread_t *svc_thread_self_rec(void)
{
    br_thread_t *self = br_task_self();
    if (self == BR_NULL) {
        return BR_NULL;
    }
    svc_thread_t *rec = svc_thread_find(self);
    if (rec != BR_NULL) {
        return rec;
    }
    for (br_u32 i = 0u; i < (br_u32)SVC_THREAD_MAX; i++) {
        if (s_threads[i].used == 0u) {
            rec = &s_threads[i];
            svc_thread_clear(rec);
            rec->used = 1u;
            rec->t    = self;
            svc_copy(rec->name, br_task_name(self), sizeof(rec->name));
            return rec;
        }
    }
    return BR_NULL;
}

/* 惰性回收: 把"已退出(ZOMBIE)且 detached"的线程 join 掉, 并还栈/清记录。
 * 在**每一次进入 pthread 层**时调用(见 pthread.h 文件头 ③)。跳过当前线程自己。 */
static void svc_reap_detached(void)
{
    br_thread_t *self = br_task_self();
    for (br_u32 i = 0u; i < (br_u32)SVC_THREAD_MAX; i++) {
        svc_thread_t *rec = &s_threads[i];
        if (rec->used == 0u || rec->detached == 0 || rec->t == BR_NULL || rec->t == self) {
            continue;
        }
        if (br_task_state(rec->t) != BR_TASK_ZOMBIE) {
            continue;                               /* 还在跑 ⇒ 下次再说 */
        }
        int code = 0;
        if (br_task_join(rec->t, &code) == 0) {
            if (rec->owns_stack != 0 && rec->stack != BR_NULL) {
                br_free(rec->stack);
            }
            svc_thread_clear(rec);
        }
    }
}

/* 线程退出时按 POSIX 语义跑 TLS 析构函数(最多 PTHREAD_DESTRUCTOR_ITERATIONS 轮,
 * 因为析构函数可以再 setspecific)。 */
static void svc_thread_run_key_dtors(void)
{
    svc_thread_t *rec = svc_thread_find(br_task_self());
    if (rec == BR_NULL) {
        return;
    }
    for (int iter = 0; iter < (int)PTHREAD_DESTRUCTOR_ITERATIONS; iter++) {
        int called = 0;
        for (br_u32 k = 0u; k < (br_u32)SVC_KEY_MAX; k++) {
            if (rec->key_values[k] != BR_NULL && s_key_dtor[k] != BR_NULL) {
                void *v = rec->key_values[k];
                rec->key_values[k] = BR_NULL;
                s_key_dtor[k](v);
                called = 1;
            }
        }
        if (called == 0) {
            break;
        }
    }
}

/* 入口跳板: core 的 `br_task_create` 只传一个 `void*`, 于是"函数 + 参数 + 返回值的落点"
 * 都挂在记录上。返回值的搬运是 pthread 与 core 的**唯一**形状差异(见 pthread.h ②)。 */
static void svc_thread_tramp(void *arg)
{
    svc_thread_t *rec = (svc_thread_t *)arg;
    rec->retval = rec->fn(rec->arg);
    svc_thread_run_key_dtors();                     /* POSIX: 线程退出跑 TLS 析构 */
    br_task_exit(0);
}

/* timespec(绝对时刻, MONOTONIC) → 相对微秒。过去时刻 ⇒ ZERO(trylock 语义)。 */
static br_time_t svc_abstime_to_rel(const struct timespec *abstime)
{
    const br_u64 abs_us = ((br_u64)abstime->tv_sec * 1000000u) +
                          ((br_u64)abstime->tv_nsec / 1000u);
    const br_u64 now    = br_clock_now();
    return (abs_us > now) ? (br_time_t)(abs_us - now) : BR_TIMEOUT_ZERO;
}

/* ==================================================================== errno */

int *br_posix_errno(void)
{
    svc_thread_t *rec = svc_thread_self_rec();
    return (rec != BR_NULL) ? &rec->err_val : &s_errno_fallback;
}

/* ==================================================================== 生命周期 */

int pthread_attr_init(pthread_attr_t *a)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->stack_size  = BR_POSIX_STACK_DEFAULT;
    a->stack       = BR_NULL;
    a->detachstate = PTHREAD_CREATE_JOINABLE;
    a->guard_size  = 0u;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a)
{
    (void)a;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t size)
{
    if (a == BR_NULL || size < BR_STACK_MIN) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->stack_size = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size)
{
    if (a == BR_NULL || size == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *size = a->stack_size;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *a, void *stack, size_t size)
{
    if (a == BR_NULL || stack == BR_NULL || size < BR_STACK_MIN) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->stack      = stack;
    a->stack_size = size;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **stack, size_t *size)
{
    if (a == BR_NULL || stack == BR_NULL || size == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *stack = a->stack;
    *size  = a->stack_size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
    if (a == BR_NULL ||
        (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->detachstate = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
    if (a == BR_NULL || state == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *state = a->detachstate;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *a, size_t size)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    /* core 没有 guard 页(栈是调用方给的连续内存)⇒ 只记录, 不改行为。如实返回成功
     * 会让 get 读回自己设的值; 想让调用方知道"没生效"就靠文档与这里的一句注释。 */
    a->guard_size = size;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size)
{
    if (a == BR_NULL || size == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *size = a->guard_size;
    return 0;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg)
{
    svc_reap_detached();
    if (out == BR_NULL || fn == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }

    svc_thread_t *rec = BR_NULL;
    for (br_u32 i = 0u; i < (br_u32)SVC_THREAD_MAX; i++) {
        if (s_threads[i].used == 0u) {
            rec = &s_threads[i];
            break;
        }
    }
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(BR_EAGAIN));         /* 线程表满(与 core 的 TCB 池同量级) */
    }

    size_t      ssize = BR_POSIX_STACK_DEFAULT;
    void       *stack = BR_NULL;
    int         owned = 0;
    int         detach = PTHREAD_CREATE_JOINABLE;

    if (attr != BR_NULL) {
        detach = attr->detachstate;
        if (attr->stack != BR_NULL) {
            stack = attr->stack;
            ssize = attr->stack_size;
        } else if (attr->stack_size != 0u) {
            ssize = attr->stack_size;
        }
    }
    if (stack == BR_NULL) {
        /* ★ 栈从 core 堆上切(POSIX 的签名里没有栈; 见 pthread.h ①) */
        stack = br_malloc((br_size_t)ssize);
        if (stack == BR_NULL) {
            return svc_rc(BR_ERR(BR_EAGAIN));
        }
        owned = 1;
    }

    br_task_attr_t ta;
    ta.name       = "pthread";
    ta.stack      = (void *)((unsigned char *)stack + ssize);   /* 栈顶 = 高地址端 */
    ta.stack_size = ssize;
    ta.prio       = 0u;
    ta.flags      = 0u;

    svc_thread_clear(rec);
    rec->used       = 1u;
    rec->fn         = fn;
    rec->arg        = arg;
    rec->retval     = BR_NULL;
    rec->stack      = stack;
    rec->owns_stack = owned;
    rec->detached   = (detach == PTHREAD_CREATE_DETACHED) ? 1 : 0;
    svc_copy(rec->name, "pthread", sizeof(rec->name));

    br_thread_t *t = BR_NULL;
    const int rc = br_task_create(&t, &ta, svc_thread_tramp, rec);
    if (rc != 0) {
        if (owned != 0) {
            br_free(stack);
        }
        svc_thread_clear(rec);
        return svc_rc(rc);
    }
    rec->t = t;
    *out   = t;
    return 0;
}

int pthread_join(pthread_t t, void **retval)
{
    svc_reap_detached();
    if (t == BR_NULL) {
        return svc_rc(BR_ERR(BR_ESRCH));
    }
    if (t == br_task_self()) {
        return svc_rc(BR_ERR(BR_EDEADLK));
    }
    svc_thread_t *rec = svc_thread_find(t);
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(BR_ESRCH));          /* 从未创建 / 已被 join 过 */
    }
    if (rec->detached != 0) {
        return svc_rc(BR_ERR(BR_EINVAL));         /* join detached 线程 = 未定义(glibc EINVAL) */
    }
    int       code = 0;
    const int rc   = br_task_join(t, &code);
    if (rc != 0) {
        return svc_rc(rc);
    }
    if (retval != BR_NULL) {
        *retval = rec->retval;                      /* core 只回 int, retval 在记录里 */
    }
    if (rec->owns_stack != 0 && rec->stack != BR_NULL) {
        br_free(rec->stack);                        /* 栈归谁切谁还 */
    }
    svc_thread_clear(rec);
    return 0;
}

void pthread_exit(void *retval)
{
    svc_thread_t *rec = svc_thread_find(br_task_self());
    if (rec != BR_NULL) {
        rec->retval = retval;                       /* 让 join 还能取到 */
    }
    svc_thread_run_key_dtors();
    br_task_exit(0);
}

int pthread_detach(pthread_t t)
{
    svc_reap_detached();
    if (t == BR_NULL) {
        return svc_rc(BR_ERR(BR_ESRCH));
    }
    svc_thread_t *rec = svc_thread_find(t);
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(BR_ESRCH));
    }
    if (rec->detached != 0) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    rec->detached = 1;
    return 0;                                       /* 回收是惰性的(见文件头 ③) */
}

pthread_t pthread_self(void)
{
    return br_task_self();
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return (a == b) ? 1 : 0;
}

int pthread_yield(void)
{
    svc_reap_detached();
    br_task_yield();
    return 0;
}

int pthread_setname_np(pthread_t t, const char *name)
{
    svc_reap_detached();
    if (t == BR_NULL) {
        t = br_task_self();
    }
    svc_thread_t *rec = (t == br_task_self()) ? svc_thread_self_rec()
                                              : svc_thread_find(t);
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(BR_ESRCH));
    }
    svc_copy(rec->name, name, sizeof(rec->name));
    return 0;
}

int pthread_getname_np(pthread_t t, char *buf, size_t len)
{
    svc_reap_detached();
    if (buf == BR_NULL || len == 0u) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    if (t == BR_NULL) {
        t = br_task_self();
    }
    svc_thread_t *rec = (t == br_task_self()) ? svc_thread_self_rec()
                                              : svc_thread_find(t);
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(BR_ESRCH));
    }
    svc_copy(buf, rec->name, len);
    return 0;
}

/* ==================================================================== once */

int pthread_once(pthread_once_t *once, void (*init)(void))
{
    svc_reap_detached();
    if (once == BR_NULL || init == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    if (once->done != 0) {
        return 0;                                   /* 无锁快路径(已完成后只读) */
    }
    const int rc = br_mutex_lock(&once->m);
    if (rc != 0) {
        return svc_rc(rc);
    }
    if (once->done == 0) {
        init();                                     /* POSIX: init 不得递归同一 once */
        once->done = 1;
    }
    (void)br_mutex_unlock(&once->m);
    return 0;
}

/* ==================================================================== TLS key */

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    svc_reap_detached();
    if (key == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    for (br_u32 i = 0u; i < (br_u32)SVC_KEY_MAX; i++) {
        if ((s_key_used & (1u << i)) == 0u) {
            s_key_used |= (1u << i);
            s_key_dtor[i] = destructor;
            *key = i;
            return 0;
        }
    }
    return svc_rc(BR_ERR(BR_EAGAIN));               /* key 用尽 */
}

int pthread_key_delete(pthread_key_t key)
{
    svc_reap_detached();
    if (key >= (pthread_key_t)SVC_KEY_MAX || (s_key_used & (1u << key)) == 0u) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    s_key_used &= ~(1u << key);
    s_key_dtor[key] = BR_NULL;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    svc_reap_detached();
    if (key >= (pthread_key_t)SVC_KEY_MAX || (s_key_used & (1u << key)) == 0u) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    svc_thread_t *rec = svc_thread_self_rec();
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(BR_EAGAIN));
    }
    rec->key_values[key] = (void *)value;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key >= (pthread_key_t)SVC_KEY_MAX || (s_key_used & (1u << key)) == 0u) {
        return BR_NULL;
    }
    svc_thread_t *rec = svc_thread_self_rec();
    return (rec != BR_NULL) ? rec->key_values[key] : BR_NULL;
}

/* ==================================================================== mutex */

int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->type     = PTHREAD_MUTEX_NORMAL;
    a->pshared  = PTHREAD_PROCESS_PRIVATE;
    a->protocol = PTHREAD_PRIO_NONE;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
    (void)a;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (a == BR_NULL ||
        (type != PTHREAD_MUTEX_NORMAL && type != PTHREAD_MUTEX_RECURSIVE &&
         type != PTHREAD_MUTEX_ERRORCHECK)) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->type = (unsigned)type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
    if (a == BR_NULL || type == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *type = (int)a->type;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int pshared)
{
    if (a == BR_NULL ||
        (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    /* 单地址空间: PROCESS_SHARED 可记录但没有跨进程语义(如实)。 */
    a->pshared = pshared;
    return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *pshared)
{
    if (a == BR_NULL || pshared == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *pshared = a->pshared;
    return 0;
}

int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int protocol)
{
    if (a == BR_NULL || protocol != PTHREAD_PRIO_NONE) {
        return svc_rc(BR_ERR(BR_ENOTSUP));          /* PI 属 v2(无优先级) */
    }
    a->protocol = protocol;
    return 0;
}

int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *a, int *protocol)
{
    if (a == BR_NULL || protocol == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *protocol = a->protocol;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    svc_reap_detached();
    if (m == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rc = br_mutex_init(&m->m);
    if (rc != 0) {
        return svc_rc(rc);
    }
    m->type  = (attr != BR_NULL) ? attr->type : PTHREAD_MUTEX_NORMAL;
    m->owner = BR_NULL;
    m->count = 0u;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    (void)m;
    return 0;                                       /* 空操作 ⇒ 0(见 pthread.h 的说明) */
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    svc_reap_detached();
    if (m == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    br_thread_t *self = br_task_self();

    /* RECURSIVE: 已持有 ⇒ 只加计数(core 的 mutex 是普通互斥量, 直接再锁会自死锁)。 */
    if (m->owner == self && m->count > 0u) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            m->count++;
            return 0;
        }
        if (m->type == PTHREAD_MUTEX_ERRORCHECK) {
            return svc_rc(BR_ERR(BR_EDEADLK));
        }
        /* NORMAL: POSIX 说自锁是未定义 ⇒ 落到 core(core 会自死锁, 与 Linux NORMAL 同型) */
    }
    const int rc = br_mutex_lock(&m->m);
    if (rc != 0) {
        return svc_rc(rc);
    }
    m->owner = self;
    m->count = 1u;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    svc_reap_detached();
    if (m == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    br_thread_t *self = br_task_self();
    if (m->owner == self && m->count > 0u) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            m->count++;
            return 0;
        }
        return svc_rc(BR_ERR(BR_EBUSY));            /* NORMAL/ERRORCHECK: 自持 ⇒ EBUSY */
    }
    /* core 的 `lock_to(ZERO)` = trylock: 忙 ⇒ **-ETIMEDOUT**(INV-1 的统一超时码)。
     * POSIX 的 trylock 要 EBUSY —— 这一处**必须**翻译, 否则调用方按 EBUSY 写的分支
     * 永远不进(这是本文件里极少数的"非零转换"点)。 */
    const int rc = br_mutex_lock_to(&m->m, BR_TIMEOUT_ZERO);
    if (rc == BR_ERR(BR_ETIMEDOUT)) {
        return svc_rc(BR_ERR(BR_EBUSY));
    }
    if (rc != 0) {
        return svc_rc(rc);
    }
    m->owner = self;
    m->count = 1u;
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime)
{
    svc_reap_detached();
    if (m == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    br_thread_t *self = br_task_self();
    if (m->owner == self && m->count > 0u) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            m->count++;
            return 0;
        }
        if (m->type == PTHREAD_MUTEX_ERRORCHECK) {
            return svc_rc(BR_ERR(BR_EDEADLK));
        }
    }
    const int rc = br_mutex_lock_to(&m->m, svc_abstime_to_rel(abstime));
    if (rc == BR_ERR(BR_ETIMEDOUT)) {
        return svc_rc(BR_ERR(BR_ETIMEDOUT));
    }
    if (rc != 0) {
        return svc_rc(rc);
    }
    m->owner = self;
    m->count = 1u;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    svc_reap_detached();
    if (m == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    br_thread_t *self = br_task_self();
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != self) {
            return svc_rc(BR_ERR(BR_EPERM));        /* ERRORCHECK/RECURSIVE: 非属主解锁 */
        }
    }
    if (m->type == PTHREAD_MUTEX_RECURSIVE && m->count > 1u) {
        m->count--;
        return 0;
    }
    m->count = 0u;
    m->owner = BR_NULL;
    const int rc = br_mutex_unlock(&m->m);
    return (rc != 0) ? svc_rc(rc) : 0;
}

/* ==================================================================== cond */

int pthread_condattr_init(pthread_condattr_t *a)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->clock   = CLOCK_MONOTONIC;
    a->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *a)
{
    (void)a;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *a, int clock)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    /* 本代只有单调钟(P-1: 无墙钟)⇒ REALTIME 明确拒绝, 不假装支持。 */
    if (clock != CLOCK_MONOTONIC) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *a, int *clock)
{
    if (a == BR_NULL || clock == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *clock = a->clock;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *a, int pshared)
{
    if (a == BR_NULL ||
        (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->pshared = pshared;
    return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t *a, int *pshared)
{
    if (a == BR_NULL || pshared == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *pshared = a->pshared;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    svc_reap_detached();
    if (c == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rc = br_cond_init(&c->c);
    if (rc != 0) {
        return svc_rc(rc);
    }
    c->clock = (attr != BR_NULL) ? attr->clock : CLOCK_MONOTONIC;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    svc_reap_detached();
    if (c == BR_NULL || m == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rc = br_cond_wait(&c->c, &m->m, BR_TIMEOUT_INF);
    return (rc != 0) ? svc_rc(rc) : 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime)
{
    svc_reap_detached();
    if (c == BR_NULL || m == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    /* ★ 绝对时刻按 **MONOTONIC** 解释: 本代没有墙钟(P-1), 于是"绝对时间"的唯一可用
     *   时基就是单调钟。POSIX 默认是 CLOCK_REALTIME ⇒ 这是一条**语义偏离**, 与
     *   `clock_gettime(CLOCK_REALTIME) = -ENOTSUP` 是同一条事实的两面; 用
     *   `pthread_condattr_setclock(CLOCK_MONOTONIC)` 可把意图写进声明。 */
    const int rc = br_cond_wait(&c->c, &m->m, svc_abstime_to_rel(abstime));
    if (rc == BR_ERR(BR_ETIMEDOUT)) {
        return svc_rc(BR_ERR(BR_ETIMEDOUT));      /* POSIX 的 ETIMEDOUT */
    }
    return (rc != 0) ? svc_rc(rc) : 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    if (c == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rc = br_cond_signal(&c->c);
    return (rc != 0) ? svc_rc(rc) : 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    if (c == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rc = br_cond_broadcast(&c->c);
    return (rc != 0) ? svc_rc(rc) : 0;
}

/* ==================================================================== rwlock */

int pthread_rwlockattr_init(pthread_rwlockattr_t *a)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->kind    = PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP;
    a->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a)
{
    (void)a;
    return 0;
}

int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *a, int kind)
{
    if (a == BR_NULL ||
        (kind != PTHREAD_RWLOCK_PREFER_READER_NP &&
         kind != PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP)) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->kind = kind;
    return 0;
}

int pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *a, int *kind)
{
    if (a == BR_NULL || kind == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *kind = a->kind;
    return 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *a, int pshared)
{
    if (a == BR_NULL ||
        (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->pshared = pshared;
    return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *a, int *pshared)
{
    if (a == BR_NULL || pshared == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *pshared = a->pshared;
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *attr)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rm = br_mutex_init(&rw->m);
    if (rm != 0) {
        return svc_rc(rm);
    }
    const int rc = br_cond_init(&rw->c);
    if (rc != 0) {
        return svc_rc(rc);
    }
    rw->readers         = 0u;
    rw->writer          = BR_NULL;
    rw->writers_waiting = 0u;
    rw->kind = (attr != BR_NULL) ? attr->kind
                                 : PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rw)
{
    (void)rw;
    return 0;                                       /* 空操作(同 mutex/cond 的理由) */
}

/* 读锁的公共实现: `timeout` 为 BR_TIMEOUT_ZERO 时是 trylock。 */
static int svc_rwlock_rdlock_to(pthread_rwlock_t *rw, br_time_t timeout)
{
    const int lk = br_mutex_lock(&rw->m);
    if (lk != 0) {
        return svc_rc(lk);
    }
    for (;;) {
        const br_bool writer = (rw->writer != BR_NULL);
        /* writer-preference: 有写者等待时新的读者让路(避免写者饿死)。
         * ★ 不支持的边界: **同一线程递归读**在有写者等待时会自阻(如实登记, 见 pthread.h)。 */
        const br_bool blocked =
            writer || (rw->kind == PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP &&
                       rw->writers_waiting > 0u);
        if (!blocked) {
            rw->readers++;
            (void)br_mutex_unlock(&rw->m);
            return 0;
        }
        if (timeout == BR_TIMEOUT_ZERO) {
            (void)br_mutex_unlock(&rw->m);
            return svc_rc(BR_ERR(BR_EBUSY));
        }
        const int w = br_cond_wait(&rw->c, &rw->m, timeout);
        if (w == BR_ERR(BR_ETIMEDOUT)) {
            (void)br_mutex_unlock(&rw->m);
            return svc_rc(BR_ERR(BR_ETIMEDOUT));
        }
        if (w != 0) {
            (void)br_mutex_unlock(&rw->m);
            return svc_rc(w);
        }
    }
}

static int svc_rwlock_wrlock_to(pthread_rwlock_t *rw, br_time_t timeout)
{
    const int lk = br_mutex_lock(&rw->m);
    if (lk != 0) {
        return svc_rc(lk);
    }
    if (timeout == BR_TIMEOUT_ZERO) {
        if (rw->writer == BR_NULL && rw->readers == 0u) {
            rw->writer = br_task_self();
            (void)br_mutex_unlock(&rw->m);
            return 0;
        }
        (void)br_mutex_unlock(&rw->m);
        return svc_rc(BR_ERR(BR_EBUSY));
    }
    rw->writers_waiting++;
    for (;;) {
        if (rw->writer == BR_NULL && rw->readers == 0u) {
            rw->writers_waiting--;
            rw->writer = br_task_self();
            (void)br_mutex_unlock(&rw->m);
            return 0;
        }
        const int w = br_cond_wait(&rw->c, &rw->m, timeout);
        if (w == BR_ERR(BR_ETIMEDOUT)) {
            rw->writers_waiting--;
            (void)br_cond_broadcast(&rw->c);        /* 可能空出了一个读名额 */
            (void)br_mutex_unlock(&rw->m);
            return svc_rc(BR_ERR(BR_ETIMEDOUT));
        }
        if (w != 0) {
            rw->writers_waiting--;
            (void)br_mutex_unlock(&rw->m);
            return svc_rc(w);
        }
    }
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    return svc_rwlock_rdlock_to(rw, BR_TIMEOUT_INF);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    return svc_rwlock_rdlock_to(rw, BR_TIMEOUT_ZERO);
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *rw, const struct timespec *abstime)
{
    svc_reap_detached();
    if (rw == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    return svc_rwlock_rdlock_to(rw, svc_abstime_to_rel(abstime));
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    return svc_rwlock_wrlock_to(rw, BR_TIMEOUT_INF);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    return svc_rwlock_wrlock_to(rw, BR_TIMEOUT_ZERO);
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *rw, const struct timespec *abstime)
{
    svc_reap_detached();
    if (rw == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    return svc_rwlock_wrlock_to(rw, svc_abstime_to_rel(abstime));
}

int pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int lk = br_mutex_lock(&rw->m);
    if (lk != 0) {
        return svc_rc(lk);
    }
    if (rw->writer == br_task_self()) {
        rw->writer = BR_NULL;
        (void)br_cond_broadcast(&rw->c);
    } else if (rw->readers > 0u) {
        rw->readers--;
        if (rw->readers == 0u) {
            (void)br_cond_broadcast(&rw->c);        /* 最后一个读者 ⇒ 唤醒写者 */
        }
    } else {
        (void)br_mutex_unlock(&rw->m);
        return svc_rc(BR_ERR(BR_EPERM));            /* 没持锁就解锁 */
    }
    (void)br_mutex_unlock(&rw->m);
    return 0;
}

/* ==================================================================== barrier */

int pthread_barrierattr_init(pthread_barrierattr_t *a)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->pshared = PTHREAD_PROCESS_PRIVATE;
    return 0;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *a)
{
    (void)a;
    return 0;
}

int pthread_barrierattr_setpshared(pthread_barrierattr_t *a, int pshared)
{
    if (a == BR_NULL ||
        (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    a->pshared = pshared;
    return 0;
}

int pthread_barrierattr_getpshared(const pthread_barrierattr_t *a, int *pshared)
{
    if (a == BR_NULL || pshared == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *pshared = a->pshared;
    return 0;
}

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr,
                         unsigned count)
{
    svc_reap_detached();
    (void)attr;
    if (b == BR_NULL || count == 0u) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rm = br_mutex_init(&b->m);
    if (rm != 0) {
        return svc_rc(rm);
    }
    const int rc = br_cond_init(&b->c);
    if (rc != 0) {
        return svc_rc(rc);
    }
    b->count      = count;
    b->waiting    = 0u;
    b->generation = 0u;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b)
{
    (void)b;
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *b)
{
    svc_reap_detached();
    if (b == BR_NULL || b->count == 0u) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int lk = br_mutex_lock(&b->m);
    if (lk != 0) {
        return svc_rc(lk);
    }
    const unsigned gen = b->generation;
    b->waiting++;
    if (b->waiting == b->count) {
        b->waiting = 0u;
        b->generation++;
        (void)br_cond_broadcast(&b->c);
        (void)br_mutex_unlock(&b->m);
        return PTHREAD_BARRIER_SERIAL_THREAD;       /* 恰好一个线程拿到 */
    }
    while (b->generation == gen) {
        const int w = br_cond_wait(&b->c, &b->m, BR_TIMEOUT_INF);
        if (w != 0) {
            (void)br_mutex_unlock(&b->m);
            return svc_rc(w);
        }
    }
    (void)br_mutex_unlock(&b->m);
    return 0;
}

/* ==================================================================== 层: semaphore */

int sem_init(sem_t *s, int pshared, unsigned value)
{
    if (s == BR_NULL || value > BR_POSIX_SEM_VALUE_MAX) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    (void)pshared;                                  /* 单地址空间 ⇒ pshared 无意义 */
    const int rc = br_sem_init(s, value);
    return (rc != 0) ? svc_rc(rc) : 0;
}

int sem_destroy(sem_t *s)
{
    (void)s;
    return 0;
}

int sem_wait(sem_t *s)
{
    if (s == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rc = br_sem_take(s, BR_TIMEOUT_INF);
    return (rc != 0) ? svc_rc(rc) : 0;
}

int sem_trywait(sem_t *s)
{
    if (s == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    /* core 的 `take(ZERO)` 忙 ⇒ -ETIMEDOUT(INV-1); POSIX 要 EAGAIN ⇒ 翻译一次。 */
    const int rc = br_sem_take(s, BR_TIMEOUT_ZERO);
    if (rc == BR_ERR(BR_ETIMEDOUT)) {
        return svc_rc(BR_ERR(BR_EAGAIN));
    }
    return (rc != 0) ? svc_rc(rc) : 0;
}

int sem_timedwait(sem_t *s, const struct timespec *abstime)
{
    if (s == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const br_u64 abs_us = ((br_u64)abstime->tv_sec * 1000000u) +
                          ((br_u64)abstime->tv_nsec / 1000u);
    const br_u64 now    = br_clock_now();
    const br_time_t rel = (abs_us > now) ? (br_time_t)(abs_us - now) : BR_TIMEOUT_ZERO;
    const int rc = br_sem_take(s, rel);
    return (rc != 0) ? svc_rc(rc) : 0;
}

int sem_post(sem_t *s)
{
    if (s == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    const int rc = br_sem_give(s);                  /* ISR-safe(core 的契约) */
    return (rc != 0) ? svc_rc(rc) : 0;
}

int sem_getvalue(sem_t *s, int *out)
{
    if (s == BR_NULL || out == BR_NULL) {
        return svc_rc(BR_ERR(BR_EINVAL));
    }
    *out = (int)br_sem_count(s);
    return 0;
}

/* ==================================================================== 层: 进程身份 */

pid_t getpid(void)  { return 1; }                   /* 单 APP ⇒ 常量 */
pid_t getppid(void) { return 0; }
uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }

/* ==================================================================== 生命周期钩子 */

int posix_early_init(void)
{
    /* EARLY: 只做"静态量归零"。fd 表在 framework/file-table 里(它自己的 EARLY 负责),
     * 本件不碰它 —— 那是单一主人的纪律。 */
    errno   = 0;
    s_umask = 0u;
    (void)svc_copy(s_cwd, "/", (size_t)sizeof(s_cwd));
    for (br_u32 i = 0u; i < (br_u32)SVC_LOCK_MAX; i++) {
        s_locks[i].used = 0u;
    }
    for (br_u32 i = 0u; i < (br_u32)SVC_THREAD_MAX; i++) {
        s_threads[i].used = 0u;
        s_threads[i].t    = BR_NULL;
    }
    return 0;
}

int posix_init(void)
{
    /* LATE(service 的相位): 此时 core 的堆/调度/服务注册表都已就绪。
     * v1 **不**自动打开 fd 0/1/2: 那是"进程模型"的一部分, 而本代没有进程模型
     * (stdio.h 的说明)。标准流符号保持 NULL, 直到有人显式 fdopen 它们。 */
    return 0;
}

int posix_start(void)
{
    br_log_info("runtime/posix: POSIX 运行时就绪 (fd 表由 framework/file-table 持有, cwd=%s)", s_cwd);
    br_log_info("runtime/posix: 覆盖 TR-A/TR-B; 未做项见 docs/11-service/11-02-svc-posix-subset.md");
    return 0;
}
