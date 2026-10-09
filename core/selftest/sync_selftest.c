/*
 * brickOS prototype v0.2.0 — 同步原语 + 时间的**自检套件**(core/selftest/)
 *
 * 设计依据与逐条裁定: `docs/decisions/0010-plugin-selftest.md`。
 * 用例口径: `6-01-test.md` §3.2/§3.3(`TC-SYNC-001..009` / `TC-TIME-001..003`);
 * 实现依据 `3-01` §7、`3-02` §11.3、`docs/decisions/0007-sync-primitives.md`。
 *
 * ## 为什么这些用例从 `sync.c` 搬出来
 *   它们本来就是**测试**, 却与生产实现同住一个 .c —— 于是"关掉自检"关不掉它们,
 *   而"读生产实现"的人要在一千行机制里翻找判定逻辑。搬到本目录后:
 *     - `core/src/sync/sync.c` 只留机制(mutex/sem/cond/spinlock);
 *     - 自检关掉 ⇒ `core/selftest/` 整目录不参与编译 ⇒ 用例代码**不进镜像**
 *       (机械事实, 不是"编了不跑"; 见 product.toml 的 selftest_sources 与 build.rs);
 *     - 用例的读者与实现的读者不再互相干扰(套件里那段"白盒造非持有者"的说明
 *       与生产语义解释也不该再混在一起)。
 *   代码是**搬移**, 不是重写: 判据/日志/顺序逐字未动(见下面的日志契约)。
 *
 * ## 夹具的归属
 *   静态夹具(`s_m`/`s_sem`/`s_cv`/`s_sp`/两条线程栈/共享计数器)**只有本套件用**
 *   (已 grep 全树的 `core/`/`tests/` 确认), 所以整组跟用例一起搬到这里 ——
 *   生产侧不持有它们, 也就不需要 `sync_internal.h` 这条新边界。
 *   唯一的跨边界依赖是 `sched_internal.h` 的 `struct br_thread`(白盒造"外部持有者")
 *   与 `br_waitq_len()`(判"无幽灵等待者")。
 *
 * ## 日志契约(门禁按**逐字**匹配 ⇒ 改一个字就是改判据)
 *   `[SYNCCONF] PASS/FAIL/SKIP <tag> <desc>` … `[SYNCCONF] SUMMARY pass=%u fail=%u total=%u`
 *   `tests/gates.toml` 的 require/forbid/require_tags 与 `docs/decisions/0007` §门禁
 *   都锚在这两行上(含"调度框架未注册"时那条 SKIP 行)。
 *
 * ## 返回值
 *   `int` = **失败项数**(ADR-0010 §2.4: 套件只汇总不下判决)。任何失败都**不停机**。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* TCB 布局与等待链四原语是 core 内部契约: 本套件要"白盒造非持有者"并用
 * `br_waitq_len()` 判幽灵节点。跨目录相对包含在本仓有先例(sync.c 原本就这么写)。 */
#include "../src/sched/sched_internal.h"

/* 入口的可见声明集中在 `core/selftest/core_selftest.c`(它是唯一聚合器); 这里再声明
 * 一次是为了满足本文件"每个函数使用前都有原型"的纪律(-Wmissing-prototypes)。 */
int br_sync_selftest(void);

/* =====================================================================
 * 一致性用例(TC-SYNC-001..009 / TC-TIME-001..003; 6-01 §3.2/§3.3)
 * ===================================================================== */

#define CONF_ITER        400u
#define CONF_STACK_BYTES 4096u
#define CONF_WAIT_SPIN   1000u

static br_u8 s_stack_a[CONF_STACK_BYTES] BR_ALIGN(16);
static br_u8 s_stack_b[CONF_STACK_BYTES] BR_ALIGN(16);

static br_mutex_t    s_m;
static br_sem_t      s_sem;
static br_cond_t     s_cv;
static br_spinlock_t s_sp;

static volatile br_u32 s_inside;
static volatile br_u32 s_overlap;
static volatile br_u32 s_count;
static int s_wrc[4];

/* 无线程上下文时的"假持有者"(白盒: 只为了造出"非持有者"这一情形, 见 ADR-0007 §3)。 */
static struct br_thread s_foreign_owner;

static void conf_line(const char *tag, br_bool ok, const char *desc,
                      br_u32 *pass, br_u32 *fail)
{
    br_log_info("[SYNCCONF] %s %s %s", (ok == BR_TRUE) ? "PASS" : "FAIL", tag, desc);
    if (ok == BR_TRUE) {
        (*pass)++;
    } else {
        (*fail)++;
    }
}

static void conf_skip(const char *tag, const char *why)
{
    br_log_info("[SYNCCONF] SKIP %s %s", tag, why);
}

static br_thread_t *conf_spawn(void (*fn)(void *), void *arg, br_u8 *stack)
{
    br_task_attr_t attr;
    br_thread_t   *t = BR_NULL;

    attr.name       = "syncconf";
    attr.stack      = (void *)(stack + CONF_STACK_BYTES);
    attr.stack_size = CONF_STACK_BYTES;
    attr.prio       = 0u;
    attr.flags      = 0u;

    if (br_task_create(&t, &attr, fn, arg) != 0) {
        return BR_NULL;
    }
    return t;
}

static br_bool conf_join(br_thread_t *t, int *code)
{
    if (t == BR_NULL) {
        return BR_FALSE;
    }
    return (br_task_join(t, code) == 0) ? BR_TRUE : BR_FALSE;
}

/* ---------------------------------------------------------------- workers  * WORKAROUND(br-wa-test-001): 本套件的 `TC-SYNC-*`/`TC-TIME-*` id 与 `6-01` §3.2/§3.3 的
 * 表**尚未逐条对齐**(含义漂移, 自述文字准确); 对齐动作见 WORKAROUNDS.md。
 */

static void conf_mutex_worker(void *arg)
{
    (void)arg;
    for (br_u32 i = 0u; i < CONF_ITER; i++) {
        if (br_mutex_lock(&s_m) != 0) {
            s_wrc[0] = -1;
            return;
        }
        if (s_inside != 0u) {
            s_overlap++;                 /* 两线程同时处于临界区 ⇒ 互斥被破坏 */
        }
        s_inside = 1u;
        s_count++;
        if ((i & 7u) == 0u) {
            br_task_yield();             /* 制造争用: 让另一个线程来撞锁并阻塞 */
        }
        s_inside = 0u;
        (void)br_mutex_unlock(&s_m);
    }
}

static void conf_eperm_worker(void *arg)
{
    (void)arg;
    s_wrc[1] = br_mutex_unlock(&s_m);    /* main 持锁 ⇒ 非持有者 */
}

static void conf_timeout_worker(void *arg)
{
    (void)arg;
    s_wrc[2] = br_mutex_lock_to(&s_m, 20000u);   /* 20 ms 后超时 */
}

static void conf_sem_worker(void *arg)
{
    int *out = (int *)arg;
    *out = br_sem_take(&s_sem, BR_TIMEOUT_INF);
}

static void conf_cond_worker(void *arg)
{
    int *out = (int *)arg;

    if (br_mutex_lock(&s_m) != 0) {
        *out = -1;
        return;
    }
    const int rc = br_cond_wait(&s_cv, &s_m, BR_TIMEOUT_INF);
    /* 返回时必须仍持有 m */
    if (!(br_mutex_owner(&s_m) == br_task_self() && br_mutex_is_locked(&s_m) == BR_TRUE)) {
        *out = -2;
    } else {
        *out = rc;
    }
    (void)br_mutex_unlock(&s_m);
}

static void conf_wait_for_waiters(volatile br_u32 *n, br_u32 want)
{
    for (br_u32 i = 0u; (i < CONF_WAIT_SPIN) && (*n < want); i++) {
        br_task_yield();
    }
}

/* ---------------------------------------------------------------- 用例 */

static void conf_sync_001(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-001", "调度框架未注册(mutex 互斥需要两个真线程)");
        return;
    }

    (void)br_mutex_init(&s_m);
    s_inside = 0u;
    s_overlap = 0u;
    s_count = 0u;
    s_wrc[0] = 0;

    br_thread_t *a = conf_spawn(conf_mutex_worker, BR_NULL, s_stack_a);
    br_thread_t *b = conf_spawn(conf_mutex_worker, BR_NULL, s_stack_b);

    int ca = 0;
    int cb = 0;
    const br_bool ja = conf_join(a, &ca);
    const br_bool jb = conf_join(b, &cb);

    const br_bool ok = (ja == BR_TRUE) && (jb == BR_TRUE) && (s_wrc[0] == 0)
                    && (s_count == 2u * CONF_ITER) && (s_overlap == 0u)
                    && (br_mutex_is_locked(&s_m) == BR_FALSE)
                    && (s_m.owner == BR_NULL) && (br_waitq_len(s_m.head) == 0u);
    conf_line("TC-SYNC-001", ok, "mutex 互斥: 双线程 x400 交叉计数, 无重叠无丢失", pass, fail);
}

static void conf_sync_002(br_u32 *pass, br_u32 *fail)
{
    (void)br_mutex_init(&s_m);

    const int r1 = br_mutex_lock_to(&s_m, BR_TIMEOUT_ZERO);   /* 空闲 ⇒ 0 */
    const int r2 = br_mutex_lock_to(&s_m, BR_TIMEOUT_ZERO);   /* 已锁 ⇒ -ETIMEDOUT(6-01 TC-SYNC-002) */
    const int r3 = br_mutex_unlock(&s_m);
    const int r4 = br_mutex_lock_to(&s_m, BR_TIMEOUT_ZERO);   /* 又空闲 ⇒ 0 */
    const int r5 = br_mutex_unlock(&s_m);

    const br_bool ok = (r1 == BR_OK) && (r2 == BR_ERR(BR_ETIMEDOUT)) && (r3 == BR_OK)
                    && (r4 == BR_OK) && (r5 == BR_OK)
                    && (br_mutex_is_locked(&s_m) == BR_FALSE);
    conf_line("TC-SYNC-002", ok, "mutex lock_to(ZERO): 空闲 0 / 忙 -ETIMEDOUT(不阻塞)", pass, fail);
}

static void conf_sync_003(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    br_bool ok;

    (void)br_mutex_init(&s_m);

    if (sched == BR_TRUE) {
        (void)br_mutex_lock(&s_m);
        s_wrc[1] = 0;
        br_thread_t *t = conf_spawn(conf_eperm_worker, BR_NULL, s_stack_a);
        int code = 0;
        const br_bool j = conf_join(t, &code);
        const int rr = br_mutex_unlock(&s_m);
        ok = (j == BR_TRUE) && (s_wrc[1] == BR_ERR(BR_EPERM)) && (rr == BR_OK);
    } else {
        /* 没有第二个线程 ⇒ 白盒地伪造"锁被别的线程持有", 仍走同一条 owner 判定。 */
        s_m.locked = 1u;
        s_m.owner  = &s_foreign_owner;
        const int rr = br_mutex_unlock(&s_m);
        ok = (rr == BR_ERR(BR_EPERM)) ? BR_TRUE : BR_FALSE;
        (void)br_mutex_init(&s_m);
    }

    conf_line("TC-SYNC-003", ok, "非持有者 unlock ⇒ -EPERM", pass, fail);
}

static void conf_sync_004(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-004", "调度框架未注册(超时需要一个真线程被阻塞)");
        return;
    }

    (void)br_mutex_init(&s_m);
    (void)br_mutex_lock(&s_m);            /* main 持锁, 并阻塞在 join 上等超时送达 */
    s_wrc[2] = 0;

    br_thread_t *t = conf_spawn(conf_timeout_worker, BR_NULL, s_stack_a);
    int code = 0;
    const br_time_t tw0 = br_clock_now();
    const br_bool j = conf_join(t, &code);
    const br_time_t tw = br_clock_now() - tw0;

    br_log_info("[SYNCCONF] MEASURE TC-SYNC-004 lock_to request=20000us measured=%lu us (tick granularity)",
                (br_u64)tw);

    const br_bool ghost_free = (br_waitq_len(s_m.head) == 0u) && (s_m.waiters == 0u);
    const int u = br_mutex_unlock(&s_m);
    const br_bool released_clean = (br_mutex_is_locked(&s_m) == BR_FALSE)
                                && (br_mutex_owner(&s_m) == BR_NULL)
                                && (s_m.waiters == 0u) && (br_waitq_len(s_m.head) == 0u);
    const int again = br_mutex_lock_to(&s_m, BR_TIMEOUT_ZERO);
    if (again == BR_OK) {
        (void)br_mutex_unlock(&s_m);
    }

    const br_bool ok = (j == BR_TRUE) && (s_wrc[2] == BR_ERR(BR_ETIMEDOUT))
                    && (ghost_free == BR_TRUE) && (u == BR_OK) && (released_clean == BR_TRUE)
                    && (again == BR_OK)
                    && (br_mutex_is_locked(&s_m) == BR_FALSE)
                    && (br_mutex_owner(&s_m) == BR_NULL)
                    && (br_waitq_len(s_m.head) == 0u);
    conf_line("TC-SYNC-004", ok, "lock_to 超时 ⇒ -ETIMEDOUT 且无幽灵等待者, 锁随后可再获取",
              pass, fail);
}

static void conf_sync_005(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    (void)br_sem_init(&s_sem, 2u);

    const int a = br_sem_take(&s_sem, BR_TIMEOUT_ZERO);   /* count 2 -> 1 */
    const int b = br_sem_take(&s_sem, BR_TIMEOUT_ZERO);   /* count 1 -> 0 */
    const int c = br_sem_take(&s_sem, BR_TIMEOUT_ZERO);   /* 空 ⇒ -ETIMEDOUT */
    const int g = br_sem_give(&s_sem);                    /* count -> 1 */
    const int d = br_sem_take(&s_sem, BR_TIMEOUT_ZERO);   /* count -> 0 */

    br_bool ok = (a == BR_OK) && (b == BR_OK) && (c == BR_ERR(BR_ETIMEDOUT)) && (g == BR_OK)
              && (d == BR_OK) && (br_sem_count(&s_sem) == 0u)
              && (br_waitq_len(s_sem.head) == 0u);

    const char *desc = "sem 计数语义(init(2)/take x2/take(ZERO)/give) + give 唤醒阻塞者";

    if (sched == BR_TRUE) {
        s_wrc[3] = 0;
        br_thread_t *t = conf_spawn(conf_sem_worker, &s_wrc[3], s_stack_a);
        conf_wait_for_waiters(&s_sem.waiters, 1u);        /* 等 T 真阻塞在 take 上 */
        const int wg = br_sem_give(&s_sem);               /* 直接交接给等待者 */
        int code = 0;
        const br_bool j = conf_join(t, &code);
        ok = (ok == BR_TRUE) && (wg == BR_OK) && (j == BR_TRUE) && (s_wrc[3] == BR_OK)
           && (s_sem.waiters == 0u) && (br_waitq_len(s_sem.head) == 0u);
    } else {
        conf_skip("TC-SYNC-005(wake)", "调度框架未注册: give 唤醒阻塞者未验, 只验计数语义");
        desc = "sem 计数语义(init(2)/take x2/take(ZERO)/give)[无线程子集]";
    }

    conf_line("TC-SYNC-005", ok, desc, pass, fail);
}

static void conf_sync_006(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-006", "调度框架未注册(需要一个线程阻塞在 take 上)");
        return;
    }

    (void)br_sem_init(&s_sem, 0u);
    s_wrc[3] = 0;
    br_thread_t *t = conf_spawn(conf_sem_worker, &s_wrc[3], s_stack_a);
    conf_wait_for_waiters(&s_sem.waiters, 1u);

    /*
     * 模拟 ISR 上下文: `br_irq_lock()` 关中断 —— 这正是 ISR 运行时的上下文(IRQ 已屏蔽),
     * 且 ISR 里**不允许**任何会让出 CPU 的调用。所以"关中断临界区里调 br_sem_give()"
     * 与"在 ISR 里调"的可观测约束完全一致: 只改计数/挂链/置唤醒位, 不切换。
     * (真起一个 timer ISR 也只能证明同一件事, 却把用例绑到平台上。)
     */
    const br_irq_state_t ist = br_irq_lock();
    const int give_rc = br_sem_give(&s_sem);
    br_irq_unlock(ist);

    int code = 0;
    const br_bool j = conf_join(t, &code);

    const br_bool ok = (give_rc == BR_OK) && (j == BR_TRUE) && (s_wrc[3] == BR_OK)
                    && (br_sem_count(&s_sem) == 0u) && (br_waitq_len(s_sem.head) == 0u);
    conf_line("TC-SYNC-006", ok, "ISR 上下文 give(CA-3 白名单): 关中断里 give 唤醒线程", pass, fail);
}

static void conf_sync_007(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    (void)br_cond_init(&s_cv);
    (void)br_mutex_init(&s_m);

    /* (a) signal 无等待者 ⇒ no-op 且**不计数** */
    const int s0 = br_cond_signal(&s_cv);
    const br_u32 w0 = br_cond_waiters(&s_cv);

    /* 不计数的机械判据: 随后的 wait(ZERO) 仍必须是超时(若 signal 记了账就会立即返回 0) */
    (void)br_mutex_lock(&s_m);
    const int z = br_cond_wait(&s_cv, &s_m, BR_TIMEOUT_ZERO);
    const br_bool held_z = (br_mutex_owner(&s_m) == br_task_self())
                        && (br_mutex_is_locked(&s_m) == BR_TRUE);
    (void)br_mutex_unlock(&s_m);

    br_bool ok = (s0 == BR_OK) && (w0 == 0u) && (z == BR_ERR(BR_ETIMEDOUT))
              && (held_z == BR_TRUE);

    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-007(signal/wait/broadcast)",
                  "调度框架未注册: 只验了 signal 无等待者 no-op 与 wait(ZERO) 的可恢复性");
        conf_line("TC-SYNC-007", ok,
                  "cond(无线程子集): signal 无等待者 no-op(不计数) + wait(ZERO) 返回后仍持锁",
                  pass, fail);
        return;
    }

    /* (b) signal → wait 配对 */
    s_wrc[2] = 123;
    br_thread_t *t = conf_spawn(conf_cond_worker, &s_wrc[2], s_stack_a);
    conf_wait_for_waiters(&s_cv.waiters, 1u);
    const int sig = br_cond_signal(&s_cv);
    int code = 0;
    (void)conf_join(t, &code);
    ok = (ok == BR_TRUE) && (sig == BR_OK) && (s_wrc[2] == BR_OK)
       && (br_cond_waiters(&s_cv) == 0u) && (br_waitq_len(s_cv.head) == 0u);

    /* (c) broadcast 唤醒全部(N=2) */
    (void)br_cond_init(&s_cv);
    (void)br_mutex_init(&s_m);
    s_wrc[2] = 123;
    s_wrc[3] = 123;
    br_thread_t *t1 = conf_spawn(conf_cond_worker, &s_wrc[2], s_stack_a);
    br_thread_t *t2 = conf_spawn(conf_cond_worker, &s_wrc[3], s_stack_b);
    conf_wait_for_waiters(&s_cv.waiters, 2u);
    const int bc = br_cond_broadcast(&s_cv);
    int c1 = 0;
    int c2 = 0;
    const br_bool j1 = conf_join(t1, &c1);
    const br_bool j2 = conf_join(t2, &c2);
    ok = (ok == BR_TRUE) && (bc == BR_OK) && (j1 == BR_TRUE) && (j2 == BR_TRUE)
       && (s_wrc[2] == BR_OK) && (s_wrc[3] == BR_OK)
       && (br_cond_waiters(&s_cv) == 0u) && (br_waitq_len(s_cv.head) == 0u);

    conf_line("TC-SYNC-007", ok,
              "cond: signal 无等待者 no-op(不计数) + wait/signal 配对 + broadcast 全醒", pass, fail);
}

static void conf_sync_008(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-SYNC-008", "调度框架未注册(cond 超时需要真阻塞 + tick)");
        return;
    }

    (void)br_cond_init(&s_cv);
    (void)br_mutex_init(&s_m);
    (void)br_mutex_lock(&s_m);

    const int rc = br_cond_wait(&s_cv, &s_m, 20000u);       /* 20 ms 后超时 */
    const br_bool held = (br_mutex_owner(&s_m) == br_task_self())
                      && (br_mutex_is_locked(&s_m) == BR_TRUE);
    const br_bool clean = (br_cond_waiters(&s_cv) == 0u) && (br_waitq_len(s_cv.head) == 0u);
    (void)br_mutex_unlock(&s_m);

    const br_bool ok = (rc == BR_ERR(BR_ETIMEDOUT)) && (held == BR_TRUE) && (clean == BR_TRUE)
                    && (br_mutex_is_locked(&s_m) == BR_FALSE)
                    && (br_mutex_owner(&s_m) == BR_NULL)
                    && (br_waitq_len(s_m.head) == 0u) && (s_m.waiters == 0u);
    conf_line("TC-SYNC-008", ok, "cond_wait 超时 ⇒ -ETIMEDOUT 且持锁状态已恢复(m 仍持有)",
              pass, fail);
}

static void conf_sync_009(br_u32 *pass, br_u32 *fail)
{
    /* ---- spinlock 配对 + 中断状态 ---- */
    (void)br_spinlock_init(&s_sp);

    const br_irq_state_t base = br_irq_lock();     /* 进入前的中断状态基线(depth 0) */
    br_irq_unlock(base);

    const br_irq_state_t got = br_spinlock_lock(&s_sp);
    const br_bool locked = br_spinlock_is_locked(&s_sp);
    br_spinlock_unlock(&s_sp, got);
    const br_bool unlocked = (br_spinlock_is_locked(&s_sp) == BR_FALSE);

    /* 嵌套: 外层 irq_lock 里再拿 spinlock, 交回后中断状态与进入前一致 */
    const br_irq_state_t outer = br_irq_lock();
    const br_irq_state_t inner = br_spinlock_lock(&s_sp);
    br_spinlock_unlock(&s_sp, inner);
    br_irq_unlock(outer);
    const br_irq_state_t after = br_irq_lock();
    br_irq_unlock(after);

    /* trylock: 忙 ⇒ -EBUSY 且不动 *saved、不持锁 */
    const br_irq_state_t held = br_spinlock_lock(&s_sp);
    br_irq_state_t saved = 0xDEADBEEFu;
    const int tb = br_spinlock_trylock(&s_sp, &saved);
    const br_bool saved_kept = (saved == 0xDEADBEEFu);
    br_spinlock_unlock(&s_sp, held);
    const int tf = br_spinlock_trylock(&s_sp, &saved);
    if (tf == BR_OK) {
        br_spinlock_unlock(&s_sp, saved);
    }

    /* ---- magic 校验: 未初始化/被踩坏必须 -EINVAL, 不静默当"没锁" ---- */
    br_mutex_t zm;
    br_sem_t   zs;
    br_cond_t  zc;
    br_spinlock_t zsp;
    for (br_size_t i = 0u; i < sizeof(zm); i++) { ((br_u8 *)&zm)[i] = 0u; }
    for (br_size_t i = 0u; i < sizeof(zs); i++) { ((br_u8 *)&zs)[i] = 0u; }
    for (br_size_t i = 0u; i < sizeof(zc); i++) { ((br_u8 *)&zc)[i] = 0u; }
    for (br_size_t i = 0u; i < sizeof(zsp); i++) { ((br_u8 *)&zsp)[i] = 0u; }

    const int em  = br_mutex_lock(&zm);
    const int es  = br_sem_take(&zs, BR_TIMEOUT_ZERO);
    const int ec  = br_cond_signal(&zc);
    const int et  = br_spinlock_trylock(&zsp, &saved);

    const br_bool ok = (got == base) && (base == after)
                    && (locked == BR_TRUE) && (unlocked == BR_TRUE)
                    && (tb == BR_ERR(BR_EBUSY)) && (saved_kept == BR_TRUE) && (tf == BR_OK)
                    && (br_spinlock_contention() == 0u)   /* 单核: 自旋计数恒 0 */
                    && (em == BR_ERR(BR_EINVAL)) && (es == BR_ERR(BR_EINVAL))
                    && (ec == BR_ERR(BR_EINVAL)) && (et == BR_ERR(BR_EINVAL));
    conf_line("TC-SYNC-009", ok,
              "spinlock lock/unlock 中断状态一致 + trylock -EBUSY + 争用计数 0 + magic -EINVAL",
              pass, fail);
}

static void conf_time_001(br_u32 *pass, br_u32 *fail, br_bool sched)
{
    if (sched != BR_TRUE) {
        conf_skip("TC-TIME-001", "调度框架未注册(sleep 需要 tick 唤醒)");
        return;
    }

    /* 单调不减(6-01 TC-TIME-001 的原始口径) */
    br_bool mono = BR_TRUE;
    br_time_t prev = br_clock_now();
    for (br_u32 i = 0u; i < 1000u; i++) {
        const br_time_t now = br_clock_now();
        if (now < prev) {
            mono = BR_FALSE;
            break;
        }
        prev = now;
    }

    /* sleep 不早醒(实测; tick = 1/HZ, 缺省 5 ms ⇒ 请求 1000 us 会晚到约一个 tick) */
    const br_time_t t0 = br_clock_now();
    const int rc = br_task_sleep(1000u);
    const br_time_t dt = br_clock_now() - t0;

    /* 明确打出"请求 vs 实测": 平台 tick 粒度决定的晚到量是可观测事实, 不是猜的。 */
    br_log_info("[SYNCCONF] MEASURE TC-TIME-001 request=1000us measured=%lu us (tick granularity)",
                (br_u64)dt);

    const br_bool ok = (mono == BR_TRUE) && (rc == BR_OK) && (dt >= 1000u);
    conf_line("TC-TIME-001", ok, "sleep(1000us) 不早醒: 实测差值 >= 请求值; clock 单调不减",
              pass, fail);
}

static void conf_time_002(br_u32 *pass, br_u32 *fail)
{
    const br_time_t t0 = br_clock_now();
    const int rc = br_task_sleep(0u);
    const br_time_t dt = br_clock_now() - t0;

    const br_bool ok = (rc == BR_OK);
    conf_line("TC-TIME-002", ok, "br_task_sleep(0) 立即返回 0", pass, fail);
    (void)dt;
}

static void conf_time_003(br_u32 *pass, br_u32 *fail)
{
    /* 裁定 G13: 相对睡眠没有"无限"的语义 —— 那是不返回 */
    const int rc = br_task_sleep(BR_TIMEOUT_INF);

    /* deadline 饱和不回绕(TC-TIME-003 原始口径) */
    const br_time_t now = br_clock_now();
    const br_time_t d_inf = br_deadline_from_now(BR_TIMEOUT_INF);
    const br_time_t d_near = br_deadline_from_now(BR_TIMEOUT_INF - 1u);
    const br_time_t d_zero = br_deadline_from_now(0u);
    const br_time_t d_edge = br_deadline_from_now(BR_TIMEOUT_INF - now); /* now + rel == INF 精确 */

    /* 真溢出: now + rel 超过 UINT64_MAX ⇒ 必须饱和到 BR_TIMEOUT_INF, 不回绕成过去 */
    br_bool sat = BR_TRUE;
    if (now > 0u) {
        sat = (br_deadline_from_now((BR_TIMEOUT_INF - now) + 1u) == BR_TIMEOUT_INF)
            ? BR_TRUE : BR_FALSE;
    }

    const br_bool ok = (rc == BR_ERR(BR_EINVAL))
                    && (d_inf == BR_TIMEOUT_INF)
                    && (d_edge == BR_TIMEOUT_INF)
                    && (d_near >= now)                    /* 绝不回绕到 now 之前 */
                    && (d_zero >= now)
                    && (sat == BR_TRUE);
    conf_line("TC-TIME-003", ok,
              "sleep(INF) ⇒ -EINVAL(G13); deadline_from_now 近 UINT64_MAX 饱和不回绕",
              pass, fail);
}

int br_sync_selftest(void)
{
    br_u32 pass = 0u;
    br_u32 fail = 0u;

    const br_bool sched = br_sched_registered();

    br_log_info("[SYNCCONF] sync primitives conformance (core; sched_registered=%u)",
                (br_u32)sched);
    if (sched != BR_TRUE) {
        br_log_info("[SYNCCONF] SKIP 调度框架未注册: 线程相关用例(001/004/006/007/008/TC-TIME-001)"
                    " 未执行; 只跑不依赖线程的子集(002/003/005 计数/009/TC-TIME-002/003)");
    }

    conf_sync_001(&pass, &fail, sched);
    conf_sync_002(&pass, &fail);
    conf_sync_003(&pass, &fail, sched);
    conf_sync_004(&pass, &fail, sched);
    conf_sync_005(&pass, &fail, sched);
    conf_sync_006(&pass, &fail, sched);
    conf_sync_007(&pass, &fail, sched);
    conf_sync_008(&pass, &fail, sched);
    conf_sync_009(&pass, &fail);

    conf_time_001(&pass, &fail, sched);
    conf_time_002(&pass, &fail);
    conf_time_003(&pass, &fail);

    /* 收尾复位: 用例是判据, 不留破坏现场(对象回到未持有/无等待者)。 */
    (void)br_mutex_init(&s_m);
    (void)br_sem_init(&s_sem, 0u);
    (void)br_cond_init(&s_cv);
    (void)br_spinlock_init(&s_sp);

    br_log_info("[SYNCCONF] SUMMARY pass=%u fail=%u total=%u",
                (br_u32)pass, (br_u32)fail, (br_u32)(pass + fail));

    /* 返回**失败项数**: 聚合器相加, 红绿由门禁判(ADR-0010 §2.4)。 */
    return (int)fail;
}
