/*
 * brickOS prototype v0.2.0 — POSIX 运行时的一致性套件(runtime/posix/src/posix_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与核心代码分离)、
 *                 `docs/decisions/0014-svc-posix-posix-runtime.md`(覆盖范围与欠账)。
 *
 * ## 本套件有一条与其他插件不同的纪律: **只经 POSIX 头文件驱动**
 *   它 `#include <unistd.h>` / `<fcntl.h>` / `<sys/stat.h>` … —— 被测代码一律走 POSIX 名字,
 *   只有 `br_log_info`(观测)与 `br_clock_now`(计时参考)是 native 的。理由: 本插件的产出
 *   就是"POSIX 面", 若用例走 native API 去测, 那么**面本身**(名字、签名、错误通道)就没被
 *   测过 —— 而那正是最容易出错的地方(pthread 返回错误号 vs 系统调用返回 -1 + errno 是
 *   两套通道, 用错了症状极隐蔽)。
 *
 * ## 用 tmpfs 当介质, 但不依赖它的实现细节
 *   用例在 "/" 上建自己的临时目录(`/posixconf.d`), 只用 POSIX 面的行为做断言。于是
 *   "rootfs 是 tmpfs"对套件不可见 —— 换掉 FS 这套用例照跑(vfs 的 `[VFSCONF]` 守同一条纪律)。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[POSIXCONF] PASS/FAIL ...` + `SUMMARY`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 `posix-test`)。
 *
 * WORKAROUND(br-wa-test-001): `TC-POSIX-*` 是**自编号**(设计 `6-01` 没有这一组);
 * 自述文字才是判据内容。设计侧需先决定"POSIX 用例归 `6-01` 的哪一节"。
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

#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* 自检入口原型(定义在本文件末尾; 生成物 `plugin_desc.c` 的 `.selftest` 钩子引用它)。 */
int posix_selftest(void);

/* ==================================================================== 装置 */

#define PT_BASE   "/posixconf.d"
#define PT_PATH   128u

static br_u32 s_pass;
static br_u32 s_fail;

static void pc(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[POSIXCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[POSIXCONF] FAIL %s %s", tag, what);
    }
}

static void pt_path(char *dst, unsigned cap, const char *name)
{
    unsigned n = 0u;
    const char *b = PT_BASE;
    while (b[n] != '\0' && n + 1u < cap) {
        dst[n] = b[n];
        n++;
    }
    if (n + 1u < cap) {
        dst[n] = '/';
        n++;
    }
    for (unsigned i = 0u; name[i] != '\0' && n + 1u < cap; i++) {
        dst[n] = name[i];
        n++;
    }
    dst[n] = '\0';
}

/* 造一个文件并写满内容(后续用例共用)。返回 0 / -1。 */
static int pt_make(const char *path, const char *content, unsigned len)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return -1;
    }
    ssize_t w = 0;
    if (len > 0u) {
        w = write(fd, content, (size_t)len);
    }
    (void)close(fd);
    return (w == (ssize_t)len) ? 0 : -1;
}

/* 读回全部内容(不超过 cap), 返回字节数或 -1。 */
static long pt_read_all(const char *path, char *buf, unsigned cap)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    const ssize_t r = read(fd, buf, (size_t)cap);
    (void)close(fd);
    return (long)r;
}

/* ---- pthread 用例的共享装置 ---- */
static sem_t       s_sem_req;
static sem_t       s_sem_done;
static int         s_worker_value;
static br_bool     s_worker_ran;

static void *pt_worker(void *arg)
{
    (void)arg;
    if (sem_wait(&s_sem_req) != 0) {
        return (void *)2L;
    }
    s_worker_value = 42;
    s_worker_ran   = BR_TRUE;
    (void)sem_post(&s_sem_done);
    return (void *)7L;                     /* 这个值必须经由 pthread_join 回到调用方 */
}

/* ==================================================================== 用例 */

int posix_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    char p1[PT_PATH];
    char p2[PT_PATH];
    char p3[PT_PATH];

    br_log_info("[POSIXCONF] runtime/posix conformance (fd / 元数据 / 目录 / 链接 / 线程 / stdio)");

    (void)chdir("/");
    (void)unlink(PT_BASE "/a.txt");
    (void)unlink(PT_BASE "/b.txt");
    (void)unlink(PT_BASE "/c.txt");
    (void)unlink(PT_BASE "/l1");
    (void)unlink(PT_BASE "/h1");
    (void)unlink(PT_BASE "/rel.txt");
    (void)rmdir(PT_BASE);
    (void)mkdir(PT_BASE, 0777);
    pt_path(p1, PT_PATH, "a.txt");
    pt_path(p2, PT_PATH, "b.txt");
    pt_path(p3, PT_PATH, "c.txt");

    /* ---- TC-POSIX-001: open/close/read/write/lseek 往返 ---- */
    {
        const int fd = open(p1, O_RDWR | O_CREAT | O_TRUNC);
        br_bool ok = (fd >= 0);
        if (ok) {
            ok = (write(fd, "brickOS", 7u) == 7);
            ok = ok && (lseek(fd, 0, SEEK_SET) == 0);
            char buf[16];
            ok = ok && (read(fd, buf, 7u) == 7) && (buf[0] == 'b') && (buf[6] == 'S');
            ok = ok && (lseek(fd, 0, SEEK_END) == 7);
            ok = ok && (read(fd, buf, 4u) == 0);            /* 到 EOF ⇒ 0, 不是错误 */
            ok = ok && (close(fd) == 0);
            ok = ok && (close(fd) == -1) && (errno == EBADF);   /* 二次 close ⇒ EBADF */
        }
        pc(ok, "TC-POSIX-001", "open/read/write/lseek/close 往返; EOF ⇒ 0; 二次 close ⇒ EBADF");
    }

    /* ---- TC-POSIX-002: 最小可用 fd / dup 共享偏移 / dup2 顶替 ---- */
    {
        const int fd = open(p1, O_RDWR);
        const int d1 = dup(fd);
        int       d2 = -1;
        br_bool   ok = (fd >= 0) && (d1 >= 0) && (d1 != fd);

        /* dup 出来的两个 fd 共享同一个 open file description ⇒ 偏移共享 */
        ok = ok && (lseek(fd, 3, SEEK_SET) == 3) && (lseek(d1, 0, SEEK_CUR) == 3);

        /* dup2 到已占用的号: 旧句柄被顶掉 */
        d2 = open(p1, O_RDONLY);
        ok = ok && (d2 >= 0) && (dup2(fd, d2) == d2);
        ok = ok && (lseek(d2, 0, SEEK_CUR) == 3);

        /* 关掉 d2 不影响 fd/d1(还有其它引用) */
        ok = ok && (close(d2) == 0) && (lseek(fd, 0, SEEK_CUR) == 3);

        ok = ok && (close(d1) == 0) && (close(fd) == 0);
        pc(ok, "TC-POSIX-002", "dup 共享偏移; dup2 顶替目标; 关一个引用不影响其它");
    }

    /* ---- TC-POSIX-003: 访问模式执法(**本服务的职责**: vfs 刻意不做) ---- */
    {
        char    buf[8];
        const int ro = open(p1, O_RDONLY);
        const int wo = open(p1, O_WRONLY);
        br_bool   ok = (ro >= 0) && (wo >= 0);

        ok = ok && (write(ro, "x", 1u) == -1) && (errno == EBADF);   /* 只读 fd 上写 */
        ok = ok && (read(wo, buf, 1u) == -1) && (errno == EBADF);    /* 只写 fd 上读 */
        ok = ok && (ftruncate(ro, 1) == -1);                          /* 只读 fd 上截断 */
        ok = ok && (close(ro) == 0) && (close(wo) == 0);
        pc(ok, "TC-POSIX-003", "只读 fd 上 write/ftruncate ⇒ EBADF; 只写 fd 上 read ⇒ EBADF");
    }

    /* ---- TC-POSIX-004: pread/pwrite 不动偏移; readv/writev 拼接 ---- */
    {
        const int fd = open(p1, O_RDWR | O_TRUNC);
        br_bool   ok = (fd >= 0);
        char      buf[16];
        struct iovec wi[2];
        struct iovec ri[2];
        char         b1[4];
        char         b2[4];

        const ssize_t pw = pwrite(fd, "0123", 4u, 0);
        ok = ok && (pw == 4);
        ok = ok && (lseek(fd, 0, SEEK_CUR) == 0);          /* pwrite 不动偏移 */
        const ssize_t pr = pread(fd, buf, 4u, 0);
        ok = ok && (pr == 4) && (buf[0] == '0') && (buf[3] == '3');
        ok = ok && (lseek(fd, 0, SEEK_CUR) == 0);          /* pread 不动偏移 */

        wi[0].iov_base = (void *)"AB";
        wi[0].iov_len  = 2u;
        wi[1].iov_base = (void *)"CD";
        wi[1].iov_len  = 2u;
        const off_t sk = lseek(fd, 4, SEEK_SET);
        ok = ok && (sk == 4);
        const ssize_t wv = writev(fd, wi, 2);              /* 追加到 4..8(不是覆盖 0..4) */
        ok = ok && (wv == 4);
        ok = ok && (lseek(fd, 4, SEEK_SET) == 4);          /* 回到刚写的开头再读回 */

        ri[0].iov_base = b1;
        ri[0].iov_len  = 2u;
        ri[1].iov_base = b2;
        ri[1].iov_len  = 2u;
        const ssize_t rv = readv(fd, ri, 2);
        ok = ok && (rv == 4) && (b1[0] == 'A') && (b2[1] == 'D');
        ok = ok && (close(fd) == 0);
        pc(ok, "TC-POSIX-004", "pread/pwrite 保持偏移; readv/writev 按 iovec 拼接");
    }

    /* ---- TC-POSIX-005: stat / fstat / lstat 与类型位 ---- */
    {
        struct stat st;
        struct stat fs;
        const int   fd = open(p1, O_RDONLY);
        br_bool     ok = (stat(p1, &st) == 0) && S_ISREG(st.st_mode) && (st.st_size == 8);
        ok = ok && (lstat(p1, &fs) == 0) && S_ISREG(fs.st_mode);
        ok = ok && (fd >= 0) && (fstat(fd, &fs) == 0) && (fs.st_size == st.st_size);
        ok = ok && (st.st_nlink == 1);
        ok = ok && (stat(PT_BASE, &st) == 0) && S_ISDIR(st.st_mode);
        ok = ok && (fd >= 0) && (close(fd) == 0);
        pc(ok, "TC-POSIX-005", "stat/fstat/lstat 一致; S_ISREG/S_ISDIR; st_nlink = 1");
    }

    /* ---- TC-POSIX-006: 名字空间操作与 errno 映射 ---- */
    {
        br_bool ok = (mkdir(PT_BASE, 0777) == -1) && (errno == EEXIST);
        ok = ok && (rename(p1, p2) == 0);
        ok = ok && (access(p1, F_OK) == -1) && (errno == ENOENT);
        ok = ok && (access(p2, F_OK) == 0);
        ok = ok && (unlink(p1) == -1) && (errno == ENOENT);
        ok = ok && (rmdir(PT_BASE) == -1) && (errno == ENOTEMPTY);   /* 目录非空 */
        ok = ok && (mkdir(p3, 0777) == 0) && (rmdir(p3) == 0);
        ok = ok && (remove(p2) == 0) && (access(p2, F_OK) == -1);
        pc(ok, "TC-POSIX-006", "mkdir/rename/unlink/rmdir/remove; EEXIST/ENOENT/ENOTEMPTY 映射");
    }

    /* ---- TC-POSIX-007: 符号链接(lstat 看链接, stat 跟随, readlink 读目标) ---- */
    {
        char      tgt[64];
        struct stat lst;
        struct stat fst;
        (void)pt_make(p2, "linked", 6u);
        pt_path(p1, PT_PATH, "l1");

        br_bool ok = (symlink("b.txt", p1) == 0);
        ok = ok && (symlink("b.txt", p1) == -1) && (errno == EEXIST);
        ok = ok && (lstat(p1, &lst) == 0) && S_ISLNK(lst.st_mode) && (lst.st_size == 5);
        ok = ok && (stat(p1, &fst) == 0) && S_ISREG(fst.st_mode) && (fst.st_size == 6);
        const ssize_t n = readlink(p1, tgt, sizeof(tgt));
        ok = ok && (n == 5) && (tgt[0] == 'b') && (tgt[4] == 't');

        /* 经链接读 = 读目标 */
        char buf[8];
        ok = ok && (pt_read_all(p1, buf, sizeof(buf)) == 6) && (buf[0] == 'l');

        /* O_NOFOLLOW 看链接本身 ⇒ ELOOP */
        ok = ok && (open(p1, O_RDONLY | O_NOFOLLOW) == -1) && (errno == ELOOP);
        ok = ok && (readlink(p2, tgt, sizeof(tgt)) == -1) && (errno == EINVAL);
        ok = ok && (unlink(p1) == 0);
        pc(ok, "TC-POSIX-007", "symlink/readlink/lstat/stat; EEXIST/ELOOP/EINVAL; 经链接读写目标");
    }

    /* ---- TC-POSIX-008: 硬链接共享同一份数据 ---- */
    {
        struct stat st;
        pt_path(p1, PT_PATH, "h1");
        (void)pt_make(p2, "shared", 6u);

        br_bool ok = (link(p2, p1) == 0);
        ok = ok && (stat(p2, &st) == 0) && (st.st_nlink == 2);
        ok = ok && (link(p2, PT_BASE) == -1) && (errno == EEXIST || errno == EPERM ||
                                                 errno == EISDIR || errno == ENOENT);

        /* 经一个名字改内容, 另一个名字必须看见 */
        const int fd = open(p1, O_WRONLY);
        ok = ok && (fd >= 0) && (write(fd, "SHARED", 6u) == 6) && (close(fd) == 0);
        char buf[8];
        ok = ok && (pt_read_all(p2, buf, sizeof(buf)) == 6) && (buf[0] == 'S');

        /* 删一个名字: 数据还在 */
        ok = ok && (unlink(p2) == 0) && (stat(p1, &st) == 0) && (st.st_nlink == 1);
        ok = ok && (pt_read_all(p1, buf, sizeof(buf)) == 6) && (buf[3] == 'R');
        ok = ok && (unlink(p1) == 0);
        /* 下一个用例还要用 b.txt ⇒ 这里补回来(用例之间不靠"上一个人没收尾") */
        ok = ok && (pt_make(p2, "shared", 6u) == 0);
        pc(ok, "TC-POSIX-008", "link 共享数据体; nlink 计数; 删一个名字数据仍在");
    }

    /* ---- TC-POSIX-009: opendir/readdir/closedir ---- */
    {
        char p4[PT_PATH];
        pt_path(p4, PT_PATH, "d1");
        (void)mkdir(p4, 0777);
        char pf[PT_PATH];
        pt_path(pf, PT_PATH, "d1/x.bin");
        (void)pt_make(pf, "x", 1u);

        DIR *d = opendir(p4);
        br_bool ok = (d != BR_NULL);
        int      n_dir = 0;
        int      n_reg = 0;
        int      n_all = 0;
        if (d != BR_NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != BR_NULL) {
                n_all++;
                if (e->d_type == DT_DIR)  { n_dir++; }
                if (e->d_type == DT_REG)  { n_reg++; }
            }
            ok = ok && (closedir(d) == 0);
        }
        /* "." + ".." + x.bin = 3 条, 其中 REG 恰 1 条 */
        ok = ok && (n_all == 3) && (n_reg == 1) && (n_dir == 2);
        ok = ok && (opendir("/posixconf_no_such_dir") == BR_NULL);
        ok = ok && (unlink(pf) == 0) && (rmdir(p4) == 0);
        pc(ok, "TC-POSIX-009", "opendir/readdir/closedir: '.'+'..'+1 文件; d_type 正确");
    }

    /* ---- TC-POSIX-010: chdir / getcwd / 相对路径 ---- */
    {
        char cwd[PT_PATH];
        br_bool ok = (getcwd(cwd, sizeof(cwd)) != BR_NULL) && (cwd[0] == '/');
        ok = ok && (chdir(PT_BASE) == 0);
        ok = ok && (getcwd(cwd, sizeof(cwd)) != BR_NULL) && (cwd[0] == '/' );
        /* 相对路径落到新 cwd 上 */
        ok = ok && (pt_make("rel.txt", "rel", 3u) == 0);
        struct stat st;
        ok = ok && (stat(PT_BASE "/rel.txt", &st) == 0) && (st.st_size == 3);
        ok = ok && (chdir("..") == 0);
        char cwd2[PT_PATH];
        ok = ok && (getcwd(cwd2, sizeof(cwd2)) != BR_NULL) && (cwd2[1] == '\0'); /* 回到 "/" */
        ok = ok && (chdir("/posixconf_no_such_dir") == -1) && (errno == ENOENT);
        ok = ok && (unlink(PT_BASE "/rel.txt") == 0);
        ok = ok && (getcwd(cwd, 1u) == BR_NULL) && (errno == ERANGE);   /* 缓冲太小 */
        pc(ok, "TC-POSIX-010", "chdir/getcwd 往返; 相对路径落到新 cwd; 小缓冲 ⇒ ERANGE");
    }

    /* ---- TC-POSIX-011: 权限面 = 空操作返回 0(用户裁定) + 存在性照查 ---- */
    {
        br_bool ok = (chmod(p2, 0600) == 0) && (chown(p2, 100, 100) == 0);
        const mode_t old = umask(0022);
        ok = ok && (umask(old) == 0022);
        const int fd = open(p2, O_RDONLY);
        ok = ok && (fd >= 0) && (fchmod(fd, 0700) == 0) && (fchown(fd, 0, 0) == 0) && (close(fd) == 0);
        ok = ok && (access(p2, R_OK | W_OK | X_OK) == 0);           /* 一律放行 */
        ok = ok && (access("/posixconf_no_such_file", F_OK) == -1) && (errno == ENOENT);
        pc(ok, "TC-POSIX-011", "chmod/chown/fchmod/fchown/umask ⇒ 0; access 查存在性但恒放行");
    }

    /* ---- TC-POSIX-012: openat(AT_FDCWD) 可用; 其它 dirfd 如实 -ENOTSUP ---- */
    {
        const int fd = openat(AT_FDCWD, p2, O_RDONLY);
        br_bool ok = (fd >= 0) && (close(fd) == 0);
        ok = ok && (openat(0, p2, O_RDONLY) == -1) && (errno == ENOTSUP);
        /* O_DIRECTORY 用错 ⇒ ENOTDIR */
        ok = ok && (open(p2, O_RDONLY | O_DIRECTORY) == -1) && (errno == ENOTDIR);
        pc(ok, "TC-POSIX-012", "openat(AT_FDCWD) 可用; 真 dirfd ⇒ ENOTSUP; O_DIRECTORY 用错 ⇒ ENOTDIR");
    }

    /* ---- TC-POSIX-013: fcntl(F_GETFL/F_SETFL/F_DUPFD/记录锁) ---- */
    {
        const int fa = open(p2, O_RDWR);
        const int fb = open(p2, O_RDWR);
        br_bool   ok = (fa >= 0) && (fb >= 0);
        ok = ok && ((fcntl(fa, F_GETFL) & O_ACCMODE) == O_RDWR);
        ok = ok && (fcntl(fa, F_SETFL, (void *)(br_uintptr_t)(O_APPEND | O_NONBLOCK)) == 0);
        ok = ok && ((fcntl(fa, F_GETFL) & O_APPEND) != 0) && ((fcntl(fa, F_GETFL) & O_NONBLOCK) != 0);

        struct flock fl;
        fl.l_type   = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start  = 0;
        fl.l_len    = 0;
        fl.l_pid    = 0;
        ok = ok && (fcntl(fa, F_SETLK, &fl) == 0);

        /* 别的 open file description 看得见这把锁 */
        struct flock q;
        q.l_type   = F_WRLCK;
        q.l_whence = SEEK_SET;
        q.l_start  = 0;
        q.l_len    = 0;
        q.l_pid    = 0;
        ok = ok && (fcntl(fb, F_GETLK, &q) == 0) && (q.l_type == F_WRLCK);

        /* 冲突的写锁 ⇒ EAGAIN */
        struct flock c;
        c.l_type   = F_WRLCK;
        c.l_whence = SEEK_SET;
        c.l_start  = 0;
        c.l_len    = 0;
        c.l_pid    = 0;
        ok = ok && (fcntl(fb, F_SETLK, &c) == -1) && (errno == EAGAIN);

        /* 解锁后不再冲突 */
        fl.l_type = F_UNLCK;
        ok = ok && (fcntl(fa, F_SETLK, &fl) == 0);
        ok = ok && (fcntl(fb, F_GETLK, &q) == 0) && (q.l_type == F_UNLCK);

        ok = ok && (fcntl(fa, F_SETLKW, &fl) == -1) && (errno == ENOTSUP);
        ok = ok && (close(fa) == 0) && (close(fb) == 0);
        pc(ok, "TC-POSIX-013", "F_GETFL/F_SETFL; F_SETLK/F_GETLK 冲突与解锁; F_SETLKW ⇒ ENOTSUP");
    }

    /* ---- TC-POSIX-014: poll / select ---- */
    {
        const int fd = open(p2, O_RDONLY);
        br_bool   ok = (fd >= 0);

        struct pollfd pf;
        pf.fd      = fd;
        pf.events  = POLLIN;
        pf.revents = 0;
        /* 内存文件恒可读(驱动没有 poll 槽位 ⇒ 本服务按"恒就绪"回答) */
        ok = ok && (poll(&pf, 1u, 0) == 1) && ((pf.revents & POLLIN) != 0);

        struct pollfd bad;
        bad.fd      = 99;
        bad.events  = POLLIN;
        bad.revents = 0;
        ok = ok && (poll(&bad, 1u, 0) == 1) && ((bad.revents & POLLNVAL) != 0);

        struct pollfd neg;
        neg.fd      = -1;
        neg.events  = POLLIN;
        neg.revents = 0x7fff;
        ok = ok && (poll(&neg, 1u, 0) == 0) && (neg.revents == 0);      /* 负 fd 被忽略 */

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 0;
        ok = ok && (select(fd + 1, &rd, BR_NULL, BR_NULL, &tv) >= 1) && (FD_ISSET(fd, &rd) != 0ul);
        ok = ok && (close(fd) == 0);
        pc(ok, "TC-POSIX-014", "poll: 就绪计数 / POLLNVAL / 负 fd 忽略; select 复用同一实现");
    }

    /* ---- TC-POSIX-015: 时间(单调可用, 墙钟如实 -ENOTSUP) ---- */
    {
        struct timespec t1;
        struct timespec t2;
        struct timespec res;
        struct timespec req;
        br_bool ok = (clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
        ok = ok && (clock_gettime(CLOCK_MONOTONIC, &t2) == 0) && (t2.tv_sec >= t1.tv_sec);
        ok = ok && (clock_gettime(CLOCK_REALTIME, &t1) == -1) && (errno == ENOTSUP);
        ok = ok && (clock_getres(CLOCK_MONOTONIC, &res) == 0) && (res.tv_nsec == 100000000L);

        const br_time_t before = br_clock_now();
        req.tv_sec  = 0;
        req.tv_nsec = 1000L * 1000L;                       /* 1 ms */
        ok = ok && (nanosleep(&req, BR_NULL) == 0);
        const br_time_t after = br_clock_now();
        ok = ok && (after >= before) && ((after - before) >= 1000u);    /* 不早醒 */
        ok = ok && (usleep(1000u) == 0) && (sleep(0u) == 0u);
        pc(ok, "TC-POSIX-015", "MONOTONIC 单调; REALTIME ⇒ ENOTSUP; getres = 100 ms; nanosleep 不早醒");
    }

    /* ---- TC-POSIX-016: pthread 线程 + retval 搬运 ---- */
    {
        pthread_t t = BR_NULL;
        s_worker_value = 0;
        s_worker_ran   = BR_FALSE;

        br_bool ok = (sem_init(&s_sem_req, 0, 0u) == 0) && (sem_init(&s_sem_done, 0, 0u) == 0);
        ok = ok && (pthread_create(&t, BR_NULL, pt_worker, BR_NULL) == 0);
        ok = ok && (sem_post(&s_sem_req) == 0);
        ok = ok && (sem_wait(&s_sem_done) == 0);

        void *ret = BR_NULL;
        ok = ok && (pthread_join(t, &ret) == 0);
        ok = ok && (ret == (void *)7L);                    /* 入口的返回值经 join 回来 */
        ok = ok && (s_worker_ran == BR_TRUE) && (s_worker_value == 42);
        ok = ok && (pthread_equal(pthread_self(), pthread_self()) != 0);
        ok = ok && (pthread_yield() == 0);
        ok = ok && (pthread_detach(t) == ENOTSUP);         /* 如实不支持 */
        ok = ok && (sem_destroy(&s_sem_req) == 0) && (sem_destroy(&s_sem_done) == 0);
        pc(ok, "TC-POSIX-016", "pthread_create/join 带回 retval; sem 配对唤醒; detach ⇒ ENOTSUP");
    }

    /* ---- TC-POSIX-017: pthread 互斥量 / 条件变量(**错误号通道**: 不设 errno) ---- */
    {
        pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;     /* 静态初始化: 直接用 core 的初值 */
        pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
        br_bool ok = (pthread_mutex_lock(&m) == 0);
        ok = ok && (pthread_mutex_trylock(&m) == EBUSY);   /* 忙 ⇒ **返回** EBUSY(不是 -1) */
        ok = ok && (pthread_mutex_unlock(&m) == 0);
        ok = ok && (pthread_mutex_trylock(&m) == 0) && (pthread_mutex_unlock(&m) == 0);
        ok = ok && (pthread_cond_signal(&c) == 0);         /* 无等待者: no-op */
        ok = ok && (pthread_cond_broadcast(&c) == 0);
        ok = ok && (pthread_mutex_destroy(&m) == 0) && (pthread_cond_destroy(&c) == 0);

        pthread_mutex_t m2;
        ok = ok && (pthread_mutex_init(&m2, BR_NULL) == 0);
        ok = ok && (pthread_mutex_lock(&m2) == 0) && (pthread_mutex_unlock(&m2) == 0);
        ok = ok && (pthread_mutex_destroy(&m2) == 0);
        pc(ok, "TC-POSIX-017", "pthread 错误号通道(trylock ⇒ EBUSY); 静态初始化器可用; destroy ⇒ 0");
    }

    /* ---- TC-POSIX-018: stdio 流层(无 printf: TR-C) ---- */
    {
        pt_path(p3, PT_PATH, "c.txt");
        (void)unlink(p3);
        FILE *f = fopen(p3, "w+");
        br_bool ok = (f != BR_NULL) && (fileno(f) >= 0);
        ok = ok && (fwrite("hello\nworld\n", 1u, 12u, f) == 12u);
        ok = ok && (fflush(f) == 0);
        ok = ok && (fseek(f, 0L, SEEK_SET) == 0);
        ok = ok && (ftell(f) == 0L);
        char line[16];
        ok = ok && (fgets(line, (int)sizeof(line), f) != BR_NULL) && (line[0] == 'h') && (line[5] == '\n');
        ok = ok && (fgets(line, (int)sizeof(line), f) != BR_NULL) && (line[0] == 'w');
        ok = ok && (feof(f) == 0);
        ok = ok && (fgetc(f) == EOF) && (feof(f) != 0);     /* 读到尽头 ⇒ EOF 标志 */
        clearerr(f);
        ok = ok && (feof(f) == 0);
        ok = ok && (fseek(f, 6L, SEEK_SET) == 0);
        ok = ok && (fread(line, 1u, 5u, f) == 5u) && (line[0] == 'w');
        ok = ok && (fclose(f) == 0);

        /* 写→读: 内容真的落到了文件里 */
        char buf[16];
        ok = ok && (pt_read_all(p3, buf, sizeof(buf)) == 12) && (buf[11] == '\n');
        ok = ok && (unlink(p3) == 0);
        pc(ok, "TC-POSIX-018", "fopen/fwrite/fflush/fseek/ftell/fgets/fread/feof/fclose 往返");
    }

    /* ---- TC-POSIX-019: string / stdlib ---- */
    {
        char b[16];
        br_bool ok = (strlen("abc") == 3u) && (strcmp("a", "a") == 0) && (strcmp("a", "b") < 0);
        ok = ok && (strncmp("abcd", "abz", 2u) == 0);
        ok = ok && (strcpy(b, "xy") != BR_NULL) && (b[0] == 'x') && (b[2] == '\0');
        ok = ok && (strcat(b, "z") != BR_NULL) && (strcmp(b, "xyz") == 0);
        ok = ok && (strchr(b, 'y') != BR_NULL) && (strrchr(b, 'z') != BR_NULL);
        ok = ok && (strchr(b, 'q') == BR_NULL);
        ok = ok && (memcmp("ab", "ab", 2u) == 0) && (memchr("abc", 'c', 3u) != BR_NULL);
        ok = ok && (atoi("-123") == -123) && (atoi(" 42xyz") == 42) && (abs(-5) == 5);
        ok = ok && (labs(-7L) == 7L);

        void *p = malloc(64u);
        ok = ok && (p != BR_NULL);
        p = realloc(p, 128u);
        ok = ok && (p != BR_NULL);
        free(p);
        void *z = calloc(4u, 8u);
        ok = ok && (z != BR_NULL) && (((unsigned char *)z)[31] == 0u);
        free(z);
        ok = ok && (getenv("PATH") == BR_NULL);
        pc(ok, "TC-POSIX-019", "str*/mem*/atoi/abs/malloc/calloc/realloc/free 语义");
    }

    /* ---- TC-POSIX-020: errno 值域与 strerror ---- */
    {
        br_bool ok = (ENOENT == 2) && (EBADF == 9) && (EEXIST == 17) && (EINVAL == 22) &&
                     (ENOSPC == 28) && (ENOTEMPTY == 39) && (ELOOP == 40) &&
                     (ENOTSUP == 95) && (ETIMEDOUT == 110) && (EAGAIN == 11);
        ok = ok && (strcmp(strerror(ENOENT), "ENOENT") == 0);
        ok = ok && (strcmp(strerror(EBADF), "EBADF") == 0);
        ok = ok && (strcmp(strerror(9999), "UNKNOWN") == 0);
        /* 健壮性: 空参数/非法 fd 一律给契约错误码, 不崩 */
        ok = ok && (open(BR_NULL, O_RDONLY) == -1) && (errno == EINVAL);
        ok = ok && (read(999, BR_NULL, 1u) == -1) && (errno == EBADF);
        ok = ok && (close(999) == -1) && (errno == EBADF);
        ok = ok && (stat("/posixconf_no_such_file", BR_NULL) == -1);
        ok = ok && (unlink(BR_NULL) == -1);
        pc(ok, "TC-POSIX-020", "errno 编号 = 内核; strerror; 空参/非法 fd 的契约错误码");
    }

    /* 收尾: 清掉自己造的东西(下一个用例从这里开始是干净的) */
    (void)unlink(PT_BASE "/a.txt");
    (void)unlink(PT_BASE "/b.txt");
    (void)unlink(PT_BASE "/c.txt");
    (void)unlink(PT_BASE "/l1");
    (void)unlink(PT_BASE "/h1");
    (void)unlink(PT_BASE "/rel.txt");
    (void)rmdir(PT_BASE);
    (void)chdir("/");

    br_log_info("[POSIXCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
