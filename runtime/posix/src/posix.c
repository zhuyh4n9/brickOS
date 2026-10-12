/*
 * brickOS prototype v0.2.0 — POSIX runtime (runtime/posix plugin)
 *
 * Design basis (design repo `brickOS-Design`):
 *   - `1-01-architecture.md` §7 (v0.5/D18: **POSIX dual-role split** — `runtime/posix` is the
 *     implementation (an ordinary service), `iface/posix` is the thin skin; §7.6 three-way
 *     port dual mode);
 *   - `11-01-service.md` §1/§2 (POSIX runtime service: fd table owner / `errno = -ret`
 *     zero conversion);
 *   - `docs/decisions/0015-runtime-posix-rename.md` (**why this plugin is called runtime/posix**);
 *   - `11-02-svc-posix-subset.md` (**this file's coverage list**: TR-A/TR-B are in scope,
 *     TR-C/TR-D explicitly not — the "honest subset duty"; that doc still uses the design-side
 *     name `runtime/posix`);
 *   - `7-01-vfs.md` §1/§4.1 (fd table entry = `br_file_t*`; `br_open` is the fd table's
 *     underlying primitive);
 *   - `3-01-core-api-list.md` §2/§3/§4/§7 (threads / sync / time / memory — this file only maps).
 * Rulings and deviations: `docs/decisions/0014-svc-posix-posix-runtime.md`.
 *
 * ============================== What this file is ==============================
 * A **translation** layer, not a second kernel. Every POSIX function has one of three shapes:
 *
 *   ① **Zero-conversion delegation**  `read()` → `br_ft_get` + `br_file_read`, `errno = -ret`
 *   ② **Shape conversion**     `struct stat` ← `br_stat_t`; `struct dirent` ← `br_dirent_t`
 *   ③ **This service's own state**  fd table (owner: framework/file-table), **cwd**, errno,
 *                          `FILE*` pool, pthread retval/stack table, record lock table
 *
 * Everything in class ③ is in the TR-B rows of `11-02` — responsibilities the design already
 * assigns to the POSIX runtime (cwd/relative paths, fd-level access-mode enforcement), not
 * extras this implementation added on the side.
 *
 * ============================== Three load-bearing rules ==============================
 * 1. **`errno = -ret` (zero conversion)**: core and vfs error codes **are** the kernel numbers
 *    (ADR-0012, checked code by code), so this file has no errno conversion table — only `-rc`.
 *    A dividend of SD-10 and D18.
 * 2. **The fd table is not here**: its sole owner is `framework/file-table` (ADR-0012). This
 *    file only consumes it ⇒ "single owner of shared state" is greppable (`br_ft_alloc` appears
 *    only in this file).
 * 3. **Access-mode enforcement belongs here**: vfs **deliberately does not enforce** it
 *    (ADR-0009 §5 leftover item 5), so "write on a read-only fd ⇒ EBADF" must live here, else
 *    the whole POSIX surface has no permission semantics.
 *
 * ============================== Known debt (honest list) ==============================
 * All items are in `docs/decisions/0014-…` and `0019-…` §4. The four most important:
 *   - `errno` is now **per-thread** (a slot in this layer's thread record, `__br_posix_errno()`);
 *   - `struct stat`'s `st_ino/st_uid/st_gid/st_*time` are **always 0** (vfs has no such surfaces);
 *   - the `printf` family is **not in this generation** (TR-C: libc choice not settled, `11-01` §3);
 *   - `pthread_detach` = **lazy reclamation** (the next pthread call joins already-exited
 *     detached threads; core has only join as a reclamation path, ADR-0019).
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
#include <sys/mount.h>
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

/* Lifecycle hooks (name = symbol_prefix(short = "posix") + phase; referenced by the build/gen descriptor). */
int posix_early_init(void);
int posix_init(void);
int posix_start(void);

/* ==================================================================== Static bounds */

#define SVC_PATH_MAX        128u   /* same order as the vfs path buffer; overflow ⇒ -ENAMETOOLONG */
#define SVC_FILE_MAX        8u     /* bound on simultaneously open FILE* */
#define SVC_THREAD_MAX      8u     /* aligned with core's BR_TASK_MAX (core is the real TCB pool bound) */
#define SVC_LOCK_MAX        16u    /* record lock entry bound */
#define SVC_SELECT_MAX      16u    /* max fds one select can manage (nfds bound) */
#define SVC_KEY_MAX         PTHREAD_KEYS_MAX   /* TLS key slots (matches pthread.h) */

/* `errno` is now **not** a global int: it is a macro in `errno.h` pointing at a per-thread
 * slot (`__br_posix_errno()`, implemented in the pthread layer below). A global int would
 * revive "thread A's failure read by thread B" and conflicts with POSIX making errno a macro. */

/* cwd: a POSIX concept owned by this service (vfs knows only absolute paths, `7-01` §2). Always starts with '/'. */
static char s_cwd[SVC_PATH_MAX] = "/";
static mode_t s_umask = 0u;                 /* recorded only (not enforced), see umask() */

/* ==================================================================== Helpers */

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

/* Single failure path: `rc` is core/vfs's **negative errno**; negating it is errno (zero conversion). */
static int svc_fail(int rc)
{
    errno = (rc < 0) ? -rc : rc;
    return -1;
}

/*
 * ★ Two error channels, do not mix (POSIX is inconsistent with itself; copy it):
 *   - **syscall family** (open/read/write/…) returns `-1` and sets `errno`  ⇒ `svc_fail`
 *   - **pthread_* family** returns the **error number directly** (never touches errno) ⇒ `svc_rc`
 *   The wrong channel is subtly broken: for `if (pthread_mutex_lock(m) != 0)`, returning -1
 *   makes the caller look up error number -1 — never found — and nobody reads errno either.
 */
static int svc_rc(int rc)
{
    return (rc < 0) ? -rc : rc;
}

/* ==================================================================== Path handling
 *
 * Two paths:
 *   - `svc_abs()`: relative → absolute (**without** resolving "."/".."; that is FS lookup
 *     semantics, see the path_norm note in br_vfs.c). Used for vfs.
 *   - `svc_clean()`: for `chdir` only — folds "."/".." at the **string level**, so `getcwd`
 *     reports a clean path (else you get "legal but ugly" cwds like "/a/../..").
 */

static int svc_abs(const char *path, char *out, size_t cap)
{
    if (path == BR_NULL || path[0] == '\0') {
        return BR_ERR(EINVAL);
    }
    if (path[0] == '/') {
        if (svc_strlen(path) + 1u > cap) {
            return BR_ERR(ENAMETOOLONG);
        }
        svc_copy(out, path, cap);
        return BR_OK;
    }

    const size_t cl = svc_strlen(s_cwd);
    const size_t pl = svc_strlen(path);
    const int    need_slash = (cl > 0u && s_cwd[cl - 1u] != '/') ? 1 : 0;
    if ((cl + (size_t)need_slash + pl + 1u) > cap) {
        return BR_ERR(ENAMETOOLONG);
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

/* String-level fold of "." / ".." (for `chdir`; above root stays root). */
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
                n--;                        /* drop the separating '/' */
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
            out[n++] = '/';                 /* separator; a trailing one is trimmed below */
        }
    }
    while (n > 1u && out[n - 1u] == '/') {
        n--;
    }
    out[n] = '\0';
}

/* ==================================================================== Flag translation
 *
 * POSIX `O_*` (Linux values) → native `BR_O_*`. The **values differ**, so this layer is
 * required (see the fcntl.h note). Unknown bits set are **not an error**: POSIX allows
 * ignoring them (`O_NOCTTY`/`O_LARGEFILE`/`O_CLOEXEC` have no semantics in this generation).
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

/* ==================================================================== fd assertions
 *
 * ★ Access-mode enforcement (**this file's job**; vfs deliberately does not — ADR-0009 §5 leftover 5).
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

/* ==================================================================== Layer: file I/O */

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
        return svc_fail(err);                       /* err is already a negative errno */
    }

    /* `O_DIRECTORY`: opened but not a directory ⇒ close and report -ENOTDIR (as Linux). */
    if (((flags & O_DIRECTORY) != 0) &&
        (br_file_inode(f) != BR_NULL) && (br_file_inode(f)->type != BR_INODE_DIR)) {
        (void)br_file_close(f);
        return svc_fail(BR_ERR(ENOTDIR));
    }

    const int fd = br_ft_alloc(f, (br_u32)flags);
    if (fd < 0) {
        (void)br_file_close(f);                     /* on allocation failure **roll back** the opened file */
        return svc_fail(fd);
    }
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    /* Only `AT_FDCWD` is supported: resolving relative to a **directory fd** needs an fd→path
     * reverse lookup, and vfs handles carry no path (they may have been renamed/unlinked).
     * Report -ENOTSUP honestly rather than pretending support. */
    if (dirfd != AT_FDCWD) {
        return svc_fail(BR_ERR(ENOTSUP));
    }
    return open(path, flags);
}

int creat(const char *path, mode_t mode)
{
    (void)mode;                                     /* no permission model (see chmod) */
    return open(path, O_WRONLY | O_CREAT | O_TRUNC);
}

ssize_t read(int fd, void *buf, size_t n)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL || svc_fd_readable(fd) == 0) {
        return (ssize_t)svc_fail(BR_ERR(EBADF));
    }
    const br_s64 r = br_file_read(f, buf, n);
    if (r < 0) {
        return (ssize_t)svc_fail((int)r);
    }
    return (ssize_t)r;
}

/*
 * `write`'s two extra duties (both belong to the fd layer):
 *   ① access-mode enforcement (read-only fd ⇒ EBADF);
 *   ② **`O_APPEND` fallback**: O_APPEND set after `F_SETFL` never reaches `br_file_t` flags
 *      (those are fixed at open), so this layer seeks to the end before writing. Regular files
 *      only — devices (e.g. uart) are not seekable and O_APPEND is meaningless for them.
 */
static ssize_t svc_write_common(int fd, const void *buf, size_t n, off_t off, int have_off)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL || svc_fd_writable(fd) == 0) {
        return (ssize_t)svc_fail(BR_ERR(EBADF));
    }

    br_u32 fl = 0u;
    (void)br_ft_get_flags(fd, &fl);

    br_s64 saved = br_file_offset(f);
    int    moved = 0;

    if (have_off != 0) {
        if (br_file_lseek(f, (br_s64)off, BR_SEEK_SET) < 0) {
            return (ssize_t)svc_fail(BR_ERR(ESPIPE));
        }
        moved = 1;
    } else if (((fl & (br_u32)O_APPEND) != 0u) &&
               (br_file_inode(f) != BR_NULL) && (br_file_inode(f)->type == BR_INODE_FILE)) {
        if (br_file_lseek(f, 0, BR_SEEK_END) < 0) {
            return (ssize_t)svc_fail(BR_ERR(ESPIPE));
        }
    }

    const br_s64 r = br_file_write(f, buf, n);

    if (moved != 0) {
        br_file_set_offset(f, saved);               /* pread/pwrite do not move the offset */
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
 * `pread`/`pwrite`: vfs has no such slots (`11-02` Q-6); this generation emulates them with
 * "save offset + lseek + read/write + restore offset". ★ **Not atomic**: two threads calling
 * pread on the same fd clobber each other's offset. Acceptable with a single APP, but it is a
 * **known semantic boundary** recorded in ADR-0014 §4.
 */
ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL || svc_fd_readable(fd) == 0) {
        return (ssize_t)svc_fail(BR_ERR(EBADF));
    }
    const br_s64 saved = br_file_offset(f);
    if (br_file_lseek(f, (br_s64)off, BR_SEEK_SET) < 0) {
        return (ssize_t)svc_fail(BR_ERR(ESPIPE));
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
    /* ★ The file is really closed only when the **last reference** goes away — the anchor of
     *   `dup` semantics (two fds share one open file description; closing one spares the other). */
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
        return (off_t)svc_fail(BR_ERR(EBADF));
    }
    /* BR_SEEK_SET/CUR/END share POSIX SEEK_SET/CUR/END values (0/1/2) — pass through. */
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
    /* A displaced handle with no other reference must be closed here — by contract the table
     * leaves that decision to the caller. */
    if (displaced != BR_NULL) {
        (void)br_file_close(displaced);
    }
    return newfd;
}

int fsync(int fd)
{
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL) {
        return svc_fail(BR_ERR(EBADF));
    }
    const int r = br_file_fsync(f);
    /* vfs's fsync slot is BR_NULL for tmpfs ⇒ -ENOTSUP. POSIX allows fsync to return
     * EINVAL/ENOTSUP for objects that do not support it — pass it up honestly, **never** fake
     * success (faking removes any basis for judging "the data is on disk"). */
    return (r != 0) ? svc_fail(r) : 0;
}

int fdatasync(int fd)
{
    return fsync(fd);
}

int ftruncate(int fd, off_t len)
{
    if (len < 0) {
        return svc_fail(BR_ERR(EINVAL));
    }
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL) {
        return svc_fail(BR_ERR(EBADF));
    }
    if (svc_fd_writable(fd) == 0) {
        return svc_fail(BR_ERR(EINVAL));         /* Linux: ftruncate on a read-only fd ⇒ EINVAL */
    }
    br_inode_t *ino = br_file_inode(f);
    if (ino == BR_NULL || ino->iops == BR_NULL || ino->iops->setattr == BR_NULL) {
        return svc_fail(BR_ERR(ENOTSUP));
    }
    const br_stat_t st = { .valid = BR_STAT_SIZE, .type = ino->type,
                           .size = (br_u64)len, .nlink = 0u };
    const int rc = ino->iops->setattr(ino, &st);
    return (rc != 0) ? svc_fail(rc) : 0;
}

int truncate(const char *path, off_t len)
{
    if (len < 0) {
        return svc_fail(BR_ERR(EINVAL));
    }
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_truncate(abs, (br_u64)len);
    return (rc != 0) ? svc_fail(rc) : 0;
}

/* No tty layer in this generation (`br-wa-io-001`) ⇒ honestly answer "not a tty" and set ENOTTY per POSIX. */
int isatty(int fd)
{
    if (svc_fd_file(fd) == BR_NULL) {
        (void)svc_fail(BR_ERR(EBADF));
        return 0;                                   /* errno = EBADF, left as set */
    }
    errno = ENOTTY;                                 /* valid fd but not a tty (no tty layer) */
    return 0;
}

/* ---- Scatter/gather (TR-B: loop over read/write; partial-completion semantics per POSIX) ---- */
ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    if (iov == BR_NULL || iovcnt <= 0) {
        return (ssize_t)svc_fail(BR_ERR(EINVAL));
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0u) {
            continue;
        }
        const ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (r < 0) {
            return (total > 0) ? total : r;         /* some bytes read ⇒ return the short count */
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) {
            break;                                  /* short read: stop (same semantics as read) */
        }
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (iov == BR_NULL || iovcnt <= 0) {
        return (ssize_t)svc_fail(BR_ERR(EINVAL));
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

/* ==================================================================== Layer: record locks
 *
 * `fcntl(F_SETLK/F_GETLK)`. The design ruled: **with a single APP this degenerates to
 * in-process mutual exclusion** (`1-01` §7.6). Semantic boundaries (recorded in ADR-0014 §4):
 *   - locks are keyed by `br_file_t*` (the open file description) — fds from `dup` share a lock;
 *   - `F_SETLKW` (blocking acquire) is **not implemented** ⇒ -ENOTSUP. Blocking needs a wait
 *     queue plus wakeups, and in-process locks have no answer for "whom to wake" (no lock
 *     manager thread); pretending to block would hang the caller.
 */
typedef struct {
    br_u32     used;
    br_file_t *f;
    int        type;      /* F_RDLCK / F_WRLCK */
    off_t      start;
    off_t      len;       /* 0 = to end of file */
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
        return svc_fail(BR_ERR(EBADF));
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

    /* Conflict check: **another** open file description holds an overlapping write lock
     * (or this request is a write lock) */
    for (br_u32 i = 0u; i < SVC_LOCK_MAX; i++) {
        if (s_locks[i].used == 0u || s_locks[i].f == f) {
            continue;
        }
        const off_t os = s_locks[i].start;
        const off_t oe = (s_locks[i].len == 0) ? (off_t)0x7fffffffffffffffL
                                               : (s_locks[i].start + s_locks[i].len);
        if (svc_overlap(s, e, os, oe) != 0 &&
            (s_locks[i].type == F_WRLCK || fl->l_type == F_WRLCK)) {
            return svc_fail(BR_ERR(EAGAIN));     /* POSIX allows EACCES/EAGAIN */
        }
    }

    /* Record an entry (replace an old entry with the same f and range, else take a free slot) */
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
        return svc_fail(BR_ERR(ENOLCK));         /* lock table full */
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
        return svc_fail(BR_ERR(EBADF));
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
    fl->l_type = F_UNLCK;                           /* "no conflict" */
    return 0;
}

int fcntl(int fd, int cmd, ...)
{
    void *arg = BR_NULL;

    /* ★ Fetch the vararg only for cmds that **need an argument**: reading it for no-arg
     *   commands like F_GETFD is UB (the caller pushed none; you may read register garbage). */
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
        return svc_fail(BR_ERR(EBADF));
    }

    switch (cmd) {
        case F_GETFD:
            return 0;                               /* no carrier for FD_CLOEXEC (no exec) */
        case F_SETFD:
            return 0;
        case F_GETFL: {
            br_u32 fl = 0u;
            if (br_ft_get_flags(fd, &fl) != 0) {
                return svc_fail(BR_ERR(EBADF));
            }
            return (int)fl;
        }
        case F_SETFL: {
            /* accept "status bit" changes only; the access mode cannot change (same in POSIX). */
            br_u32 fl = 0u;
            if (br_ft_get_flags(fd, &fl) != 0) {
                return svc_fail(BR_ERR(EBADF));
            }
            const br_u32 want   = (br_u32)(br_uintptr_t)arg;
            const br_u32 change = (br_u32)(O_APPEND | O_NONBLOCK);
            fl = (fl & ~change) | (want & change);
            const int rc = br_ft_set_flags(fd, fl);
            return (rc != 0) ? svc_fail(rc) : 0;
        }
        case F_DUPFD: {
            /* duplicate to the lowest free fd **≥ arg** (POSIX F_DUPFD semantics) */
            const int lo = (int)(br_intptr_t)arg;
            if (lo < 0) {
                return svc_fail(BR_ERR(EINVAL));
            }
            for (int cand = lo; cand < (int)BR_FT_MAX; cand++) {
                if (br_ft_get(cand) == BR_NULL) {
                    br_file_t *disp = BR_NULL;
                    const int  r = br_ft_dup2(fd, cand, &disp);
                    if (r < 0) {
                        return svc_fail(r);
                    }
                    if (disp != BR_NULL) {
                        (void)br_file_close(disp);  /* the slot was free ⇒ unreachable in theory */
                    }
                    return cand;
                }
            }
            return svc_fail(BR_ERR(EMFILE));
        }
        case F_SETLK:
            return svc_lock_set(fd, (struct flock *)arg);
        case F_GETLK:
            return svc_lock_get(fd, (struct flock *)arg);
        case F_SETLKW:
            /* see the file header: blocking record locks need a wait queue; not in this generation. */
            return svc_fail(BR_ERR(ENOTSUP));
        default:
            return svc_fail(BR_ERR(EINVAL));
    }
}

/* ==================================================================== Layer: metadata */

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
    /* st_dev/st_ino/st_uid/st_gid/st_rdev/st_*time are always 0 — vfs has no such surfaces (ADR-0014 §4). */
}

static int svc_stat_common(const char *path, struct stat *st, int nofollow)
{
    if (st == BR_NULL) {
        return svc_fail(BR_ERR(EINVAL));
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
        return svc_fail(BR_ERR(EINVAL));
    }
    br_file_t *f = svc_fd_file(fd);
    if (f == BR_NULL) {
        return svc_fail(BR_ERR(EBADF));
    }
    br_stat_t b;
    const int rc = br_fstat(f, &b);                 /* vfs handle-level primitive (ADR-0013) */
    if (rc != 0) {
        return svc_fail(rc);
    }
    svc_fill_stat(&b, st);
    return 0;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode;                                     /* no permission model (see ADR-0014) */
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_mkdir(abs);
    return (rc != 0) ? svc_fail(rc) : 0;
}

/* ---- Permission surface: per the user ruling = **no-op returning 0** (see ADR-0014 §2) ----
 * Why a no-op is honest rather than lazy: this prototype has **no** permission model (vfs
 * stores no mode/uid/gid and there is no "current user"), so "accept and ignore" lets
 * POSIX-written code run to completion (a chmod 0644 leaves behaviour unaffected), whereas
 * -ENOTSUP would make it fail somewhere unrelated. The real risk is "the caller thinks the
 * permissions took effect" — covered by documentation and explicit ADR statements, not by
 * return codes. */
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
    return old;                                     /* record and return the old value; no enforcement */
}

/*
 * `access`: permission bits **always allowed** (user ruling), but the **existence check stays** —
 * an access that answers "readable" for a missing file would distort the caller's branching
 * completely; that is not a no-op, that is lying. So semantics = "F_OK checked; R/W/X always true".
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

/* ==================================================================== Layer: directories */

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

/* POSIX remove(): files go through unlink, empty directories through rmdir. */
int remove(const char *path)
{
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(path, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    rc = br_unlink(abs);
    if (rc == BR_ERR(EISDIR)) {
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
        return svc_fail(BR_ERR(EINVAL));
    }
    char abs[SVC_PATH_MAX];
    int  rc = svc_abs(linkpath, abs, (size_t)sizeof(abs));
    if (rc != BR_OK) {
        return svc_fail(rc);
    }
    /* The target string is **not** made absolute: a symlink is interpreted relative to the
     * directory holding it (`7-01` and vfs resolve_norm rely on this); pre-joining an absolute
     * path would pin the link to the current cwd. */
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
    return (ssize_t)rc;                             /* full length (may exceed cap, per POSIX) */
}

/* ---- DIR ---- */

struct br_posix_dir {
    br_dir_t      *d;
    struct dirent  ent;
    char           path[SVC_PATH_MAX];   /* rewinddir closes and reopens ⇒ must remember what was opened */
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
        errno = ENOTDIR;                            /* vfs opendir sets no errno; see below */
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
        errno = -rc;                                /* real error: let the caller see it */
        return BR_NULL;
    }
    if (rc == 0) {
        return BR_NULL;                             /* directory exhausted (POSIX: NULL, errno untouched) */
    }
    d->ent.d_ino    = 0;                            /* vfs has no inode number */
    d->ent.d_off    = 0;
    d->ent.d_reclen = (unsigned short)sizeof(struct dirent);
    d->ent.d_type   = (unsigned char)svc_dtype(de.type);
    svc_copy(d->ent.d_name, de.name, (size_t)BR_POSIX_NAME_MAX);
    return &d->ent;
}

int closedir(DIR *d)
{
    if (d == BR_NULL) {
        return svc_fail(BR_ERR(EBADF));
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
    /* vfs's directory iterator has no "reset" slot ⇒ close and reopen (equivalent, costs one
     * open). This is the other side of "seekdir/telldir are TR-C" (see dirent.h); that is why
     * the path is remembered. */
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
        return svc_fail(BR_ERR(ENOTDIR));
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

/* ==================================================================== Layer: time */

static void svc_us_to_ts(br_u64 us, struct timespec *ts)
{
    ts->tv_sec  = (time_t)(us / 1000000u);
    ts->tv_nsec = (long)((us % 1000000u) * 1000u);
}

int clock_gettime(clockid_t id, struct timespec *ts)
{
    if (ts == BR_NULL) {
        return svc_fail(BR_ERR(EINVAL));
    }
    /* ★ MONOTONIC only: there is no wall-clock source (P-1), so REALTIME **honestly** returns
     *   -ENOTSUP instead of inventing a fake "seconds since 1970" (which would silently break
     *   ordering/timeout logic). */
    if (id != CLOCK_MONOTONIC) {
        return svc_fail(BR_ERR(ENOTSUP));
    }
    svc_us_to_ts(br_clock_now(), ts);
    return 0;
}

int clock_getres(clockid_t id, struct timespec *ts)
{
    if (ts == BR_NULL) {
        return svc_fail(BR_ERR(EINVAL));
    }
    if (id != CLOCK_MONOTONIC) {
        return svc_fail(BR_ERR(ENOTSUP));
    }
    ts->tv_sec  = 0;
    ts->tv_nsec = BR_POSIX_CLOCK_RES_NS;            /* honestly report one tick = 1/HZ (periodic tick) */
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req == BR_NULL || req->tv_sec < 0 || req->tv_nsec < 0) {
        return svc_fail(BR_ERR(EINVAL));
    }
    if (rem != BR_NULL) {
        rem->tv_sec  = 0;                           /* no signal interruption here ⇒ no remainder */
        rem->tv_nsec = 0;
    }
    const br_u64 us = ((br_u64)req->tv_sec * 1000000u) + ((br_u64)req->tv_nsec / 1000u);
    const int rc = br_task_sleep((br_time_t)us);    /* guarantees **no early wakeup** (INV-2) */
    return (rc != 0) ? svc_fail(rc) : 0;
}

unsigned int sleep(unsigned int seconds)
{
    (void)br_task_sleep((br_time_t)((br_u64)seconds * 1000000u));
    return 0u;                                      /* no early wakeup ⇒ slept fully, no remainder */
}

int usleep(useconds_t usec)
{
    const int rc = br_task_sleep((br_time_t)usec);
    return (rc != 0) ? svc_fail(rc) : 0;
}

/* ==================================================================== Layer: poll/select
 *
 * v1 semantics = design SD-7's "polling + coarse sleep" (see the poll.h note).
 */

/* One query round: write revents, return the number ready. */
static int svc_poll_once(struct pollfd *fds, nfds_t n, int *bad)
{
    int ready = 0;
    *bad = 0;
    for (nfds_t i = 0u; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) {
            continue;                               /* POSIX: a negative fd is **ignored** (revents = 0) */
        }
        br_file_t *f = svc_fd_file(fds[i].fd);
        if (f == BR_NULL) {
            fds[i].revents = POLLNVAL;              /* POSIX: an invalid fd makes poll return at once */
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
        } else if (rc == BR_ERR(ENOTSUP)) {
            /* No poll slot (e.g. tmpfs in-memory files): POSIX's answer is "always ready" —
             * memory files are always readable/writable, closer to the truth than an error. */
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
        return svc_fail(BR_ERR(EINVAL));
    }
    if (nfds == 0u) {
        /* Pure delay: per POSIX sleep the full timeout, then return 0 */
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
            /* invalid fd takes priority (POSIX requires an immediate return, no waiting) */
            return ready;
        }
        if (timeout_ms == 0) {
            return 0;
        }
        if (deadline != BR_TIMEOUT_INF && br_clock_now() >= deadline) {
            return 0;
        }
        /* Check again after a slice. ★ tickless is not implemented ⇒ real sleep ≈ one tick
         *   (`1/HZ`, 5 ms default), so taking 10 ms as the "poll interval" is a **stated
         *   intent**, not a promise (P-2). */
        (void)br_task_sleep((br_time_t)10000u);
        if (deadline != BR_TIMEOUT_INF && br_clock_now() >= deadline) {
            return 0;
        }
    }
}

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *timeout)
{
    if (nfds < 0 || nfds > (int)SVC_SELECT_MAX) {
        return svc_fail(BR_ERR(EINVAL));         /* out of range **errors** rather than silently truncating */
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

    /* poll → select writeback (all three sets rewritten per the result, POSIX semantics) */
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

/* ==================================================================== Layer: stdio
 *
 * Stream layer only (FILE* + write buffering + positioning); no formatting layer (see stdio.h).
 */

struct br_posix_file {
    br_u32         magic;
    int            fd;
    int            owned;       /* whether fclose() closes fd */
    int            eof;
    int            err;
    int            readable;
    int            writable;
    int            append;      /* O_APPEND ⇒ no write buffering (interleaved writes would misorder) */
    long           bufmode;     /* _IOFBF / _IOLBF / _IONBF */
    unsigned char *wbuf;
    size_t         wcap;
    size_t         wlen;
};

#define SVC_FILE_MAGIC  0x5046494Cu   /* "PFIL" */

static FILE *s_files[SVC_FILE_MAX];

/* Find a free slot; also reclaim zombie slots left by fclose without clearing (their magic is dead) */
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
        return BR_ERR(EINVAL);
    }
    *rd = 0;
    *wr = 0;
    *ap = 0;

    switch (mode[0]) {
        case 'r': *flags = O_RDONLY; *rd = 1; break;
        case 'w': *flags = O_WRONLY | O_CREAT | O_TRUNC; *wr = 1; break;
        case 'a': *flags = O_WRONLY | O_CREAT | O_APPEND; *wr = 1; *ap = 1; break;
        default:  return BR_ERR(EINVAL);
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
            /* 'b' = binary (no distinction); 'e'/'x' not in this generation ⇒ ignore */
        } else {
            return BR_ERR(EINVAL);
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
        return BR_NULL;                             /* errno already set by open */
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
        (void)svc_fail(BR_ERR(EBADF));
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
        return svc_fail(BR_ERR(EBADF));
    }
    if (f->wlen == 0u) {
        return 0;
    }
    size_t done = 0u;
    while (done < f->wlen) {
        const ssize_t r = write(f->fd, f->wbuf + done, f->wlen - done);
        if (r <= 0) {
            f->err = 1;
            return svc_fail(BR_ERR(EIO));
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
        (void)svc_fail(BR_ERR(EBADF));
        return 0u;
    }
    if (size == 0u || n == 0u) {
        return 0u;                                  /* zero-length write: **leaves** errno alone (per POSIX) */
    }
    const size_t total = size * n;
    const size_t done  = svc_file_push(f, (const unsigned char *)buf, total);
    return done / size;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC || f->readable == 0) {
        (void)svc_fail(BR_ERR(EBADF));
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
        return svc_fail(BR_ERR(EBADF));
    }
    if (fflush(f) != 0) {
        return -1;
    }
    const off_t r = lseek(f->fd, (off_t)off, whence);
    if (r < 0) {
        return -1;
    }
    f->eof = 0;                                     /* POSIX: seeking clears the EOF flag */
    return 0;
}

long ftell(FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        (void)svc_fail(BR_ERR(EBADF));
        return -1;
    }
    const off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0) {
        return -1;
    }
    /* ★ Logical position = file position + bytes not yet flushed (the write buffer lags the kernel position) */
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
        return svc_fail(BR_ERR(EBADF));
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
        (void)svc_fail(BR_ERR(EBADF));
        return -1;
    }
    return f->fd;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC) {
        return svc_fail(BR_ERR(EBADF));
    }
    (void)size;
    /* The caller's `buf` is **not adopted**: the caller owns its lifetime, while this
     * implementation must be able to free its own buffer. POSIX lets an implementation ignore
     * this argument (it is only a hint). */
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
        (void)svc_fail(BR_ERR(EBADF));
        return EOF;
    }
    return (svc_file_push(f, &b, 1u) == 1u) ? (int)b : EOF;
}

int fputs(const char *s, FILE *f)
{
    if (f == BR_NULL || f->magic != SVC_FILE_MAGIC || f->writable == 0) {
        (void)svc_fail(BR_ERR(EBADF));
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
        (void)svc_fail(BR_ERR(EBADF));
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

/* Standard stream symbols: NULL by default (POSIX's 0/1/2 belong to a process model, absent
 * here). `posix_init` would attach them to fd 0/1/2 — only then are they real streams. */
FILE *stdin  = BR_NULL;
FILE *stdout = BR_NULL;
FILE *stderr = BR_NULL;

/* ==================================================================== Layer: string
 *
 * `mem*` is **not here** (those are core symbols, see the string.h note); `str*` is defined here.
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
        dst[i] = '\0';                              /* POSIX: pad with '\0' up to n */
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

/* errno → short name. Returns a **static string**; the caller must not free it (as in POSIX). */
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

/* ==================================================================== Layer: stdlib */

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

/* Minimal integer parsing (the cheapest entry into TR-C's "write it yourself" path; a full
 * strtol is still pending). Degraded POSIX semantics: skip whitespace, optional sign, digits;
 * no digits ⇒ 0; overflow is **not reported** (saturates). */
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
            break;                                  /* saturate, do not wrap */
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
    return BR_NULL;                                 /* no environment model in this generation (answer honestly) */
}

/* ==================================================================== Layer: pthread */

/*
 * Per-thread record. One record carries four things at once (all of which POSIX requires to be
 * per-thread):
 *   - **lifetime**: the core TCB mapping + `join`'s retval + ownership of the self-cut stack
 *     (file header ①②);
 *   - **detach flag**: the criterion for lazy reclamation (file header ③);
 *   - **errno slot**: the `errno` macro points here (file header ④);
 *   - **TLS key slots**: `pthread_setspecific/getspecific` land here (file header ⑤).
 *
 * The main thread (and any thread not created by `pthread_create`) lazily registers a record
 * on **first need** — so per-thread errno/TLS also holds for the APP main thread without a
 * generic core slot.
 */
typedef struct {
    br_u32        used;
    br_thread_t  *t;
    void        *(*fn)(void *);
    void         *arg;
    void         *retval;
    void         *stack;        /* stack cut by this service (returned at join/reap) */
    int           owns_stack;
    int           detached;     /* PTHREAD_CREATE_DETACHED / pthread_detach() */
    int           err_val;      /* per-thread errno */
    char          name[BR_PTHREAD_NAME_MAX];
    void         *key_values[SVC_KEY_MAX];
} svc_thread_t;

static svc_thread_t s_threads[SVC_THREAD_MAX];

/* key registry: used bitmap + destructors. Keys are process-unique (not per-thread). */
static br_u32 s_key_used;
static void (*s_key_dtor[SVC_KEY_MAX])(void *);

/* Fallback slot for extreme cases (errno outside a kernel thread / record table full): keeps `errno` always a writable lvalue. */
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

/* The current thread's record: if not found, **lazily register** one (main and non-pthread
 * threads take this path). `br_task_self()` is unavailable very early ⇒ return NULL and the
 * caller falls back to the fallback slot. */
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

/* Lazy reclamation: join threads that exited (ZOMBIE) and are detached, then return their
 * stacks and clear records. Called on **every entry into the pthread layer** (see pthread.h
 * header ③). Skips the current thread. */
static void svc_reap_detached(void)
{
    br_thread_t *self = br_task_self();
    for (br_u32 i = 0u; i < (br_u32)SVC_THREAD_MAX; i++) {
        svc_thread_t *rec = &s_threads[i];
        if (rec->used == 0u || rec->detached == 0 || rec->t == BR_NULL || rec->t == self) {
            continue;
        }
        if (br_task_state(rec->t) != BR_TASK_ZOMBIE) {
            continue;                               /* still running ⇒ next time */
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

/* On thread exit, run TLS destructors per POSIX (at most PTHREAD_DESTRUCTOR_ITERATIONS
 * rounds, since a destructor may call setspecific again). */
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

/* Entry trampoline: core's `br_task_create` passes only one `void*`, so function + argument +
 * retval landing spot all hang off the record. Moving the retval is the **only** shape
 * difference between pthread and core (see pthread.h ②). */
static void svc_thread_tramp(void *arg)
{
    svc_thread_t *rec = (svc_thread_t *)arg;
    rec->retval = rec->fn(rec->arg);
    svc_thread_run_key_dtors();                     /* POSIX: run TLS destructors on thread exit */
    br_task_exit(0);
}

/* timespec (absolute, MONOTONIC) → relative microseconds. A past time ⇒ ZERO (trylock semantics). */
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

/* ==================================================================== Lifecycle */

int pthread_attr_init(pthread_attr_t *a)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    a->stack_size = size;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size)
{
    if (a == BR_NULL || size == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *size = a->stack_size;
    return 0;
}

int pthread_attr_setstack(pthread_attr_t *a, void *stack, size_t size)
{
    if (a == BR_NULL || stack == BR_NULL || size < BR_STACK_MIN) {
        return svc_rc(BR_ERR(EINVAL));
    }
    a->stack      = stack;
    a->stack_size = size;
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **stack, size_t *size)
{
    if (a == BR_NULL || stack == BR_NULL || size == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *stack = a->stack;
    *size  = a->stack_size;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
    if (a == BR_NULL ||
        (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED)) {
        return svc_rc(BR_ERR(EINVAL));
    }
    a->detachstate = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
    if (a == BR_NULL || state == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *state = a->detachstate;
    return 0;
}

int pthread_attr_setguardsize(pthread_attr_t *a, size_t size)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    /* core has no guard pages (stacks are caller-provided contiguous memory) ⇒ record only,
     * no behaviour change. Honestly returning success lets get read back what was set; making
     * the caller know it "has no effect" is left to the docs and this comment. */
    a->guard_size = size;
    return 0;
}

int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size)
{
    if (a == BR_NULL || size == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *size = a->guard_size;
    return 0;
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg)
{
    svc_reap_detached();
    if (out == BR_NULL || fn == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }

    svc_thread_t *rec = BR_NULL;
    for (br_u32 i = 0u; i < (br_u32)SVC_THREAD_MAX; i++) {
        if (s_threads[i].used == 0u) {
            rec = &s_threads[i];
            break;
        }
    }
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(EAGAIN));         /* thread table full (same order as core's TCB pool) */
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
        /* ★ cut the stack from the core heap (POSIX's signature has no stack; see pthread.h ①) */
        stack = br_malloc((br_size_t)ssize);
        if (stack == BR_NULL) {
            return svc_rc(BR_ERR(EAGAIN));
        }
        owned = 1;
    }

    br_task_attr_t ta;
    ta.name       = "pthread";
    ta.stack      = (void *)((unsigned char *)stack + ssize);   /* stack top = high address end */
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
        return svc_rc(BR_ERR(ESRCH));
    }
    if (t == br_task_self()) {
        return svc_rc(BR_ERR(EDEADLK));
    }
    svc_thread_t *rec = svc_thread_find(t);
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(ESRCH));          /* never created / already joined */
    }
    if (rec->detached != 0) {
        return svc_rc(BR_ERR(EINVAL));         /* joining a detached thread is undefined (glibc: EINVAL) */
    }
    int       code = 0;
    const int rc   = br_task_join(t, &code);
    if (rc != 0) {
        return svc_rc(rc);
    }
    if (retval != BR_NULL) {
        *retval = rec->retval;                      /* core returns only int; retval lives in the record */
    }
    if (rec->owns_stack != 0 && rec->stack != BR_NULL) {
        br_free(rec->stack);                        /* whoever cut the stack returns it */
    }
    svc_thread_clear(rec);
    return 0;
}

void pthread_exit(void *retval)
{
    svc_thread_t *rec = svc_thread_find(br_task_self());
    if (rec != BR_NULL) {
        rec->retval = retval;                       /* so join can still retrieve it */
    }
    svc_thread_run_key_dtors();
    br_task_exit(0);
}

int pthread_detach(pthread_t t)
{
    svc_reap_detached();
    if (t == BR_NULL) {
        return svc_rc(BR_ERR(ESRCH));
    }
    svc_thread_t *rec = svc_thread_find(t);
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(ESRCH));
    }
    if (rec->detached != 0) {
        return svc_rc(BR_ERR(EINVAL));
    }
    rec->detached = 1;
    return 0;                                       /* reclamation is lazy (see file header ③) */
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
        return svc_rc(BR_ERR(ESRCH));
    }
    svc_copy(rec->name, name, sizeof(rec->name));
    return 0;
}

int pthread_getname_np(pthread_t t, char *buf, size_t len)
{
    svc_reap_detached();
    if (buf == BR_NULL || len == 0u) {
        return svc_rc(BR_ERR(EINVAL));
    }
    if (t == BR_NULL) {
        t = br_task_self();
    }
    svc_thread_t *rec = (t == br_task_self()) ? svc_thread_self_rec()
                                              : svc_thread_find(t);
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(ESRCH));
    }
    svc_copy(buf, rec->name, len);
    return 0;
}

/* ==================================================================== once */

int pthread_once(pthread_once_t *once, void (*init)(void))
{
    svc_reap_detached();
    if (once == BR_NULL || init == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    if (once->done != 0) {
        return 0;                                   /* lock-free fast path (read-only once done) */
    }
    const int rc = br_mutex_lock(&once->m);
    if (rc != 0) {
        return svc_rc(rc);
    }
    if (once->done == 0) {
        init();                                     /* POSIX: init must not recurse into the same once */
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
        return svc_rc(BR_ERR(EINVAL));
    }
    for (br_u32 i = 0u; i < (br_u32)SVC_KEY_MAX; i++) {
        if ((s_key_used & (1u << i)) == 0u) {
            s_key_used |= (1u << i);
            s_key_dtor[i] = destructor;
            *key = i;
            return 0;
        }
    }
    return svc_rc(BR_ERR(EAGAIN));               /* keys exhausted */
}

int pthread_key_delete(pthread_key_t key)
{
    svc_reap_detached();
    if (key >= (pthread_key_t)SVC_KEY_MAX || (s_key_used & (1u << key)) == 0u) {
        return svc_rc(BR_ERR(EINVAL));
    }
    s_key_used &= ~(1u << key);
    s_key_dtor[key] = BR_NULL;
    return 0;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    svc_reap_detached();
    if (key >= (pthread_key_t)SVC_KEY_MAX || (s_key_used & (1u << key)) == 0u) {
        return svc_rc(BR_ERR(EINVAL));
    }
    svc_thread_t *rec = svc_thread_self_rec();
    if (rec == BR_NULL) {
        return svc_rc(BR_ERR(EAGAIN));
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
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    a->type = (unsigned)type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
    if (a == BR_NULL || type == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *type = (int)a->type;
    return 0;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int pshared)
{
    if (a == BR_NULL ||
        (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)) {
        return svc_rc(BR_ERR(EINVAL));
    }
    /* Single address space: PROCESS_SHARED is recorded but has no cross-process semantics (honest). */
    a->pshared = pshared;
    return 0;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *pshared)
{
    if (a == BR_NULL || pshared == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *pshared = a->pshared;
    return 0;
}

int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int protocol)
{
    if (a == BR_NULL || protocol != PTHREAD_PRIO_NONE) {
        return svc_rc(BR_ERR(ENOTSUP));          /* PI is v2 (no priorities) */
    }
    a->protocol = protocol;
    return 0;
}

int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *a, int *protocol)
{
    if (a == BR_NULL || protocol == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *protocol = a->protocol;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
    svc_reap_detached();
    if (m == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
    return 0;                                       /* no-op ⇒ 0 (see the pthread.h note) */
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    svc_reap_detached();
    if (m == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    br_thread_t *self = br_task_self();

    /* RECURSIVE: already held ⇒ just bump the count (core's mutex is a plain mutex; relocking it would self-deadlock). */
    if (m->owner == self && m->count > 0u) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            m->count++;
            return 0;
        }
        if (m->type == PTHREAD_MUTEX_ERRORCHECK) {
            return svc_rc(BR_ERR(EDEADLK));
        }
        /* NORMAL: POSIX says self-lock is undefined ⇒ fall through to core (which self-deadlocks, as Linux NORMAL) */
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
        return svc_rc(BR_ERR(EINVAL));
    }
    br_thread_t *self = br_task_self();
    if (m->owner == self && m->count > 0u) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            m->count++;
            return 0;
        }
        return svc_rc(BR_ERR(EBUSY));            /* NORMAL/ERRORCHECK: self-held ⇒ EBUSY */
    }
    /* core's `lock_to(ZERO)` = trylock: busy ⇒ **-ETIMEDOUT** (INV-1's unified timeout code).
     * POSIX trylock wants EBUSY — this **must** be translated, else a caller's EBUSY branch
     * never runs (one of the very few "non-zero conversion" points in this file). */
    const int rc = br_mutex_lock_to(&m->m, BR_TIMEOUT_ZERO);
    if (rc == BR_ERR(ETIMEDOUT)) {
        return svc_rc(BR_ERR(EBUSY));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    br_thread_t *self = br_task_self();
    if (m->owner == self && m->count > 0u) {
        if (m->type == PTHREAD_MUTEX_RECURSIVE) {
            m->count++;
            return 0;
        }
        if (m->type == PTHREAD_MUTEX_ERRORCHECK) {
            return svc_rc(BR_ERR(EDEADLK));
        }
    }
    const int rc = br_mutex_lock_to(&m->m, svc_abstime_to_rel(abstime));
    if (rc == BR_ERR(ETIMEDOUT)) {
        return svc_rc(BR_ERR(ETIMEDOUT));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    br_thread_t *self = br_task_self();
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != self) {
            return svc_rc(BR_ERR(EPERM));        /* ERRORCHECK/RECURSIVE: unlock by a non-owner */
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
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    /* This generation has only the monotonic clock (P-1: no wall clock) ⇒ REALTIME is explicitly rejected, not faked. */
    if (clock != CLOCK_MONOTONIC) {
        return svc_rc(BR_ERR(EINVAL));
    }
    a->clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *a, int *clock)
{
    if (a == BR_NULL || clock == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *clock = a->clock;
    return 0;
}

int pthread_condattr_setpshared(pthread_condattr_t *a, int pshared)
{
    if (a == BR_NULL ||
        (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)) {
        return svc_rc(BR_ERR(EINVAL));
    }
    a->pshared = pshared;
    return 0;
}

int pthread_condattr_getpshared(const pthread_condattr_t *a, int *pshared)
{
    if (a == BR_NULL || pshared == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *pshared = a->pshared;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
    svc_reap_detached();
    if (c == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    const int rc = br_cond_wait(&c->c, &m->m, BR_TIMEOUT_INF);
    return (rc != 0) ? svc_rc(rc) : 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime)
{
    svc_reap_detached();
    if (c == BR_NULL || m == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    /* ★ Absolute times are interpreted as **MONOTONIC**: this generation has no wall clock
     *   (P-1), so the monotonic clock is the only usable base for "absolute time". POSIX
     *   defaults to CLOCK_REALTIME ⇒ a **semantic deviation**, the other face of
     *   `clock_gettime(CLOCK_REALTIME) = -ENOTSUP`; `pthread_condattr_setclock(CLOCK_MONOTONIC)`
     *   writes the intent into the declaration. */
    const int rc = br_cond_wait(&c->c, &m->m, svc_abstime_to_rel(abstime));
    if (rc == BR_ERR(ETIMEDOUT)) {
        return svc_rc(BR_ERR(ETIMEDOUT));      /* POSIX's ETIMEDOUT */
    }
    return (rc != 0) ? svc_rc(rc) : 0;
}

int pthread_cond_signal(pthread_cond_t *c)
{
    if (c == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    const int rc = br_cond_signal(&c->c);
    return (rc != 0) ? svc_rc(rc) : 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    if (c == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    const int rc = br_cond_broadcast(&c->c);
    return (rc != 0) ? svc_rc(rc) : 0;
}

/* ==================================================================== rwlock */

int pthread_rwlockattr_init(pthread_rwlockattr_t *a)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    a->kind = kind;
    return 0;
}

int pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *a, int *kind)
{
    if (a == BR_NULL || kind == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *kind = a->kind;
    return 0;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *a, int pshared)
{
    if (a == BR_NULL ||
        (pshared != PTHREAD_PROCESS_PRIVATE && pshared != PTHREAD_PROCESS_SHARED)) {
        return svc_rc(BR_ERR(EINVAL));
    }
    a->pshared = pshared;
    return 0;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *a, int *pshared)
{
    if (a == BR_NULL || pshared == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *pshared = a->pshared;
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *attr)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
    return 0;                                       /* no-op (same reason as mutex/cond) */
}

/* Shared read-lock implementation: `timeout` == BR_TIMEOUT_ZERO makes it a trylock. */
static int svc_rwlock_rdlock_to(pthread_rwlock_t *rw, br_time_t timeout)
{
    const int lk = br_mutex_lock(&rw->m);
    if (lk != 0) {
        return svc_rc(lk);
    }
    for (;;) {
        const br_bool writer = (rw->writer != BR_NULL);
        /* writer-preference: new readers yield while a writer waits (avoids writer starvation).
         * ★ Unsupported boundary: **recursive reads from one thread** self-block while a writer
         *   waits (recorded honestly, see pthread.h). */
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
            return svc_rc(BR_ERR(EBUSY));
        }
        const int w = br_cond_wait(&rw->c, &rw->m, timeout);
        if (w == BR_ERR(ETIMEDOUT)) {
            (void)br_mutex_unlock(&rw->m);
            return svc_rc(BR_ERR(ETIMEDOUT));
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
        return svc_rc(BR_ERR(EBUSY));
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
        if (w == BR_ERR(ETIMEDOUT)) {
            rw->writers_waiting--;
            (void)br_cond_broadcast(&rw->c);        /* may have freed a reader slot */
            (void)br_mutex_unlock(&rw->m);
            return svc_rc(BR_ERR(ETIMEDOUT));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    return svc_rwlock_rdlock_to(rw, BR_TIMEOUT_INF);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    return svc_rwlock_rdlock_to(rw, BR_TIMEOUT_ZERO);
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *rw, const struct timespec *abstime)
{
    svc_reap_detached();
    if (rw == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    return svc_rwlock_rdlock_to(rw, svc_abstime_to_rel(abstime));
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    return svc_rwlock_wrlock_to(rw, BR_TIMEOUT_INF);
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    return svc_rwlock_wrlock_to(rw, BR_TIMEOUT_ZERO);
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *rw, const struct timespec *abstime)
{
    svc_reap_detached();
    if (rw == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    return svc_rwlock_wrlock_to(rw, svc_abstime_to_rel(abstime));
}

int pthread_rwlock_unlock(pthread_rwlock_t *rw)
{
    svc_reap_detached();
    if (rw == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
            (void)br_cond_broadcast(&rw->c);        /* last reader ⇒ wake writers */
        }
    } else {
        (void)br_mutex_unlock(&rw->m);
        return svc_rc(BR_ERR(EPERM));            /* unlocking without holding the lock */
    }
    (void)br_mutex_unlock(&rw->m);
    return 0;
}

/* ==================================================================== barrier */

int pthread_barrierattr_init(pthread_barrierattr_t *a)
{
    if (a == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    a->pshared = pshared;
    return 0;
}

int pthread_barrierattr_getpshared(const pthread_barrierattr_t *a, int *pshared)
{
    if (a == BR_NULL || pshared == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
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
        return PTHREAD_BARRIER_SERIAL_THREAD;       /* exactly one thread gets it */
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

/* ==================================================================== Layer: semaphore */

int sem_init(sem_t *s, int pshared, unsigned value)
{
    if (s == BR_NULL || value > BR_POSIX_SEM_VALUE_MAX) {
        return svc_rc(BR_ERR(EINVAL));
    }
    (void)pshared;                                  /* single address space ⇒ pshared is meaningless */
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
        return svc_rc(BR_ERR(EINVAL));
    }
    const int rc = br_sem_take(s, BR_TIMEOUT_INF);
    return (rc != 0) ? svc_rc(rc) : 0;
}

int sem_trywait(sem_t *s)
{
    if (s == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    /* core's `take(ZERO)` busy ⇒ -ETIMEDOUT (INV-1); POSIX wants EAGAIN ⇒ translate once. */
    const int rc = br_sem_take(s, BR_TIMEOUT_ZERO);
    if (rc == BR_ERR(ETIMEDOUT)) {
        return svc_rc(BR_ERR(EAGAIN));
    }
    return (rc != 0) ? svc_rc(rc) : 0;
}

int sem_timedwait(sem_t *s, const struct timespec *abstime)
{
    if (s == BR_NULL || abstime == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
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
        return svc_rc(BR_ERR(EINVAL));
    }
    const int rc = br_sem_give(s);                  /* ISR-safe (core's contract) */
    return (rc != 0) ? svc_rc(rc) : 0;
}

int sem_getvalue(sem_t *s, int *out)
{
    if (s == BR_NULL || out == BR_NULL) {
        return svc_rc(BR_ERR(EINVAL));
    }
    *out = (int)br_sem_count(s);
    return 0;
}

/* ==================================================================== Layer: process identity */

pid_t getpid(void)  { return 1; }                   /* single APP ⇒ constant */
pid_t getppid(void) { return 0; }
uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }

/* ==================================================================== Lifecycle hooks */

int posix_early_init(void)
{
    /* EARLY: zero the static state only. The fd table lives in framework/file-table (its own
     * EARLY handles it); this file does not touch it — single-owner discipline. */
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
    /* LATE (the service phase): core's heap/scheduler/service registry are all ready by now.
     * v1 does **not** auto-open fd 0/1/2: that belongs to a "process model", absent here (see
     * stdio.h). The standard stream symbols stay NULL until someone fdopens them explicitly. */
    return 0;
}

/* ==================================================================== Layer: mounts
 *
 * ★ Why **here** (not in the FS plugin's start phase): ADR-0029 — mount plans belong to the
 *   **consumer** (the APP). The FS plugin only registers drivers (`br_fs_driver_register`);
 *   the APP uses POSIX `mount()` to decide which FS goes where.
 * ★ This layer is **pure forwarding + argument shaping**: mount semantics (path normalization,
 *   parent mounts, auto mkdir, table full) all live in `framework/vfs-core`; they are not
 *   reimplemented here (that would be a second source of truth).
 */
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data)
{
    /* v1 reserved bits: explicit rejection is more honest than "accepted but unused" — else the APP thinks flags/data took effect. */
    if (mountflags != 0ul || data != NULL) {
        errno = EINVAL;
        return -1;
    }
    const int rc = br_vfs_mount(source, target, filesystemtype, mountflags, data);
    if (rc != BR_OK) {
        errno = -rc;                 /* vfs returns a negative errno ⇒ zero-conversion into errno (11-01 §2) */
        return -1;
    }
    return 0;
}

int umount(const char *target)
{
    (void)target;
    /* Runtime unmount = design O-S3 (not before v3): it must first reclaim open handles and
     * transient inodes under the mount point, while the mount table's root inode and the static
     * composition-time layout assume "mounted once, never unmounted". Explicit -ENOTSUP. */
    errno = ENOTSUP;
    return -1;
}

int posix_start(void)
{
    br_log_info("runtime/posix: POSIX runtime ready (fd table owned by framework/file-table, cwd=%s)", s_cwd);
    br_log_info("runtime/posix: covers TR-A/TR-B; not-done items in docs/11-service/11-02-svc-posix-subset.md");
    return 0;
}
