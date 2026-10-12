#include <hello/hello.h>
#include <iface/min/min.h>
#include <iface/posix/posix.h>

#define BR_MAINLOOP_PERIOD_MS   1000u

/* ------------------------------------------------------------------ APPCONF fixture */

#define APP_WORKERS   3u
#define APP_ITERS     500

static br_u32 s_app_pass;
static br_u32 s_app_fail;

static void ap(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_app_pass++;
        br_log_info("[APPCONF] PASS %s %s", tag, what);
    } else {
        s_app_fail++;
        br_log_info("[APPCONF] FAIL %s %s", tag, what);
    }
}

/* ------------------------------------------------------------------ Case ①:
 * mutex-protected counter + barrier start + retval moved via join. */
static pthread_barrier_t s_start_bar;      /* APP_WORKERS + main thread */
static pthread_mutex_t   s_count_lock = PTHREAD_MUTEX_INITIALIZER;
static long              s_count;

static void *worker_count(void *arg)
{
    const long bump = (long)(br_intptr_t)arg;
    (void)pthread_barrier_wait(&s_start_bar);       /* all three workers on one start line */
    for (int i = 0; i < APP_ITERS; i++) {
        if (pthread_mutex_lock(&s_count_lock) != 0) {
            return (void *)(br_intptr_t)(-1);
        }
        s_count++;
        if (pthread_mutex_unlock(&s_count_lock) != 0) {
            return (void *)(br_intptr_t)(-2);
        }
    }
    return (void *)(br_intptr_t)bump;               /* retval returns to the main thread via join */
}

static br_bool case_mutex_count(void)
{
    pthread_t t[APP_WORKERS];
    s_count = 0;
    br_bool ok = (pthread_barrier_init(&s_start_bar, BR_NULL, APP_WORKERS + 1u) == 0);
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        ok = ok && (pthread_create(&t[i], BR_NULL, worker_count, (void *)(br_intptr_t)(i + 1u)) == 0);
    }
    (void)pthread_barrier_wait(&s_start_bar);        /* release the start line */
    long sum = 0;
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        void *ret = BR_NULL;
        ok = ok && (pthread_join(t[i], &ret) == 0);
        sum += (long)(br_intptr_t)ret;               /* sum of retvals = 1+2+3 */
    }
    ok = ok && (sum == 6) && (s_count == (long)(APP_WORKERS * (unsigned)APP_ITERS));
    ok = ok && (pthread_barrier_destroy(&s_start_bar) == 0);
    return ok;
}

/* ------------------------------------------------------------------ Case ②:
 * condvar rendezvous (main thread waits for all workers ready, then broadcasts go). */
static pthread_mutex_t s_join_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_join_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  s_join_go = PTHREAD_COND_INITIALIZER;
static int             s_join_count;
static int             s_join_go_flag;

static void *worker_join(void *arg)
{
    (void)arg;
    if (pthread_mutex_lock(&s_join_lock) != 0) {
        return (void *)(br_intptr_t)(-1);
    }
    s_join_count++;
    (void)pthread_cond_signal(&s_join_ready);
    while (s_join_go_flag == 0) {
        if (pthread_cond_wait(&s_join_go, &s_join_lock) != 0) {
            (void)pthread_mutex_unlock(&s_join_lock);
            return (void *)(br_intptr_t)(-2);
        }
    }
    (void)pthread_mutex_unlock(&s_join_lock);
    return BR_NULL;
}

static br_bool case_cond_rendezvous(void)
{
    pthread_t t[APP_WORKERS];
    s_join_count   = 0;
    s_join_go_flag = 0;
    br_bool ok = BR_TRUE;
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        ok = ok && (pthread_create(&t[i], BR_NULL, worker_join, BR_NULL) == 0);
    }
    ok = ok && (pthread_mutex_lock(&s_join_lock) == 0);
    while (s_join_count < (int)APP_WORKERS) {
        const int w = pthread_cond_wait(&s_join_ready, &s_join_lock);
        if (w != 0) {
            ok = BR_FALSE;
            break;
        }
    }
    s_join_go_flag = 1;
    ok = ok && (pthread_cond_broadcast(&s_join_go) == 0);
    ok = ok && (pthread_mutex_unlock(&s_join_lock) == 0);
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        ok = ok && (pthread_join(t[i], BR_NULL) == 0);
    }
    return ok;
}

/* ------------------------------------------------------------------ Case ③:
 * TLS key: per-thread value isolation + destructor on exit. */
static pthread_key_t s_tls_key;
static int           s_tls_dtors;

static void tls_dtor(void *v) { s_tls_dtors += (int)(br_intptr_t)v; }

static void *worker_tls(void *arg)
{
    const int idx = (int)(br_intptr_t)arg;
    if (pthread_setspecific(s_tls_key, (void *)(br_intptr_t)(idx + 1)) != 0) {
        return (void *)(br_intptr_t)(-1);
    }
    (void)pthread_yield();                           /* let other threads write their own slots too */
    const long got = (long)(br_intptr_t)pthread_getspecific(s_tls_key);
    return (void *)(br_intptr_t)((got == (long)(idx + 1)) ? 0 : -2);
}

static br_bool case_tls_key(void)
{
    pthread_t t[APP_WORKERS];
    s_tls_dtors = 0;
    br_bool ok = (pthread_key_create(&s_tls_key, tls_dtor) == 0);
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        ok = ok && (pthread_create(&t[i], BR_NULL, worker_tls, (void *)(br_intptr_t)i) == 0);
    }
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        void *r = BR_NULL;
        ok = ok && (pthread_join(t[i], &r) == 0) && ((br_intptr_t)r == 0);
    }
    ok = ok && (s_tls_dtors == 6);                   /* (0+1)+(1+1)+(2+1) */
    ok = ok && (pthread_key_delete(s_tls_key) == 0);
    return ok;
}

/* ------------------------------------------------------------------ Case ④:
 * detach (lazy reclaim): the detached thread still runs; join detached => EINVAL. */
static int            s_det_ran;
static sem_t          s_det_gate;

static void *worker_det(void *arg)
{
    (void)arg;
    (void)sem_wait(&s_det_gate);
    s_det_ran++;
    return BR_NULL;
}

static br_bool case_detach(void)
{
    pthread_attr_t a;
    pthread_t      t = BR_NULL;
    s_det_ran = 0;
    br_bool ok = (sem_init(&s_det_gate, 0, 0u) == 0);
    ok = ok && (pthread_attr_init(&a) == 0);
    ok = ok && (pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED) == 0);
    ok = ok && (pthread_create(&t, &a, worker_det, BR_NULL) == 0);
    ok = ok && (pthread_attr_destroy(&a) == 0);
    void *r = BR_NULL;
    ok = ok && (pthread_join(t, &r) == EINVAL);      /* record still exists => EINVAL (not ESRCH) */
    ok = ok && (sem_post(&s_det_gate) == 0);
    for (int i = 0; i < 8; i++) {
        (void)pthread_yield();                       /* let it exit; later calls trigger lazy reclaim */
    }
    ok = ok && (s_det_ran == 1);
    ok = ok && (sem_destroy(&s_det_gate) == 0);
    return ok;
}

/* ------------------------------------------------------------------ Case ⑤:
 * per-thread errno isolation (each thread writes its own slot; barrier makes the write precede the read). */
static pthread_barrier_t s_err_bar;
static int               s_err_seen[2];

static void *worker_errno(void *arg)
{
    const int idx = (int)(br_intptr_t)arg;
    errno = (idx == 0) ? ENOENT : EACCES;
    (void)pthread_barrier_wait(&s_err_bar);
    s_err_seen[idx] = errno;
    (void)pthread_barrier_wait(&s_err_bar);
    return BR_NULL;
}

static br_bool case_errno_isolated(void)
{
    pthread_t a = BR_NULL;
    pthread_t b = BR_NULL;
    s_err_seen[0] = -1;
    s_err_seen[1] = -1;
    br_bool ok = (pthread_barrier_init(&s_err_bar, BR_NULL, 2u) == 0);
    ok = ok && (pthread_create(&a, BR_NULL, worker_errno, (void *)(br_intptr_t)0) == 0);
    ok = ok && (pthread_create(&b, BR_NULL, worker_errno, (void *)(br_intptr_t)1) == 0);
    ok = ok && (pthread_join(a, BR_NULL) == 0) && (pthread_join(b, BR_NULL) == 0);
    ok = ok && (s_err_seen[0] == ENOENT) && (s_err_seen[1] == EACCES);
    ok = ok && (pthread_barrier_destroy(&s_err_bar) == 0);
    return ok;
}

/* ------------------------------------------------------------------ Case ⑥:
 * rwlock: concurrent readers / exclusive writer. */
static pthread_rwlock_t s_rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t  s_rw_stats = PTHREAD_MUTEX_INITIALIZER;
static int              s_rw_value;
static int              s_rw_active;
static int              s_rw_active_max;
static int              s_rw_bad;

static void *worker_reader(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2; i++) {
        if (pthread_rwlock_rdlock(&s_rw) != 0) {
            s_rw_bad = 1;
            return BR_NULL;
        }
        (void)pthread_mutex_lock(&s_rw_stats);
        s_rw_active++;
        if (s_rw_active > s_rw_active_max) {
            s_rw_active_max = s_rw_active;
        }
        (void)pthread_mutex_unlock(&s_rw_stats);
        (void)usleep(5000u);                         /* sleep holding the read lock => another reader can enter */
        (void)pthread_mutex_lock(&s_rw_stats);
        s_rw_active--;
        (void)pthread_mutex_unlock(&s_rw_stats);
        (void)pthread_rwlock_unlock(&s_rw);
    }
    return BR_NULL;
}

static void *worker_writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 4; i++) {
        if (pthread_rwlock_wrlock(&s_rw) != 0) {
            s_rw_bad = 1;
            return BR_NULL;
        }
        (void)pthread_mutex_lock(&s_rw_stats);
        if (s_rw_active != 0) {
            s_rw_bad = 1;                            /* no active readers allowed while a writer holds the lock */
        }
        s_rw_value++;
        (void)pthread_mutex_unlock(&s_rw_stats);
        (void)pthread_rwlock_unlock(&s_rw);
    }
    return BR_NULL;
}

static br_bool case_rwlock(void)
{
    pthread_t r1 = BR_NULL;
    pthread_t r2 = BR_NULL;
    pthread_t w = BR_NULL;
    s_rw_value      = 0;
    s_rw_active     = 0;
    s_rw_active_max = 0;
    s_rw_bad        = 0;
    br_bool ok = (pthread_create(&r1, BR_NULL, worker_reader, BR_NULL) == 0);
    ok = ok && (pthread_create(&r2, BR_NULL, worker_reader, BR_NULL) == 0);
    ok = ok && (pthread_create(&w, BR_NULL, worker_writer, BR_NULL) == 0);
    ok = ok && (pthread_join(r1, BR_NULL) == 0) && (pthread_join(r2, BR_NULL) == 0) &&
         (pthread_join(w, BR_NULL) == 0);
    ok = ok && (s_rw_bad == 0) && (s_rw_value == 4) && (s_rw_active_max >= 2);
    return ok;
}

/* ------------------------------------------------------------------ Case ⑦:
 * once (exactly once) + thread name set/get. */
static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static int            s_once_count;
static sem_t          s_name_gate;
static int            s_name_ok;

static void once_fn(void) { s_once_count++; }

static void *worker_once(void *arg)
{
    (void)arg;
    (void)pthread_once(&s_once, once_fn);
    return BR_NULL;
}

static void *worker_name(void *arg)
{
    (void)arg;
    (void)sem_wait(&s_name_gate);
    char buf[BR_PTHREAD_NAME_MAX];
    if (pthread_getname_np(BR_NULL, buf, sizeof(buf)) != 0) {
        return (void *)(br_intptr_t)(-1);
    }
    s_name_ok = (strcmp(buf, "app-worker") == 0) ? 1 : 0;
    return BR_NULL;
}

static br_bool case_once_and_name(void)
{
    pthread_t t[APP_WORKERS];
    pthread_t named = BR_NULL;
    s_once_count = 0;
    s_name_ok    = 0;
    br_bool ok = (sem_init(&s_name_gate, 0, 0u) == 0);
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        ok = ok && (pthread_create(&t[i], BR_NULL, worker_once, BR_NULL) == 0);
    }
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        ok = ok && (pthread_join(t[i], BR_NULL) == 0);
    }
    ok = ok && (s_once_count == 1);
    ok = ok && (pthread_create(&named, BR_NULL, worker_name, BR_NULL) == 0);
    ok = ok && (pthread_setname_np(named, "app-worker") == 0);
    ok = ok && (sem_post(&s_name_gate) == 0);
    ok = ok && (pthread_join(named, BR_NULL) == 0);
    ok = ok && (s_name_ok == 1);
    char self[BR_PTHREAD_NAME_MAX];
    ok = ok && (pthread_getname_np(BR_NULL, self, sizeof(self)) == 0);
    ok = ok && (sem_destroy(&s_name_gate) == 0);
    return ok;
}

/* ------------------------------------------------------------------ Cases ⑧–⑭:
 * End-to-end samples for media-class filesystems (third-party FAT / EROFS ports) (ADR-0022).
 *
 * ★ **Include only POSIX headers, call only the POSIX face** (via the iface/posix skin): the value of
 *   this family is exactly that "the APP sees no third filesystem" — only ordinary
 *   open/read/write/stat/opendir on the four mount points `/fat12`, `/fat16`, `/fat32`, `/assets`.
 *   The mount table, block device, SDHCI and LZ4 decompression are all several layers below; the APP
 *   knows none of it.
 * ★ The criterion is **byte-for-byte**: the host uses authoritative external tools (mkfs.fat + mcopy /
 *   mkfs.erofs) to place these bytes into the volumes (build/emmc0..3.img, see tools/mk-fs-images.sh) —
 *   a homemade mkfs and a homemade reader mirroring each other prove nothing, so "bytes read == bytes
 *   put in by the external tool" is this family's root criterion.
 * ★ Write-path cases **clean up after themselves** (unlink before create, unlink/rmdir after use): the
 *   gate may boot the same image repeatedly, and leftover garbage would drift the next directory check.
 */

/* Chunk-compare buffer: 512 B is intentional — APP thread stacks are 4 KiB-class and the largest
 * checked file is 70000 B (the EROFS LZ4 artifact); it cannot be read into the stack whole. */
#define FS_CHUNK 512u
static br_u8 s_fs_buf[FS_CHUNK];

/* Expected-byte generators (ids correspond one-to-one with tools/mk-fs-images.sh content generation) */
#define FSGEN_CONST   0   /* constant character `ch` */
#define FSGEN_ABCDEF  1   /* "ABCDEFGH" repeated forever (the EROFS LZ4 artifact) */
#define FSGEN_IDX     2   /* byte i = i & 0xFF */
#define FSGEN_PRNG    3   /* same LCG as the image-building script (EROFS raw.bin) */

static br_u8 fs_gen(int gen, char ch, br_u64 i)
{
    switch (gen) {
    case FSGEN_CONST:  return (br_u8)ch;
    case FSGEN_ABCDEF: return (br_u8)("ABCDEFGH"[i % 8u]);
    case FSGEN_IDX:    return (br_u8)(i & 0xFFu);
    case FSGEN_PRNG:   return (br_u8)((((i * 1103515245ull) + 12345ull) >> 16) & 0xFFull);
    default:           return 0u;
    }
}

/* Read exactly `len` bytes and compare byte-for-byte; also requires "exactly EOF here" (one more read must return 0). */
static br_bool fs_check_read(const char *path, br_u64 len, int gen, char ch)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        br_log_info("[APPCONF]   (open %s failed errno=%d)", path, errno);
        return BR_FALSE;
    }

    br_bool ok  = BR_TRUE;
    br_u64  got = 0u;
    while (got < len) {
        const br_u64 left = len - got;
        const size_t want = (left < (br_u64)FS_CHUNK) ? (size_t)left : (size_t)FS_CHUNK;
        const ssize_t n = read(fd, s_fs_buf, want);
        if (n <= 0) {
            ok = BR_FALSE;
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (s_fs_buf[i] != fs_gen(gen, ch, got + (br_u64)i)) {
                ok = BR_FALSE;
            }
        }
        got += (br_u64)n;
    }

    if (ok == BR_TRUE) {
        br_u8 tail = 0u;
        if (read(fd, &tail, 1u) != 0) {   /* the file must end exactly at len */
            ok = BR_FALSE;
        }
    }
    (void)close(fd);
    return ok;
}

/* Compare a constant string byte-for-byte (no generator path; a plainer criterion: logs show where it differs). */
static br_bool fs_check_exact(const char *path, const char *text)
{
    const size_t len = strlen(text);
    const int    fd  = open(path, O_RDONLY);
    if (fd < 0) {
        return BR_FALSE;
    }
    size_t got = 0u;
    br_bool ok = BR_TRUE;
    while (got < len) {
        const size_t  want = ((len - got) < (size_t)FS_CHUNK) ? (len - got) : (size_t)FS_CHUNK;
        const ssize_t n    = read(fd, s_fs_buf, want);
        if (n <= 0) { ok = BR_FALSE; break; }
        for (ssize_t i = 0; i < n; i++) {
            if (s_fs_buf[i] != (br_u8)text[got + (size_t)i]) { ok = BR_FALSE; }
        }
        got += (size_t)n;
    }
    if (ok == BR_TRUE) {
        br_u8 tail = 0u;
        if (read(fd, &tail, 1u) != 0) { ok = BR_FALSE; }
    }
    (void)close(fd);
    return ok;
}

/* APP-FS-001: all four mount points present; hello.txt type/size/content byte-correct (uncompressed path) */
static br_bool case_fs_hello(void)
{
    static const struct { const char *path; const char *text; } t[] = {
        { "/fat12/hello.txt",  "fat12 hello\n" },
        { "/fat16/hello.txt",  "fat16 hello\n" },
        { "/fat32/hello.txt",  "fat32 hello\n" },
        { "/assets/hello.txt", "erofs hello\n" },
    };
    br_bool ok = BR_TRUE;
    for (unsigned i = 0u; i < (sizeof(t) / sizeof(t[0])); i++) {
        struct stat st;
        if (stat(t[i].path, &st) != 0) { ok = BR_FALSE; continue; }
        ok = ok && S_ISREG(st.st_mode);
        ok = ok && (st.st_size == (off_t)strlen(t[i].text));
        ok = ok && fs_check_exact(t[i].path, t[i].text);
    }
    return ok;
}

/* APP-FS-002: dir/a.bin on each volume compared byte-for-byte via generators (cross-block/large reads) */
static br_bool case_fs_binaries(void)
{
    br_bool ok = BR_TRUE;
    ok = ok && fs_check_read("/fat12/dir/a.bin",  16u,    FSGEN_CONST, 'A');
    ok = ok && fs_check_read("/fat16/dir/a.bin",  256u,   FSGEN_CONST, 'B');
    ok = ok && fs_check_read("/fat32/dir/a.bin",  4096u,  FSGEN_CONST, 'C');
    ok = ok && fs_check_read("/assets/dir/a.bin", 4096u,  FSGEN_IDX,   0);
    ok = ok && fs_check_exact("/assets/dir/sub/deep.txt", "deep\n");
    return ok;
}

/* APP-FS-003: FAT write round-trip (create -> three writes -> fsync -> close -> reopen/read back -> stat -> unlink) */
static br_bool case_fs_write_roundtrip(void)
{
    const char *path = "/fat16/app-rw.txt";
    const char *text = "brickOS fat roundtrip";

    (void)unlink(path);

    const int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        return BR_FALSE;
    }

    br_bool ok = (write(fd, "brickOS ", 8u) == 8);
    ok = ok && (write(fd, "fat ", 4u) == 4);
    ok = ok && (write(fd, "roundtrip", 9u) == 9);
    ok = ok && (fsync(fd) == 0);
    ok = ok && (close(fd) == 0);

    struct stat st;
    ok = ok && (stat(path, &st) == 0) && (st.st_size == (off_t)strlen(text));
    ok = ok && fs_check_exact(path, text);

    ok = ok && (unlink(path) == 0);
    return ok;
}

/* APP-FS-004: FAT directory face (mkdir -> create file inside -> opendir/readdir sees it -> clean up) */
static br_bool case_fs_directory(void)
{
    const char *dir  = "/fat32/appdir";
    const char *file = "/fat32/appdir/in.txt";

    (void)unlink(file);
    (void)rmdir(dir);

    br_bool ok = (mkdir(dir, 0755) == 0);
    const int fd = ok ? open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644) : -1;
    ok = ok && (fd >= 0);
    if (fd >= 0) {
        ok = ok && (write(fd, "in\n", 3u) == 3);
        ok = ok && (close(fd) == 0);
    }

    DIR *d = opendir(dir);
    ok = ok && (d != BR_NULL);
    if (d != BR_NULL) {
        br_bool seen_dot = BR_FALSE;
        br_bool seen_dotdot = BR_FALSE;
        br_bool seen_in = BR_FALSE;
        struct dirent *e;
        while ((e = readdir(d)) != BR_NULL) {
            if (strcmp(e->d_name, ".") == 0)  { seen_dot = BR_TRUE; }
            if (strcmp(e->d_name, "..") == 0) { seen_dotdot = BR_TRUE; }
            if (strcmp(e->d_name, "in.txt") == 0) {
                seen_in = BR_TRUE;
                ok = ok && (e->d_type == DT_REG);
            }
        }
        ok = ok && seen_dot && seen_dotdot && seen_in;
        ok = ok && (closedir(d) == 0);
    }

    ok = ok && (unlink(file) == 0);
    ok = ok && (rmdir(dir) == 0);
    return ok;
}

/* APP-FS-005: FAT rename + truncate (old name gone after rename; truncate changes size and content) */
static br_bool case_fs_rename_truncate(void)
{
    const char *old_path = "/fat12/app-ren.txt";
    const char *new_path = "/fat12/app-ren2.txt";

    (void)unlink(old_path);
    (void)unlink(new_path);

    const int fd = open(old_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        return BR_FALSE;
    }
    br_bool ok = (write(fd, "0123456789", 10u) == 10);
    ok = ok && (close(fd) == 0);

    struct stat st;
    ok = ok && (rename(old_path, new_path) == 0);
    errno = 0;
    ok = ok && (stat(old_path, &st) == -1) && (errno == ENOENT);
    ok = ok && (stat(new_path, &st) == 0) && (st.st_size == 10);

    ok = ok && (truncate(new_path, 4) == 0);
    ok = ok && (stat(new_path, &st) == 0) && (st.st_size == 4);
    ok = ok && fs_check_exact(new_path, "0123");

    ok = ok && (unlink(new_path) == 0);
    return ok;
}

/* APP-FS-006: EROFS read-only enforcement (write/mkdir/unlink must give EROFS, not another error) + LZ4 read path */
static br_bool case_fs_erofs_readonly_lz4(void)
{
    errno = 0;
    const int wfd = open("/assets/hello.txt", O_WRONLY);
    br_bool ok = (wfd == -1) && (errno == EROFS);
    if (wfd >= 0) {
        (void)close(wfd);
    }

    errno = 0;
    ok = ok && (mkdir("/assets/appdir", 0755) == -1) && (errno == EROFS);

    errno = 0;
    ok = ok && (unlink("/assets/hello.txt") == -1) && (errno == EROFS);

    /* compressed path: 70000 B of repeated "ABCDEFGH" (host-built with mkfs.erofs -zlz4) */
    ok = ok && fs_check_read("/assets/comp/lz4.txt", 70000u, FSGEN_ABCDEF, 0);
    /* another compressed path: 8192 B near-random (the barely compressible kind) */
    ok = ok && fs_check_read("/assets/raw.bin", 8192u, FSGEN_PRNG, 0);
    return ok;
}

/* APP-FS-007: VFAT long file name (host placed "long name file.txt", short name LONGNA~1.TXT) */
static br_bool case_fs_longname(void)
{
    const char *path = "/fat32/long name file.txt";
    struct stat st;
    br_bool ok = (stat(path, &st) == 0);
    ok = ok && S_ISREG(st.st_mode) && (st.st_size == 4);
    ok = ok && fs_check_exact(path, "lfn\n");
    return ok;
}

/* ------------------------------------------------------------------ Entry */

static void app_pthread_conformance(void)
{
    s_app_pass = 0u;
    s_app_fail = 0u;
    br_log_info("[APPCONF] ==== POSIX pthread multithreading conformance (APP side; via iface/posix) ====");
    ap(case_mutex_count(),     "APP-PT-001", "mutex-protected counter (3x500) + barrier start + retval moved via join");
    ap(case_cond_rendezvous(), "APP-PT-002", "condvar rendezvous: main thread waits for all workers ready, then broadcasts go");
    ap(case_tls_key(),         "APP-PT-003", "pthread_key_*: per-thread value isolation + destructor on exit");
    ap(case_detach(),          "APP-PT-004", "detach: detached thread still runs; join detached => EINVAL");
    ap(case_errno_isolated(),  "APP-PT-005", "per-thread errno isolation (each thread writes its own slot)");
    ap(case_rwlock(),          "APP-PT-006", "rwlock: concurrent readers (active_max>=2), exclusive writer");
    ap(case_once_and_name(),   "APP-PT-007", "pthread_once exactly once + setname/getname_np");
    br_log_info("[APPCONF] SUMMARY pass=%u fail=%u total=%u", s_app_pass, s_app_fail,
                s_app_pass + s_app_fail);
}

/*
 * Media-class filesystem conformance (ADR-0022). **Shares** the same counter pair and the same SUMMARY
 * line as the set above — so the gate's one-liner `[APPCONF] SUMMARY ... fail=0` covers both families,
 * while the per-case tags still flag "one family got compiled out" as before.
 *
 * ★ Why this family runs in the **start phase** (called directly by `hello_start`) rather than in an APP
 *   thread: the selftest pass runs "after all starts, before `br_sched_run()`", and **timer preemption is
 *   already in effect before that** (the ADR-0011 IRQ-exit stack switch only looks at "is any task ready",
 *   not at whether `br_sched_run()` was called). Measured: the first version put this family in an APP
 *   thread and it immediately ran concurrently with `[SELFTEST]`, so **both sides touched the same
 *   card/volume at once**: the EROFS side read `bdev read failed lba=2 rc=-5`, two eMMC cases each hit the
 *   full 2 s timeout (sdhci wait timeout/pending=0), and the APP-side FAT write round-trip also went red.
 *   In the start phase there is no such concurrency window: there is only one control flow (the boot
 *   context), and all four volumes are mounted, interrupts are on, the block device is registered — every
 *   precondition this family needs **holds**.
 *   (This also honestly exposes the v1 boundary "this generation's FS stack does not guarantee
 *    multithreaded concurrency": vfs-core / fs/tmpfs / both ports have no volume-level lock, which is a
 *    different matter from "device ops are self-locked by the driver".)
 *
 * WORKAROUND(br-wa-test-001): like `APP-PT-*`, `APP-FS-*` is **self-numbered** — the `6-01` design case
 * table has no "APP's own samples" family. Registered together with the other families' numbering debt.
 */
static void app_fs_conformance(void)
{
    s_app_pass = 0u;
    s_app_fail = 0u;
    br_log_info("[APPCONF] ==== media-class filesystem end-to-end (FAT12/16/32 + EROFS; via iface/posix) ====");
    ap(case_fs_hello(),             "APP-FS-001", "all four mount points present; hello.txt type/size/content byte-correct");
    ap(case_fs_binaries(),          "APP-FS-002", "dir/a.bin on each volume compared byte-for-byte via generators (cross-block reads)");
    ap(case_fs_write_roundtrip(),   "APP-FS-003", "FAT write round-trip: create->three writes->fsync->reopen/read back->stat->unlink");
    ap(case_fs_directory(),         "APP-FS-004", "FAT directory face: mkdir->create file->opendir/readdir sees it->clean up");
    ap(case_fs_rename_truncate(),   "APP-FS-005", "FAT rename (old name ENOENT) + truncate changes size and content");
    ap(case_fs_erofs_readonly_lz4(),"APP-FS-006", "EROFS read-only enforcement (EROFS) + LZ4 artifact read back byte-for-byte");
    ap(case_fs_longname(),          "APP-FS-007", "VFAT long name \"long name file.txt\" can be stat'ed/read");
    br_log_info("[APPCONF] SUMMARY pass=%u fail=%u total=%u", s_app_pass, s_app_fail,
                s_app_pass + s_app_fail);
}

/*
 * APP thread body = MainLoop (**never returns**): run the multithreading conformance cases once, then
 * enter the periodic heartbeat.
 *
 * ⚠ The "sleep" in the heartbeat loop uses `usleep` (POSIX; implemented in runtime/posix, ending at core's
 *   `br_task_sleep`). Wakeup resolution = one tick (the inverse of `product.toml [kernel].hz`, default 200
 *   => 5 ms; ADR-0017) => each tick is really about 1.00–1.01 s; in the 3 s smoke gate `tick=2` still holds.
 *   The criterion "delay >= requested" is the "no early wakeup" duty of design 3-01 §2.1.
 */
static void *hello_mainloop(void *arg)
{
    (void)arg;

    app_pthread_conformance();

    br_u64 tick = 0;
    for (;;) {
        tick++;

        const br_time_t t_before = br_clock_now();
        (void)usleep(BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS);
        const br_time_t measured = br_clock_now() - t_before;
        const br_bool delay_ok = (measured >= BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS);

        br_log_info("tick=%lu uptime=%lu us delay=%lu us (>=%lu us: %s) irq_ticks=%lu",
                    tick,
                    (br_u64)br_clock_now(),
                    (br_u64)measured,
                    (br_u64)(BR_MAINLOOP_PERIOD_MS * BR_US_PER_MS),
                    delay_ok ? "ok" : "EARLY",
                    (br_u64)br_clock_tick_count());
    }
    return BR_NULL;
}

/* EARLY phase: no heap/threads/interrupts => the APP has nothing to do (the hook must be present). */
int hello_early_init(void)
{
    return 0;
}

/* CORE phase: raise the log level (logging is ready). */
int hello_init(void)
{
    br_log_set_level(BR_LOG_DEBUG);
    br_log_info("app: init (CORE phase; POSIX multithreading samples start threads in the start phase)");
    return 0;
}

/*
 * START phase (**the last** start): set the APP thread name -> `pthread_create` the thread -> return 0;
 * the manager then calls `br_sched_run()` for the first schedule, and the APP thread takes over MainLoop.
 * ★ Selftests are not driven by the APP (ADR-0010): core runs each plugin's selftest after all starts;
 *   the `[APPCONF]` here is the **APP's own multithreading samples**, not plugin selftest.
 */
/* ==================================================================== Mounting (ADR-0030)
 *
 * ★ **The APP decides mounting**: FS plugins only register drivers with vfs-core
 *   (`"tmpfs"`/`"devfs"`/`"fat"`/`"erofs"`) and no longer mount themselves. What to mount and where
 *   is written in this plugin's `brick.toml` under `[[mount]]`; brickie generates that table plus
 *   the walk into `hello_mount_plan_apply()` (see the notes in `include/hello/hello.h`: the APP may
 *   not include core headers, so the table does not enter APP sources).
 *
 * ★ Why the start phase rather than init:
 *   ① Media-class FS `mount()` needs a block device, and bdev is registered by `adapter/uboot-mmc`
 *      in **its** start phase; APP starts run only after all non-APP starts (plugin manager phase
 *      rules) => this is the earliest it is available.
 *   ② Mount point order: generation already sorts by `order` ("/" first), otherwise child mount
 *      points have no parent FS to attach to (-ENODEV).
 */
int hello_start(void)
{
    br_log_info("%s %s -- core MainLoop (POSIX threads + interrupt heartbeat + logging)",
                BR_PROTOTYPE_NAME, BR_VERSION_STRING);
    br_log_info("clock: %lu Hz (arch timer), %lu ticks/ms (exact integer conversion)",
                br_clock_freq_hz(), br_clock_ticks_per_ms());

    /* ① Execute this APP's declared mount plan first — only then is there a namespace to see */
    if (hello_mount_plan_apply() != 0) {
        return -1;
    }

    /*
     * ★ The media-class FS end-to-end samples run **here** (not in an APP thread): only one control
     *   flow exists, so they cannot concurrently touch the same volumes/card as the plugin selftest
     *   pass that starts right after start. Rationale and measurements are in the app_fs_conformance
     *   comment.
     */
    app_fs_conformance();

    pthread_t app = BR_NULL;
    const int rc = pthread_create(&app, BR_NULL, hello_mainloop, BR_NULL);
    if (rc != 0) {
        br_log_error("app: pthread_create(APP thread) failed rc=%d", rc);
        return rc;
    }
    (void)pthread_setname_np(app, "app-main");
    br_log_info("app: APP thread created; handed to br_sched_run() for the first schedule");
    return 0;
}
