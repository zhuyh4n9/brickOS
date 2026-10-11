#include <hello/hello.h>
#include <iface/min/min.h>
#include <iface/posix/posix.h>

#define BR_MAINLOOP_PERIOD_MS   1000u

/* ------------------------------------------------------------------ APPCONF 装置 */

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

/* ------------------------------------------------------------------ 用例 ①:
 * mutex 保护计数 + barrier 起跑 + join 搬 retval。 */
static pthread_barrier_t s_start_bar;      /* APP_WORKERS + 主线程 */
static pthread_mutex_t   s_count_lock = PTHREAD_MUTEX_INITIALIZER;
static long              s_count;

static void *worker_count(void *arg)
{
    const long bump = (long)(br_intptr_t)arg;
    (void)pthread_barrier_wait(&s_start_bar);       /* 三个 worker 同一起跑线 */
    for (int i = 0; i < APP_ITERS; i++) {
        if (pthread_mutex_lock(&s_count_lock) != 0) {
            return (void *)(br_intptr_t)(-1);
        }
        s_count++;
        if (pthread_mutex_unlock(&s_count_lock) != 0) {
            return (void *)(br_intptr_t)(-2);
        }
    }
    return (void *)(br_intptr_t)bump;               /* retval 经 join 回主线程 */
}

static br_bool case_mutex_count(void)
{
    pthread_t t[APP_WORKERS];
    s_count = 0;
    br_bool ok = (pthread_barrier_init(&s_start_bar, BR_NULL, APP_WORKERS + 1u) == 0);
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        ok = ok && (pthread_create(&t[i], BR_NULL, worker_count, (void *)(br_intptr_t)(i + 1u)) == 0);
    }
    (void)pthread_barrier_wait(&s_start_bar);        /* 放开起跑线 */
    long sum = 0;
    for (unsigned i = 0u; i < APP_WORKERS; i++) {
        void *ret = BR_NULL;
        ok = ok && (pthread_join(t[i], &ret) == 0);
        sum += (long)(br_intptr_t)ret;               /* retval 之和 = 1+2+3 */
    }
    ok = ok && (sum == 6) && (s_count == (long)(APP_WORKERS * (unsigned)APP_ITERS));
    ok = ok && (pthread_barrier_destroy(&s_start_bar) == 0);
    return ok;
}

/* ------------------------------------------------------------------ 用例 ②:
 * condvar 会合(主线程等全部 worker 就绪, 再广播放行)。 */
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

/* ------------------------------------------------------------------ 用例 ③:
 * TLS key: 每线程值隔离 + 退出时析构。 */
static pthread_key_t s_tls_key;
static int           s_tls_dtors;

static void tls_dtor(void *v) { s_tls_dtors += (int)(br_intptr_t)v; }

static void *worker_tls(void *arg)
{
    const int idx = (int)(br_intptr_t)arg;
    if (pthread_setspecific(s_tls_key, (void *)(br_intptr_t)(idx + 1)) != 0) {
        return (void *)(br_intptr_t)(-1);
    }
    (void)pthread_yield();                           /* 让别的线程也写它自己的槽位 */
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

/* ------------------------------------------------------------------ 用例 ④:
 * detach(惰性回收): detached 线程照跑; join detached ⇒ EINVAL。 */
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
    ok = ok && (pthread_join(t, &r) == EINVAL);      /* 记录还在 ⇒ EINVAL(不是 ESRCH) */
    ok = ok && (sem_post(&s_det_gate) == 0);
    for (int i = 0; i < 8; i++) {
        (void)pthread_yield();                       /* 让它退出; 后续调用触发惰性回收 */
    }
    ok = ok && (s_det_ran == 1);
    ok = ok && (sem_destroy(&s_det_gate) == 0);
    return ok;
}

/* ------------------------------------------------------------------ 用例 ⑤:
 * 每线程 errno 隔离(两线程各写各的槽位, 用 barrier 保证写入先于读取)。 */
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

/* ------------------------------------------------------------------ 用例 ⑥:
 * rwlock 读并发 / 写独占。 */
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
        (void)usleep(5000u);                         /* 持读锁睡 ⇒ 另一个读者能同时进 */
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
            s_rw_bad = 1;                            /* 写者持锁时不许有活跃读者 */
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

/* ------------------------------------------------------------------ 用例 ⑦:
 * once(恰一次)+ 线程名 set/get。 */
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

/* ------------------------------------------------------------------ 用例 ⑧–⑭:
 * 介质类文件系统(第三方 FAT / EROFS 移植件)的端到端样例(ADR-0022)。
 *
 * ★ **只 include POSIX 头、只调 POSIX 面**(经 iface/posix 皮肤): 这一族的价值恰在于
 *   "APP 眼里没有第三个文件系统" —— 它看见的只是 `/fat12`、`/fat16`、`/fat32`、`/assets`
 *   四个挂载点上的普通 open/read/write/stat/opendir。挂载表、块设备、SDHCI、LZ4 解压
 *   全在下面若干层, APP 一行都不知道。
 * ★ 判据是**逐字节**的: 宿主用外部权威工具(mkfs.fat + mcopy / mkfs.erofs)把这些字节
 *   放进卷里(build/emmc0..3.img, 见 tools/mk-fs-images.sh)—— 自研 mkfs 与自研 reader
 *   互为镜像证明不了任何东西, 所以"读出来的字节 == 外部工具放进去的字节"是这一族的根判据。
 * ★ 写面用例**自己收摊**(先 unlink 再建、用完 unlink/rmdir): 门禁可能多次启动同一镜像,
 *   留垃圾会让下一次的目录断言漂移。
 */

/* 分块比对的缓冲: 512 B 是有意的 —— APP 线程栈是 4 KiB 级, 最大的被判据文件是 70000 B
 * (EROFS 的 LZ4 压缩件), 不可能整个读进栈里。 */
#define FS_CHUNK 512u
static br_u8 s_fs_buf[FS_CHUNK];

/* 期望字节生成式(id 与 tools/mk-fs-images.sh 的内容生成一一对应) */
#define FSGEN_CONST   0   /* 常量字符 `ch` */
#define FSGEN_ABCDEF  1   /* "ABCDEFGH" 无限重复(EROFS 的 LZ4 压缩件) */
#define FSGEN_IDX     2   /* 第 i 字节 = i & 0xFF */
#define FSGEN_PRNG    3   /* 与造镜像脚本同一个线性同余式(EROFS 的 raw.bin) */

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

/* 读满 `len` 字节并逐字节比对; 还要求"恰好在此处 EOF"(再读一字节必须得 0)。 */
static br_bool fs_check_read(const char *path, br_u64 len, int gen, char ch)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        br_log_info("[APPCONF]   (open %s 失败 errno=%d)", path, errno);
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
        if (read(fd, &tail, 1u) != 0) {   /* 文件应当恰好在 len 处结束 */
            ok = BR_FALSE;
        }
    }
    (void)close(fd);
    return ok;
}

/* 逐字节比对一个常量串(不走生成式路径, 判据更直白: 出错时日志能看出差在哪)。 */
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

/* APP-FS-001: 四个挂载点都在, hello.txt 的类型/大小/内容逐字节正确(未压缩路径) */
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

/* APP-FS-002: 各卷的 dir/a.bin 按生成式逐字节比对(跨块/大文件读) */
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

/* APP-FS-003: FAT 写闭环(建 → 三段写 → fsync → 关 → 重开读回 → stat → 删) */
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

/* APP-FS-004: FAT 目录面(mkdir → 在其中建文件 → opendir/readdir 看得见 → 收摊) */
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

/* APP-FS-005: FAT 改名 + 截断(改名后旧名不存在; 截断改 size 与内容) */
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

/* APP-FS-006: EROFS 只读执法(写/建目录/删都必须是 EROFS, 而不是别的错) + LZ4 压缩读路径 */
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

    /* 压缩路径: 70000 B 的 "ABCDEFGH" 重复(宿主用 mkfs.erofs -zlz4 造的压缩件) */
    ok = ok && fs_check_read("/assets/comp/lz4.txt", 70000u, FSGEN_ABCDEF, 0);
    /* 另一条压缩路径: 8192 B 近似随机(压不太动的那一类) */
    ok = ok && fs_check_read("/assets/raw.bin", 8192u, FSGEN_PRNG, 0);
    return ok;
}

/* APP-FS-007: VFAT 长文件名(宿主放的是 "long name file.txt", 短名是 LONGNA~1.TXT) */
static br_bool case_fs_longname(void)
{
    const char *path = "/fat32/long name file.txt";
    struct stat st;
    br_bool ok = (stat(path, &st) == 0);
    ok = ok && S_ISREG(st.st_mode) && (st.st_size == 4);
    ok = ok && fs_check_exact(path, "lfn\n");
    return ok;
}

/* ------------------------------------------------------------------ 入口 */

static void app_pthread_conformance(void)
{
    s_app_pass = 0u;
    s_app_fail = 0u;
    br_log_info("[APPCONF] ==== POSIX pthread 多线程一致性(APP 侧; 经 iface/posix) ====");
    ap(case_mutex_count(),     "APP-PT-001", "mutex 保护计数(3×500) + barrier 起跑 + join 搬 retval");
    ap(case_cond_rendezvous(), "APP-PT-002", "condvar 会合: 主线程等全部 worker 就绪后广播放行");
    ap(case_tls_key(),         "APP-PT-003", "pthread_key_*: 每线程值隔离 + 退出析构");
    ap(case_detach(),          "APP-PT-004", "detach: detached 线程照跑; join detached ⇒ EINVAL");
    ap(case_errno_isolated(),  "APP-PT-005", "每线程 errno 隔离(两线程各写各的槽位)");
    ap(case_rwlock(),          "APP-PT-006", "rwlock: 读者并发(active_max>=2)、写者独占");
    ap(case_once_and_name(),   "APP-PT-007", "pthread_once 恰一次 + setname/getname_np");
    br_log_info("[APPCONF] SUMMARY pass=%u fail=%u total=%u", s_app_pass, s_app_fail,
                s_app_pass + s_app_fail);
}

/*
 * 介质类文件系统一致性(ADR-0022)。与上面那套**共用**同一对计数器与同一条 SUMMARY 行 ——
 * 于是门禁的 `[APPCONF] SUMMARY ... fail=0` 一句话覆盖两族用例, 而 tag 逐条点名又把
 * "某一族被裁掉"这件事照旧判红。
 *
 * ★ 为什么本族落在 **start 相**(由 `hello_start` 直接调用)而不是 APP 线程里:
 *   自检 pass 跑在"全部 start 之后、`br_sched_run()` 之前", 而 **timer 抢占在此之前就
 *   已经生效**(ADR-0011 的 IRQ 出口换栈只看"有没有就绪任务", 不看 `br_sched_run()` 是否
 *   被调用过)—— 实测: 第一版把本族放在 APP 线程里, 它立刻与 `[SELFTEST]` 并发, 于是
 *   **两边同时访问同一块卡/同一个卷**: EROFS 侧读到 `bdev read 失败 lba=2 rc=-5`,
 *   eMMC 侧两条用例各等满 2 s 超时(sdhci wait 超时/pending=0), APP 侧 FAT 写闭环也红。
 *   放在 start 相就没有这个并发窗口: 此刻只有启动上下文一条控制流, 且四个卷都已挂好、
 *   中断已开、块设备已注册 —— 本族需要的前提**全部成立**。
 *   (这也把"FS 栈本代不保证多线程并发"这条 v1 边界如实暴露出来: vfs-core / fs/tmpfs /
 *    两件移植件都没有卷级锁, 与"设备 ops 由驱动自锁"是两回事。)
 *
 * WORKAROUND(br-wa-test-001): `APP-FS-*` 与 `APP-PT-*` 一样是**自编号** —— 设计 `6-01`
 * 的用例表里没有"APP 自己的样例"这一族。与其余各族的编号债同一处登记。
 */
static void app_fs_conformance(void)
{
    s_app_pass = 0u;
    s_app_fail = 0u;
    br_log_info("[APPCONF] ==== 介质类文件系统端到端(FAT12/16/32 + EROFS; 经 iface/posix) ====");
    ap(case_fs_hello(),             "APP-FS-001", "四挂载点都在; hello.txt 类型/大小/内容逐字节正确");
    ap(case_fs_binaries(),          "APP-FS-002", "各卷 dir/a.bin 按生成式逐字节比对(跨块读)");
    ap(case_fs_write_roundtrip(),   "APP-FS-003", "FAT 写闭环: 建→三段写→fsync→重开读回→stat→删");
    ap(case_fs_directory(),         "APP-FS-004", "FAT 目录面: mkdir→建文件→opendir/readdir 看得见→收摊");
    ap(case_fs_rename_truncate(),   "APP-FS-005", "FAT 改名(旧名 ENOENT)+ truncate 改 size 与内容");
    ap(case_fs_erofs_readonly_lz4(),"APP-FS-006", "EROFS 只读执法(EROFS)+ LZ4 压缩件逐字节读回");
    ap(case_fs_longname(),          "APP-FS-007", "VFAT 长名 \"long name file.txt\" 可 stat/读");
    br_log_info("[APPCONF] SUMMARY pass=%u fail=%u total=%u", s_app_pass, s_app_fail,
                s_app_pass + s_app_fail);
}

/*
 * APP 线程体 = MainLoop(**不返回**): 先跑一次多线程一致性用例, 再进周期心跳。
 *
 * ⚠ 心跳循环里的"睡眠"用 `usleep`(POSIX; 实现在 runtime/posix, 落到 core 的
 *   `br_task_sleep`)。唤醒精度 = 一拍(`product.toml [kernel].hz` 的倒数, 缺省 200
 *   ⇒ 5 ms; ADR-0017)⇒ 每拍实际约 1.00–1.01 s; 3 秒的 smoke 门禁里 `tick=2` 仍稳。
 *   判据 "delay >= 请求" 是设计 3-01 §2.1 的"不早醒"义务。
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

/* EARLY 相: 不用堆/无线程/关中断 ⇒ APP 没有可做的事(钩子必须在场)。 */
int hello_early_init(void)
{
    return 0;
}

/* CORE 相: 放开日志等级(日志设施已就绪)。 */
int hello_init(void)
{
    br_log_set_level(BR_LOG_DEBUG);
    br_log_info("app: init (CORE 相; POSIX 多线程样例在 start 相起线程)");
    return 0;
}

/*
 * START 相(**最后**一个 start): 设置 APP 线程名 → `pthread_create` 建线程 → 返回 0;
 * 管理器随后 `br_sched_run()` 首次调度, APP 线程接管 MainLoop。
 * ★ 自检不由 APP 驱动(ADR-0010): 各插件 selftest 由 core 在全部 start 之后统一跑;
 *   本文件里的 `[APPCONF]` 是 **APP 自己的多线程样例**, 不是插件自检。
 */
int hello_start(void)
{
    br_log_info("%s %s -- core MainLoop (POSIX threads + interrupt heartbeat + logging)",
                BR_PROTOTYPE_NAME, BR_VERSION_STRING);
    br_log_info("clock: %lu Hz (arch timer), %lu ticks/ms (exact integer conversion)",
                br_clock_freq_hz(), br_clock_ticks_per_ms());

    /*
     * ★ 介质类 FS 的端到端样例在**这里**跑(而不是 APP 线程里): 此刻只有一条控制流,
     *   不会与"start 之后立刻开始"的插件自检 pass 并发访问同一批卷/同一块卡。
     *   理由与实测现象见 app_fs_conformance 的注释。
     */
    app_fs_conformance();

    pthread_t app = BR_NULL;
    const int rc = pthread_create(&app, BR_NULL, hello_mainloop, BR_NULL);
    if (rc != 0) {
        br_log_error("app: pthread_create(APP 线程) 失败 rc=%d", rc);
        return rc;
    }
    (void)pthread_setname_np(app, "app-main");
    br_log_info("app: APP 线程已创建; 交给 br_sched_run() 首次调度");
    return 0;
}
