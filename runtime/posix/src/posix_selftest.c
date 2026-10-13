/*
 * POSIX conformance suite (runtime/posix/src/posix_selftest.c); cases drive the POSIX headers so
 * the surface itself is tested, tmpfs is only the medium. Returns the failure count (0 = all
 * green) and logs [POSIXCONF] PASS/FAIL + SUMMARY; a failure does not stop the run.
 * WORKAROUND(br-wa-test-001): TC-POSIX-* are self-assigned; the case text is the criterion.
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

/* Selftest entry; the generated plugin_desc.c references it as its .selftest hook. */
int posix_selftest(void);

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

/* Create a file and write its full content (shared by later cases). Returns 0 / -1. */
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

/* Read back the whole content (at most cap bytes); returns the byte count or -1. */
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
    return (void *)7L;                     /* this value must travel back to the caller via pthread_join */
}

/* ---- 021: per-thread errno + barrier ---- */
static pthread_barrier_t s_bar21;
static int               s_err21[2];

static void *pt_errno_worker(void *arg)
{
    const int idx = (int)(br_intptr_t)arg;
    errno = (idx == 0) ? ENOENT : EACCES;
    (void)pthread_barrier_wait(&s_bar21);
    s_err21[idx] = errno;              /* the other thread already wrote its slot => must still read our own */
    (void)pthread_barrier_wait(&s_bar21);
    return BR_NULL;
}

static pthread_mutex_t s_m22;

static void *pt_eperm_worker(void *arg)
{
    (void)arg;
    return (void *)(br_intptr_t)pthread_mutex_unlock(&s_m22);
}

static pthread_once_t  *s_once23_p;
static int              s_once23_count;
static void s_once23_fn(void) { s_once23_count++; }
static void *pt_once_worker(void *arg)
{
    (void)arg;
    (void)pthread_once(s_once23_p, s_once23_fn);
    return BR_NULL;
}

/* ---- 024: rwlock (concurrent readers / exclusive writer) ---- */
static pthread_rwlock_t s_rw24 = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t  s_rw24_stats = PTHREAD_MUTEX_INITIALIZER;
static int              s_rw24_value;
static int              s_rw24_active;
static int              s_rw24_active_max;
static int              s_rw24_bad;

static void *pt_reader_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2; i++) {
        if (pthread_rwlock_rdlock(&s_rw24) != 0) {
            s_rw24_bad = 1;
            return BR_NULL;
        }
        (void)pthread_mutex_lock(&s_rw24_stats);
        s_rw24_active++;
        if (s_rw24_active > s_rw24_active_max) {
            s_rw24_active_max = s_rw24_active;
        }
        (void)pthread_mutex_unlock(&s_rw24_stats);
        usleep(10000u);                          /* sleep holding the read lock => another reader should get in too */
        (void)pthread_mutex_lock(&s_rw24_stats);
        s_rw24_active--;
        (void)pthread_mutex_unlock(&s_rw24_stats);
        (void)pthread_rwlock_unlock(&s_rw24);
    }
    return BR_NULL;
}

static void *pt_writer_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 4; i++) {
        if (pthread_rwlock_wrlock(&s_rw24) != 0) {
            s_rw24_bad = 1;
            return BR_NULL;
        }
        (void)pthread_mutex_lock(&s_rw24_stats);
        if (s_rw24_active != 0) {
            s_rw24_bad = 1;          /* no active readers allowed while the writer holds the lock */
        }
        s_rw24_value++;
        (void)pthread_mutex_unlock(&s_rw24_stats);
        (void)pthread_rwlock_unlock(&s_rw24);
    }
    return BR_NULL;
}

/* ---- 025: TLS key (isolation + destructor) ---- */
static pthread_key_t s_key25;
static int           s_key25_dtors;

static void s_key25_dtor(void *v) { s_key25_dtors += (int)(br_intptr_t)v; }

static void *pt_key_worker(void *arg)
{
    const int idx = (int)(br_intptr_t)arg;
    if (pthread_setspecific(s_key25, (void *)(br_intptr_t)(idx + 1)) != 0) {
        return (void *)(br_intptr_t)(-1);
    }
    (void)pthread_yield();
    const br_intptr_t got = (br_intptr_t)pthread_getspecific(s_key25);
    return (void *)(br_intptr_t)((got == (br_intptr_t)(idx + 1)) ? 0 : -2);
}

/* ---- 026: detach (lazy reclamation) ---- */
static int   s_det26_ran;
static sem_t s_det26_gate;
static void *pt_det_worker(void *arg) { (void)arg; s_det26_ran++; return BR_NULL; }
/* Block on the sem so the record is not yet ZOMBIE: "join detached => EINVAL" stays testable. */
static void *pt_det_gated(void *arg) { (void)arg; (void)sem_wait(&s_det26_gate); s_det26_ran++; return BR_NULL; }

/* ---- 027: attr / name ---- */
static br_u8 s_stack27[8192] BR_ALIGN(16);
static sem_t s_name27_gate;
static int   s_name27_ok;

static void *pt_name_worker(void *arg)
{
    (void)arg;
    (void)sem_wait(&s_name27_gate);
    char buf[BR_PTHREAD_NAME_MAX];
    if (pthread_getname_np(BR_NULL, buf, sizeof(buf)) != 0) {
        return (void *)(br_intptr_t)(-1);
    }
    s_name27_ok = (strcmp(buf, "worker27") == 0) ? 1 : 0;
    return BR_NULL;
}


int posix_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    char p1[PT_PATH];
    char p2[PT_PATH];
    char p3[PT_PATH];

    br_log_info("[POSIXCONF] runtime/posix conformance (fd / metadata / dir / link / thread / stdio)");

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

    /* ---- TC-POSIX-001: open/close/read/write/lseek round trip ---- */
    {
        const int fd = open(p1, O_RDWR | O_CREAT | O_TRUNC);
        br_bool ok = (fd >= 0);
        if (ok) {
            ok = (write(fd, "brickOS", 7u) == 7);
            ok = ok && (lseek(fd, 0, SEEK_SET) == 0);
            char buf[16];
            ok = ok && (read(fd, buf, 7u) == 7) && (buf[0] == 'b') && (buf[6] == 'S');
            ok = ok && (lseek(fd, 0, SEEK_END) == 7);
            ok = ok && (read(fd, buf, 4u) == 0);            /* at EOF => 0, not an error */
            ok = ok && (close(fd) == 0);
            ok = ok && (close(fd) == -1) && (errno == EBADF);   /* second close => EBADF */
        }
        pc(ok, "TC-POSIX-001", "open/read/write/lseek/close round trip; EOF => 0; second close => EBADF");
    }

    /* ---- TC-POSIX-002: lowest free fd / dup shares offset / dup2 replaces target ---- */
    {
        const int fd = open(p1, O_RDWR);
        const int d1 = dup(fd);
        int       d2 = -1;
        br_bool   ok = (fd >= 0) && (d1 >= 0) && (d1 != fd);

        ok = ok && (lseek(fd, 3, SEEK_SET) == 3) && (lseek(d1, 0, SEEK_CUR) == 3);

        d2 = open(p1, O_RDONLY);
        ok = ok && (d2 >= 0) && (dup2(fd, d2) == d2);
        ok = ok && (lseek(d2, 0, SEEK_CUR) == 3);

        ok = ok && (close(d2) == 0) && (lseek(fd, 0, SEEK_CUR) == 3);

        ok = ok && (close(d1) == 0) && (close(fd) == 0);
        pc(ok, "TC-POSIX-002", "dup shares offset; dup2 replaces target; closing one reference leaves the others");
    }

    /* ---- TC-POSIX-003: access-mode enforcement (vfs does not enforce it) ---- */
    {
        char    buf[8];
        const int ro = open(p1, O_RDONLY);
        const int wo = open(p1, O_WRONLY);
        br_bool   ok = (ro >= 0) && (wo >= 0);

        ok = ok && (write(ro, "x", 1u) == -1) && (errno == EBADF);   /* write on a read-only fd */
        ok = ok && (read(wo, buf, 1u) == -1) && (errno == EBADF);    /* read on a write-only fd */
        ok = ok && (ftruncate(ro, 1) == -1);                          /* truncate on a read-only fd */
        ok = ok && (close(ro) == 0) && (close(wo) == 0);
        pc(ok, "TC-POSIX-003", "write/ftruncate on a read-only fd => EBADF; read on a write-only fd => EBADF");
    }

    /* ---- TC-POSIX-004: pread/pwrite keep the offset; readv/writev gather/scatter ---- */
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
        ok = ok && (lseek(fd, 0, SEEK_CUR) == 0);          /* pwrite does not move the offset */
        const ssize_t pr = pread(fd, buf, 4u, 0);
        ok = ok && (pr == 4) && (buf[0] == '0') && (buf[3] == '3');
        ok = ok && (lseek(fd, 0, SEEK_CUR) == 0);          /* pread does not move the offset */

        wi[0].iov_base = (void *)"AB";
        wi[0].iov_len  = 2u;
        wi[1].iov_base = (void *)"CD";
        wi[1].iov_len  = 2u;
        const off_t sk = lseek(fd, 4, SEEK_SET);
        ok = ok && (sk == 4);
        const ssize_t wv = writev(fd, wi, 2);              /* appends at 4..8 (not overwriting 0..4) */
        ok = ok && (wv == 4);
        ok = ok && (lseek(fd, 4, SEEK_SET) == 4);          /* back to the start of what was just written, then read it back */

        ri[0].iov_base = b1;
        ri[0].iov_len  = 2u;
        ri[1].iov_base = b2;
        ri[1].iov_len  = 2u;
        const ssize_t rv = readv(fd, ri, 2);
        ok = ok && (rv == 4) && (b1[0] == 'A') && (b2[1] == 'D');
        ok = ok && (close(fd) == 0);
        pc(ok, "TC-POSIX-004", "pread/pwrite keep the offset; readv/writev gather by iovec");
    }

    /* ---- TC-POSIX-005: stat / fstat / lstat and type bits ---- */
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
        pc(ok, "TC-POSIX-005", "stat/fstat/lstat agree; S_ISREG/S_ISDIR; st_nlink = 1");
    }

    /* ---- TC-POSIX-006: namespace operations and errno mapping ---- */
    {
        br_bool ok = (mkdir(PT_BASE, 0777) == -1) && (errno == EEXIST);
        ok = ok && (rename(p1, p2) == 0);
        ok = ok && (access(p1, F_OK) == -1) && (errno == ENOENT);
        ok = ok && (access(p2, F_OK) == 0);
        ok = ok && (unlink(p1) == -1) && (errno == ENOENT);
        ok = ok && (rmdir(PT_BASE) == -1) && (errno == ENOTEMPTY);   /* directory not empty */
        ok = ok && (mkdir(p3, 0777) == 0) && (rmdir(p3) == 0);
        ok = ok && (remove(p2) == 0) && (access(p2, F_OK) == -1);
        pc(ok, "TC-POSIX-006", "mkdir/rename/unlink/rmdir/remove; EEXIST/ENOENT/ENOTEMPTY mapping");
    }

    /* ---- TC-POSIX-007: symlinks (lstat sees the link, stat follows, readlink reads it) ---- */
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

        char buf[8];
        ok = ok && (pt_read_all(p1, buf, sizeof(buf)) == 6) && (buf[0] == 'l');

        ok = ok && (open(p1, O_RDONLY | O_NOFOLLOW) == -1) && (errno == ELOOP);
        ok = ok && (readlink(p2, tgt, sizeof(tgt)) == -1) && (errno == EINVAL);
        ok = ok && (unlink(p1) == 0);
        pc(ok, "TC-POSIX-007", "symlink/readlink/lstat/stat; EEXIST/ELOOP/EINVAL; read/write through the link");
    }

    /* ---- TC-POSIX-008: hard links share the same data ---- */
    {
        struct stat st;
        pt_path(p1, PT_PATH, "h1");
        (void)pt_make(p2, "shared", 6u);

        br_bool ok = (link(p2, p1) == 0);
        ok = ok && (stat(p2, &st) == 0) && (st.st_nlink == 2);
        ok = ok && (link(p2, PT_BASE) == -1) && (errno == EEXIST || errno == EPERM ||
                                                 errno == EISDIR || errno == ENOENT);

        const int fd = open(p1, O_WRONLY);
        ok = ok && (fd >= 0) && (write(fd, "SHARED", 6u) == 6) && (close(fd) == 0);
        char buf[8];
        ok = ok && (pt_read_all(p2, buf, sizeof(buf)) == 6) && (buf[0] == 'S');

        ok = ok && (unlink(p2) == 0) && (stat(p1, &st) == 0) && (st.st_nlink == 1);
        ok = ok && (pt_read_all(p1, buf, sizeof(buf)) == 6) && (buf[3] == 'R');
        ok = ok && (unlink(p1) == 0);
        /* The next case needs b.txt and cases do not rely on earlier cleanup => restore it. */
        ok = ok && (pt_make(p2, "shared", 6u) == 0);
        pc(ok, "TC-POSIX-008", "link shares the data body; nlink counting; data survives deleting one name");
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
        /* "." + ".." + x.bin = 3 entries, exactly 1 of them REG */
        ok = ok && (n_all == 3) && (n_reg == 1) && (n_dir == 2);
        ok = ok && (opendir("/posixconf_no_such_dir") == BR_NULL);
        ok = ok && (unlink(pf) == 0) && (rmdir(p4) == 0);
        pc(ok, "TC-POSIX-009", "opendir/readdir/closedir: '.'+'..'+1 file; d_type correct");
    }

    /* ---- TC-POSIX-010: chdir / getcwd / relative paths ---- */
    {
        char cwd[PT_PATH];
        br_bool ok = (getcwd(cwd, sizeof(cwd)) != BR_NULL) && (cwd[0] == '/');
        ok = ok && (chdir(PT_BASE) == 0);
        ok = ok && (getcwd(cwd, sizeof(cwd)) != BR_NULL) && (cwd[0] == '/' );
        ok = ok && (pt_make("rel.txt", "rel", 3u) == 0);
        struct stat st;
        ok = ok && (stat(PT_BASE "/rel.txt", &st) == 0) && (st.st_size == 3);
        ok = ok && (chdir("..") == 0);
        char cwd2[PT_PATH];
        ok = ok && (getcwd(cwd2, sizeof(cwd2)) != BR_NULL) && (cwd2[1] == '\0'); /* back at "/" */
        ok = ok && (chdir("/posixconf_no_such_dir") == -1) && (errno == ENOENT);
        ok = ok && (unlink(PT_BASE "/rel.txt") == 0);
        ok = ok && (getcwd(cwd, 1u) == BR_NULL) && (errno == ERANGE);   /* buffer too small */
        pc(ok, "TC-POSIX-010", "chdir/getcwd round trip; relative path lands in the new cwd; small buffer => ERANGE");
    }

    /* ---- TC-POSIX-011: permission surface = no-op returning 0; existence still checked ---- */
    {
        br_bool ok = (chmod(p2, 0600) == 0) && (chown(p2, 100, 100) == 0);
        const mode_t old = umask(0022);
        ok = ok && (umask(old) == 0022);
        const int fd = open(p2, O_RDONLY);
        ok = ok && (fd >= 0) && (fchmod(fd, 0700) == 0) && (fchown(fd, 0, 0) == 0) && (close(fd) == 0);
        ok = ok && (access(p2, R_OK | W_OK | X_OK) == 0);           /* always allowed */
        ok = ok && (access("/posixconf_no_such_file", F_OK) == -1) && (errno == ENOENT);
        pc(ok, "TC-POSIX-011", "chmod/chown/fchmod/fchown/umask => 0; access checks existence but always allows");
    }

    /* ---- TC-POSIX-012: openat(AT_FDCWD) works; any other dirfd honestly -ENOTSUP ---- */
    {
        const int fd = openat(AT_FDCWD, p2, O_RDONLY);
        br_bool ok = (fd >= 0) && (close(fd) == 0);
        ok = ok && (openat(0, p2, O_RDONLY) == -1) && (errno == ENOTSUP);
        ok = ok && (open(p2, O_RDONLY | O_DIRECTORY) == -1) && (errno == ENOTDIR);
        pc(ok, "TC-POSIX-012", "openat(AT_FDCWD) works; real dirfd => ENOTSUP; O_DIRECTORY misuse => ENOTDIR");
    }

    /* ---- TC-POSIX-013: fcntl(F_GETFL/F_SETFL/F_DUPFD/record locks) ---- */
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

        struct flock q;
        q.l_type   = F_WRLCK;
        q.l_whence = SEEK_SET;
        q.l_start  = 0;
        q.l_len    = 0;
        q.l_pid    = 0;
        ok = ok && (fcntl(fb, F_GETLK, &q) == 0) && (q.l_type == F_WRLCK);

        struct flock c;
        c.l_type   = F_WRLCK;
        c.l_whence = SEEK_SET;
        c.l_start  = 0;
        c.l_len    = 0;
        c.l_pid    = 0;
        ok = ok && (fcntl(fb, F_SETLK, &c) == -1) && (errno == EAGAIN);

        fl.l_type = F_UNLCK;
        ok = ok && (fcntl(fa, F_SETLK, &fl) == 0);
        ok = ok && (fcntl(fb, F_GETLK, &q) == 0) && (q.l_type == F_UNLCK);

        ok = ok && (fcntl(fa, F_SETLKW, &fl) == -1) && (errno == ENOTSUP);
        ok = ok && (close(fa) == 0) && (close(fb) == 0);
        pc(ok, "TC-POSIX-013", "F_GETFL/F_SETFL; F_SETLK/F_GETLK conflict and unlock; F_SETLKW => ENOTSUP");
    }

    /* ---- TC-POSIX-014: poll / select ---- */
    {
        const int fd = open(p2, O_RDONLY);
        br_bool   ok = (fd >= 0);

        struct pollfd pf;
        pf.fd      = fd;
        pf.events  = POLLIN;
        pf.revents = 0;
        /* Memory files are always readable: no poll slot => this service answers "always ready". */
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
        ok = ok && (poll(&neg, 1u, 0) == 0) && (neg.revents == 0);      /* negative fd is ignored */

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 0;
        ok = ok && (select(fd + 1, &rd, BR_NULL, BR_NULL, &tv) >= 1) && (FD_ISSET(fd, &rd) != 0ul);
        ok = ok && (close(fd) == 0);
        pc(ok, "TC-POSIX-014", "poll: ready count / POLLNVAL / negative fd ignored; select reuses the same implementation");
    }

    /* ---- TC-POSIX-015: time (monotonic works, wall clock honestly -ENOTSUP) ---- */
    {
        struct timespec t1;
        struct timespec t2;
        struct timespec res;
        struct timespec req;
        br_bool ok = (clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
        ok = ok && (clock_gettime(CLOCK_MONOTONIC, &t2) == 0) && (t2.tv_sec >= t1.tv_sec);
        ok = ok && (clock_gettime(CLOCK_REALTIME, &t1) == -1) && (errno == ENOTSUP);
        ok = ok && (clock_getres(CLOCK_MONOTONIC, &res) == 0) &&
             (res.tv_nsec == (long)BR_POSIX_CLOCK_RES_NS);

        const br_time_t before = br_clock_now();
        req.tv_sec  = 0;
        req.tv_nsec = 1000L * 1000L;                       /* 1 ms */
        ok = ok && (nanosleep(&req, BR_NULL) == 0);
        const br_time_t after = br_clock_now();
        ok = ok && (after >= before) && ((after - before) >= 1000u);    /* no early wakeup */
        ok = ok && (usleep(1000u) == 0) && (sleep(0u) == 0u);
        pc(ok, "TC-POSIX-015", "MONOTONIC monotonic; REALTIME => ENOTSUP; getres = 1/HZ (one tick); nanosleep does not wake early");
    }

    /* ---- TC-POSIX-016: pthread threads + retval handoff ---- */
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
        ok = ok && (ret == (void *)7L);                    /* the entry point's return value comes back through join */
        ok = ok && (s_worker_ran == BR_TRUE) && (s_worker_value == 42);
        ok = ok && (pthread_equal(pthread_self(), pthread_self()) != 0);
        ok = ok && (pthread_yield() == 0);
        ok = ok && (pthread_detach(t) == ESRCH);            /* already-joined thread: record reclaimed => ESRCH */
        ok = ok && (sem_destroy(&s_sem_req) == 0) && (sem_destroy(&s_sem_done) == 0);
        pc(ok, "TC-POSIX-016", "pthread_create/join brings retval back; sem paired wakeup; detach after join => ESRCH");
    }

    /* ---- TC-POSIX-017: pthread mutex / condition variable (error-number channel) ---- */
    {
        pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;     /* static init: uses core's initial value directly */
        pthread_cond_t  c = PTHREAD_COND_INITIALIZER;
        br_bool ok = (pthread_mutex_lock(&m) == 0);
        ok = ok && (pthread_mutex_trylock(&m) == EBUSY);   /* busy => **returns** EBUSY (not -1) */
        ok = ok && (pthread_mutex_unlock(&m) == 0);
        ok = ok && (pthread_mutex_trylock(&m) == 0) && (pthread_mutex_unlock(&m) == 0);
        ok = ok && (pthread_cond_signal(&c) == 0);         /* no waiters: no-op */
        ok = ok && (pthread_cond_broadcast(&c) == 0);
        ok = ok && (pthread_mutex_destroy(&m) == 0) && (pthread_cond_destroy(&c) == 0);

        pthread_mutex_t m2;
        ok = ok && (pthread_mutex_init(&m2, BR_NULL) == 0);
        ok = ok && (pthread_mutex_lock(&m2) == 0) && (pthread_mutex_unlock(&m2) == 0);
        ok = ok && (pthread_mutex_destroy(&m2) == 0);
        pc(ok, "TC-POSIX-017", "pthread error-number channel (trylock => EBUSY); static initializers work; destroy => 0");
    }

    /* ---- TC-POSIX-018: stdio stream layer (no printf: TR-C) ---- */
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
        ok = ok && (fgetc(f) == EOF) && (feof(f) != 0);     /* read to the end => EOF flag */
        clearerr(f);
        ok = ok && (feof(f) == 0);
        ok = ok && (fseek(f, 6L, SEEK_SET) == 0);
        ok = ok && (fread(line, 1u, 5u, f) == 5u) && (line[0] == 'w');
        ok = ok && (fclose(f) == 0);

        char buf[16];
        ok = ok && (pt_read_all(p3, buf, sizeof(buf)) == 12) && (buf[11] == '\n');
        ok = ok && (unlink(p3) == 0);
        pc(ok, "TC-POSIX-018", "fopen/fwrite/fflush/fseek/ftell/fgets/fread/feof/fclose round trip");
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
        pc(ok, "TC-POSIX-019", "str*/mem*/atoi/abs/malloc/calloc/realloc/free semantics");
    }

    /* ---- TC-POSIX-020: errno value range and strerror ---- */
    {
        br_bool ok = (ENOENT == 2) && (EBADF == 9) && (EEXIST == 17) && (EINVAL == 22) &&
                     (ENOSPC == 28) && (ENOTEMPTY == 39) && (ELOOP == 40) &&
                     (ENOTSUP == 95) && (ETIMEDOUT == 110) && (EAGAIN == 11);
        ok = ok && (strcmp(strerror(ENOENT), "ENOENT") == 0);
        ok = ok && (strcmp(strerror(EBADF), "EBADF") == 0);
        ok = ok && (strcmp(strerror(9999), "UNKNOWN") == 0);
        ok = ok && (open(BR_NULL, O_RDONLY) == -1) && (errno == EINVAL);
        ok = ok && (read(999, BR_NULL, 1u) == -1) && (errno == EBADF);
        ok = ok && (close(999) == -1) && (errno == EBADF);
        ok = ok && (stat("/posixconf_no_such_file", BR_NULL) == -1);
        ok = ok && (unlink(BR_NULL) == -1);
        pc(ok, "TC-POSIX-020", "errno numbers = kernel; strerror; contract error codes for null args / invalid fd");
    }

    /* ---- TC-POSIX-021: barrier + per-thread errno ---- */
    {
        pthread_t a = BR_NULL;
        pthread_t b = BR_NULL;
        s_err21[0] = -1;
        s_err21[1] = -1;
        br_bool ok = (pthread_barrier_init(&s_bar21, BR_NULL, 2u) == 0);
        ok = ok && (pthread_create(&a, BR_NULL, pt_errno_worker, (void *)(br_intptr_t)0) == 0);
        ok = ok && (pthread_create(&b, BR_NULL, pt_errno_worker, (void *)(br_intptr_t)1) == 0);
        ok = ok && (pthread_join(a, BR_NULL) == 0);
        ok = ok && (pthread_join(b, BR_NULL) == 0);
        ok = ok && (s_err21[0] == ENOENT) && (s_err21[1] == EACCES);
        ok = ok && (pthread_barrier_destroy(&s_bar21) == 0);
        pc(ok, "TC-POSIX-021", "barrier syncs both parties + per-thread errno isolation (each writes its own slot)");
    }

    /* ---- TC-POSIX-022: RECURSIVE / ERRORCHECK / unlock by a non-owner ---- */
    {
        pthread_mutexattr_t ma;
        pthread_mutex_t     rec;
        pthread_mutexattr_t ea;
        pthread_mutex_t     ec;
        pthread_t           w = BR_NULL;
        void               *r = BR_NULL;
        int                 ty = -1;
        br_bool ok = (pthread_mutexattr_init(&ma) == 0);
        ok = ok && (pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE) == 0);
        ok = ok && (pthread_mutexattr_gettype(&ma, &ty) == 0) && (ty == PTHREAD_MUTEX_RECURSIVE);
        ok = ok && (pthread_mutex_init(&rec, &ma) == 0);
        ok = ok && (pthread_mutex_lock(&rec) == 0) && (pthread_mutex_lock(&rec) == 0);
        ok = ok && (pthread_mutex_unlock(&rec) == 0) && (pthread_mutex_unlock(&rec) == 0);
        ok = ok && (pthread_mutex_destroy(&rec) == 0);
        ok = ok && (pthread_mutexattr_destroy(&ma) == 0);

        ok = ok && (pthread_mutexattr_init(&ea) == 0);
        ok = ok && (pthread_mutexattr_settype(&ea, PTHREAD_MUTEX_ERRORCHECK) == 0);
        ok = ok && (pthread_mutex_init(&ec, &ea) == 0);
        ok = ok && (pthread_mutex_lock(&ec) == 0);
        ok = ok && (pthread_mutex_lock(&ec) == EDEADLK);        /* same thread relocks */
        s_m22 = ec;                                            /* borrow a thread to do the "non-owner unlock" */
        ok = ok && (pthread_create(&w, BR_NULL, pt_eperm_worker, BR_NULL) == 0);
        ok = ok && (pthread_join(w, &r) == 0) && ((br_intptr_t)r == EPERM);
        ok = ok && (pthread_mutex_unlock(&ec) == 0);
        ok = ok && (pthread_mutex_destroy(&ec) == 0);
        ok = ok && (pthread_mutexattr_destroy(&ea) == 0);
        pc(ok, "TC-POSIX-022", "RECURSIVE reentrant counting; ERRORCHECK self-lock EDEADLK + non-owner unlock EPERM");
    }

    /* ---- TC-POSIX-023: once (runs exactly once across threads) ---- */
    {
        pthread_once_t once = PTHREAD_ONCE_INIT;
        pthread_t t1 = BR_NULL;
        pthread_t t2 = BR_NULL;
        pthread_t t3 = BR_NULL;
        s_once23_count = 0;
        s_once23_p = &once;
        br_bool ok = (pthread_create(&t1, BR_NULL, pt_once_worker, BR_NULL) == 0);
        ok = ok && (pthread_create(&t2, BR_NULL, pt_once_worker, BR_NULL) == 0);
        ok = ok && (pthread_create(&t3, BR_NULL, pt_once_worker, BR_NULL) == 0);
        ok = ok && (pthread_join(t1, BR_NULL) == 0) && (pthread_join(t2, BR_NULL) == 0) &&
             (pthread_join(t3, BR_NULL) == 0);
        ok = ok && (s_once23_count == 1);
        pc(ok, "TC-POSIX-023", "pthread_once: three concurrent threads => init runs exactly once");
    }

    /* ---- TC-POSIX-024: rwlock (concurrent readers / exclusive writer) ---- */
    {
        pthread_t r1 = BR_NULL;
        pthread_t r2 = BR_NULL;
        pthread_t wr = BR_NULL;
        s_rw24_value      = 0;
        s_rw24_active     = 0;
        s_rw24_active_max = 0;
        s_rw24_bad        = 0;
        br_bool ok = (pthread_create(&r1, BR_NULL, pt_reader_worker, BR_NULL) == 0);
        ok = ok && (pthread_create(&r2, BR_NULL, pt_reader_worker, BR_NULL) == 0);
        ok = ok && (pthread_create(&wr, BR_NULL, pt_writer_worker, BR_NULL) == 0);
        ok = ok && (pthread_join(r1, BR_NULL) == 0) && (pthread_join(r2, BR_NULL) == 0) &&
             (pthread_join(wr, BR_NULL) == 0);
        ok = ok && (s_rw24_bad == 0) && (s_rw24_value == 4);
        ok = ok && (s_rw24_active_max >= 2);            /* two readers really held the lock concurrently */
        pc(ok, "TC-POSIX-024", "rwlock: readers concurrent (active_max>=2), writer exclusive, all 4 writes take effect");
    }

    /* ---- TC-POSIX-025: TLS key (isolation + destructor on exit) ---- */
    {
        pthread_key_t key;
        pthread_t     a = BR_NULL;
        pthread_t     b = BR_NULL;
        void         *ra = BR_NULL;
        void         *rb = BR_NULL;
        s_key25_dtors = 0;
        br_bool ok = (pthread_key_create(&key, s_key25_dtor) == 0);
        s_key25 = key;
        ok = ok && (pthread_create(&a, BR_NULL, pt_key_worker, (void *)(br_intptr_t)0) == 0);
        ok = ok && (pthread_create(&b, BR_NULL, pt_key_worker, (void *)(br_intptr_t)1) == 0);
        ok = ok && (pthread_join(a, &ra) == 0) && (pthread_join(b, &rb) == 0);
        ok = ok && ((br_intptr_t)ra == 0) && ((br_intptr_t)rb == 0);
        ok = ok && (s_key25_dtors == 3);                /* (0+1) + (1+1) */
        ok = ok && (pthread_key_delete(key) == 0);
        pc(ok, "TC-POSIX-025", "pthread_key_*: per-thread values isolated; destructor runs on thread exit (values 1+2)");
    }

    /* ---- TC-POSIX-026: detach (lazy reclamation) ---- */
    {
        pthread_t      t = BR_NULL;
        pthread_t      normal = BR_NULL;
        pthread_t      d[3];
        pthread_attr_t da;
        void          *r = BR_NULL;
        s_det26_ran = 0;
        br_bool ok = (sem_init(&s_det26_gate, 0, 0u) == 0);
        ok = ok && (pthread_attr_init(&da) == 0);
        ok = ok && (pthread_attr_setdetachstate(&da, PTHREAD_CREATE_DETACHED) == 0);
        ok = ok && (pthread_create(&t, &da, pt_det_gated, BR_NULL) == 0);   /* stuck on the gate */
        ok = ok && (pthread_attr_destroy(&da) == 0);
        ok = ok && (pthread_join(t, &r) == EINVAL);     /* join detached => EINVAL (the record still exists) */
        ok = ok && (sem_post(&s_det26_gate) == 0);      /* let it finish and exit */
        for (int i = 0; i < 3; i++) {
            ok = ok && (pthread_create(&d[i], BR_NULL, pt_det_worker, BR_NULL) == 0);
            ok = ok && (pthread_detach(d[i]) == 0);
        }
        for (int i = 0; i < 8; i++) {
            (void)pthread_yield();                      /* let the detached threads run to completion and exit */
        }
        usleep(20000u);
        ok = ok && (pthread_create(&normal, BR_NULL, pt_det_worker, BR_NULL) == 0);
        ok = ok && (pthread_join(normal, BR_NULL) == 0);
        ok = ok && (s_det26_ran == 5);                  /* gated + 3 detached + normal all ran */
        ok = ok && (sem_destroy(&s_det26_gate) == 0);
        pc(ok, "TC-POSIX-026", "attr DETACHED / detach => 0, join detached => EINVAL; later pthread calls reclaim lazily without exhausting TCBs");
    }

    /* ---- TC-POSIX-027: attr (stack/detachstate/guardsize) + thread name ---- */
    {
        pthread_attr_t a;
        pthread_t      t = BR_NULL;
        void          *st = BR_NULL;
        size_t         ss = 0u;
        size_t         gs = 0u;
        int            ds = -1;
        br_bool ok = (pthread_attr_init(&a) == 0);
        ok = ok && (pthread_attr_setstack(&a, s_stack27, sizeof(s_stack27)) == 0);
        ok = ok && (pthread_attr_getstack(&a, &st, &ss) == 0) && (st == s_stack27) &&
             (ss == sizeof(s_stack27));
        ok = ok && (pthread_attr_setdetachstate(&a, PTHREAD_CREATE_JOINABLE) == 0);
        ok = ok && (pthread_attr_getdetachstate(&a, &ds) == 0) && (ds == PTHREAD_CREATE_JOINABLE);
        ok = ok && (pthread_attr_setguardsize(&a, 4096u) == 0);
        ok = ok && (pthread_attr_getguardsize(&a, &gs) == 0) && (gs == 4096u);
        s_name27_ok = 0;
        ok = ok && (sem_init(&s_name27_gate, 0, 0u) == 0);
        ok = ok && (pthread_create(&t, &a, pt_name_worker, BR_NULL) == 0);
        ok = ok && (pthread_setname_np(t, "worker27") == 0);
        ok = ok && (sem_post(&s_name27_gate) == 0);
        ok = ok && (pthread_join(t, BR_NULL) == 0);
        ok = ok && (s_name27_ok == 1);
        char self_name[BR_PTHREAD_NAME_MAX];
        ok = ok && (pthread_getname_np(BR_NULL, self_name, sizeof(self_name)) == 0);
        ok = ok && (pthread_attr_destroy(&a) == 0) && (sem_destroy(&s_name27_gate) == 0);
        pc(ok, "TC-POSIX-027", "attr get/set (stack/detachstate/guardsize) + setname/getname_np round trip");
    }

    /* ---- TC-POSIX-028: condattr clock base + timed* timeouts ---- */
    {
        pthread_condattr_t ca;
        pthread_cond_t     c;
        pthread_mutex_t    m;
        int                clk = -1;
        int                ps = -1;
        struct timespec    past;
        past.tv_sec  = 0;
        past.tv_nsec = 0;
        br_bool ok = (pthread_condattr_init(&ca) == 0);
        ok = ok && (pthread_condattr_setclock(&ca, CLOCK_REALTIME) == EINVAL);   /* no wall clock */
        ok = ok && (pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0);
        ok = ok && (pthread_condattr_getclock(&ca, &clk) == 0) && (clk == CLOCK_MONOTONIC);
        ok = ok && (pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_PRIVATE) == 0);
        ok = ok && (pthread_condattr_getpshared(&ca, &ps) == 0) && (ps == PTHREAD_PROCESS_PRIVATE);
        ok = ok && (pthread_cond_init(&c, &ca) == 0) && (pthread_mutex_init(&m, BR_NULL) == 0);
        ok = ok && (pthread_mutex_lock(&m) == 0);
        ok = ok && (pthread_cond_timedwait(&c, &m, &past) == ETIMEDOUT);
        ok = ok && (pthread_mutex_timedlock(&m, &past) == ETIMEDOUT);   /* already held by ourselves */
        ok = ok && (pthread_mutex_unlock(&m) == 0);
        ok = ok && (pthread_cond_destroy(&c) == 0) && (pthread_mutex_destroy(&m) == 0);
        ok = ok && (pthread_condattr_destroy(&ca) == 0);
        pc(ok, "TC-POSIX-028", "condattr clock (MONOTONIC only) + cond/mutex timed* timeout => ETIMEDOUT");
    }

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
