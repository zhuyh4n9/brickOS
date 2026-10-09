/*
 * brickOS prototype v0.2.0 — APP 插件 `app/hello`: POSIX 多线程样例
 *
 * 本件是 **APP 类别的样例**: 它**只经 Interface 皮肤**取用下层 —— POSIX 面(线程/同步/
 * 延时)走 `iface/posix`(`#include <iface/posix/posix.h>`), core native 面(日志/时钟/
 * tick/版本)走 `iface/min`(`#include <iface/min/min.h>`)—— 设计 `1-01` §7.3 / D18 / A-2 的
 * "APP 只依赖 Interface"; 两条声明边见 plugin.toml。**源码里不出现 `<br/core/...>`**:
 * APP 不直连 iface 层以下的接口(ADR-0020, 由 `brickie check`/`build` 执法)。
 *
 * 三个生命周期钩子(名字 = `symbol_prefix` + 相, 由 `build/gen/app/hello/plugin_desc.c` 引用):
 *   hello_early_init()  EARLY 相: 无动作(此相不用堆/无线程/关中断)。
 *   hello_init()        CORE 相: 放开日志等级。
 *   hello_start()       START 相(全局开中断后, **最后**一个 start): 创建 APP 线程, 返回 0;
 *                       管理器随后 `br_sched_run()` 首次调度 ⇒ APP 线程接管 MainLoop。
 *
 * ## 本样例演示什么
 *   `hello_mainloop()` 的第一件事是 **`app_pthread_conformance()`**: 一段自判红绿的
 *   多线程一致性用例(每项打 `[APPCONF] PASS/FAIL`, 末尾 `[APPCONF] SUMMARY`),
 *   覆盖: barrier 起跑 / mutex 保护计数 / join 搬 retval / condvar 会合 / TLS key 隔离
 *   与析构 / detach(惰性回收) / 每线程 errno / rwlock 读并发写独占 / once / 线程名。
 *   之后进入周期心跳循环(每秒一行 `tick=… irq_ticks=…`), 由 `smoke` 门禁判"不早醒"。
 *
 * ## 与 core 的关系
 *   线程/同步/延时全走 POSIX(`iface/posix`); 日志与时钟读数走 `iface/min`(core native
 *   的直通皮肤)。两处都**不是** APP 直连下层: 名字由皮肤转出, 声明边只落在 Interface。
 *
 * ★ 为什么 APP 不自己碰中断控制(P-IRQ-17): 中断控制归 P3, APP 是 P0 ⇒ 只能**读**心跳
 *   计数(`br_clock_tick_count()`, 由 platform 的 ISR 每拍通知 core; ADR-0016)。
 *
 * ★ WORKAROUNDS: br-wa-boot-001 ①② 已还清(ADR-0008/0016); ③(日志直写 console)的标记
 *   在 `core/src/log.c`。
 */
#include <hello/hello.h>

/* core native 面(日志/时钟/tick/版本)**经 Interface 皮肤**进来(ADR-0020)。
 * `iface/min` 是直通 core 的极简别名皮肤: 头文件把 core 头原样转出, 实现仍是 core 的
 * 一份。声明边 = plugin.toml 的 `[[dep]] iface/min`; 于是本件源码里**不出现**
 * `#include <br/core/...>` —— APP 不直连 iface 层以下的接口。 */
#include <iface/min/min.h>

/* POSIX 面**经 Interface 皮肤**进来(设计 D18 / §7.3: APP 只依赖 Interface)。
 * `iface/posix` 是零状态皮肤, 它再导出 `runtime/posix#posix` ⇒ `usleep`、`pthread_*`、
 * `sem_*`、`errno` 的名字在这里就位, 实现仍只有 runtime/posix 一份。
 * 声明边 = plugin.toml 的 `[[dep]] iface/posix`; 编译期可见性 = 声明依赖闭包(ADR-0018)。 */
#include <iface/posix/posix.h>

/* MainLoop 周期。选 1s 是因为它同时是"人能看清的节奏"与"计时误差能被日志一眼量化"的长度。 */
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
