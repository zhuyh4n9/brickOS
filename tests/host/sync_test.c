/*
 * tests/host/sync_test.c — 宿主侧同步原语语义用例(设计 6-01 §3.2/§3.3 的 TC-SYNC/TC-TIME)
 *
 * ## 为什么宿主侧是主判据
 *
 * mutex/sem/cond 的**难点全在阻塞路径**: "挂等待链 → 让出 CPU → 被唤醒/超时 → 摘链 →
 * 重新加锁/交接所有权"。只测无锁快路径等于没测。宿主上我们用一个**假调度器**把
 * `br_sched_block_current()`/`br_sched_wake()`/超时扫描真的实现出来 —— 阻塞、唤醒、
 * 超时三态于是**真的跑一遍**, 而不是查数据结构。
 *
 * ## 测试装置的边界(与 ADR-0007 §2.7 对应)
 *
 * - 宿主的上下文切换用 `ucontext.h` 的 `swapcontext()`。它是**测试装置**, 不是第二份
 *   目标实现 —— 目标侧切换是 `core/src/sched/` 的 asm, 宿主侧只需要"能阻塞/能被唤醒"
 *   这一件事; 在 x86 宿主上手写一遍 aarch64 切换汇编毫无意义。
 * - 被测文件 = `core/src/sync/sync.c`(同步原语)+ `core/src/time.c`(br_deadline_from_now)
 *   + `core/src/string.c`(编译器的支持例程)。**不**编 `core/src/sched/sched_core.c`
 *   (它含 `wfi` 内联汇编, 宿主编不过): 等待链四原语与 TCB 布局在宿主侧按
 *   `sched_internal.h` 的**冻结契约**原样重实现(它就是 30 行单链表, 不是算法)。
 * - 假调度器**严格遵守** sync.c 依赖的握手协议(见 sync.c 头注的 ICB 协议):
 *   `br_sched_wake()` 先写 `wait_status=0`; `br_sched_block_current()` 看到
 *   `wait_status==0` 就不切走; 超时只对 `state==BLOCKED` 的线程生效。
 *
 * ## 输出协议
 *
 *   [SYNCCONF] PASS/FAIL/SKIP <tag> <desc>     (与 target 侧同一份 sync.c 打出)
 *   [HOSTTEST] PASS/FAIL <tag> <desc>
 *   [HOSTTEST] SUMMARY pass=N fail=0 total=N
 * 有失败 ⇒ 退出码非 0。
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include <br/platform/br_plat.h>

#include "sched_internal.h"

/*
 * 自检套件入口的前置声明。ADR-0010 之后它**不再**出现在 `br_sync.h` —— 自检入口不是
 * 插件的对外能力, 留在 golden 面会让"改一个用例"变成接口变更。声明集中在
 * `core/selftest/core_selftest.c`; 本文件是调用者, 于是自带一份(实现随套件在
 * `core/selftest/sync_selftest.c`, 由 gates.toml 的 sync-test sources 一起编)。
 */
int br_sync_selftest(void);

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/* =====================================================================
 * 用例框架
 * ===================================================================== */

static int s_pass;
static int s_fail;

#define CHECK(cond, tag, desc)                                          \
    do {                                                                \
        if ((cond) != 0) {                                              \
            s_pass++;                                                   \
            printf("[HOSTTEST] PASS %s %s\n", (tag), (desc));           \
        } else {                                                        \
            s_fail++;                                                   \
            printf("[HOSTTEST] FAIL %s %s\n", (tag), (desc));           \
        }                                                               \
    } while (0)

static void section(const char *tag, const char *desc)
{
    printf("[HOSTTEST] ---- %s %s\n", tag, desc);
}

/* =====================================================================
 * 假平台时钟(time.c 的宿主替身): 1 MHz ⇒ 1 tick == 1 us(虚拟时钟, 便于精确判据)
 * ===================================================================== */

#define FAKE_TICKS_HZ 1000000u

static br_u64 g_ticks;

br_u64 br_plat_ticks_freq(void)
{
    return FAKE_TICKS_HZ;
}

br_u64 br_plat_ticks_now(void)
{
    return g_ticks;
}

/* panic 的宿主替身(只有"魔数被踩坏"的 spinlock 路径会走到这里)。 */
BR_NORETURN void br_panic_bare(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    (void)fputs("[HOSTTEST] PANIC ", stderr);
    (void)vfprintf(stderr, fmt, ap);
    (void)fputc('\n', stderr);
    va_end(ap);
    abort();
}

/* =====================================================================
 * 假 L2 中断锁(br_irq_lock/unlock 的宿主替身)
 *
 * 宿主没有可屏蔽的中断; 但调度是**协作式**的(只有显式 block/yield 才切换), 所以
 * "关中断临界区里不切换"这一条在宿主上同样成立。状态编码与真实现同构(I 位 + depth),
 * 于是 spinlock 的配对用例在宿主上同样有意义。
 * ===================================================================== */

static br_u32 g_irq_depth;

br_irq_state_t br_irq_lock(void)
{
    br_irq_state_t st = (br_irq_state_t)(g_irq_depth << BR_IRQ_ST_DEPTH_SHIFT);

    if (g_irq_depth != 0u) {
        st |= BR_IRQ_ST_I_MASK;      /* 已经关着了 ⇒ "进入前 I 位 = 1" */
    }
    g_irq_depth++;
    return st;
}

void br_irq_unlock(br_irq_state_t st)
{
    (void)st;
    if (g_irq_depth == 0u) {
        return;                      /* 下溢护栏(与真实现同纪律) */
    }
    g_irq_depth--;
}

/* =====================================================================
 * 等待链四原语(按 sched_internal.h 的冻结契约原样重实现; 见文件头注)
 * ===================================================================== */

void br_waitq_push(void **head, br_thread_t *t)
{
    if (head == NULL || t == NULL) {
        return;
    }
    t->wait_next = NULL;
    if (*head == NULL) {
        *head = t;
        return;
    }
    br_thread_t *p = (br_thread_t *)*head;
    while (p->wait_next != NULL) {
        p = p->wait_next;
    }
    p->wait_next = t;
}

br_thread_t *br_waitq_pop(void **head)
{
    if (head == NULL || *head == NULL) {
        return NULL;
    }
    br_thread_t *t = (br_thread_t *)*head;
    *head = t->wait_next;
    t->wait_next = NULL;
    return t;
}

int br_waitq_remove(void **head, br_thread_t *t)
{
    if (head == NULL || t == NULL) {
        return 0;
    }
    br_thread_t *prev = NULL;
    br_thread_t *p = (br_thread_t *)*head;

    while (p != NULL) {
        if (p == t) {
            if (prev == NULL) {
                *head = p->wait_next;
            } else {
                prev->wait_next = p->wait_next;
            }
            p->wait_next = NULL;
            return 1;
        }
        prev = p;
        p = p->wait_next;
    }
    return 0;
}

br_u32 br_waitq_len(const void *head)
{
    br_u32 n = 0u;
    const br_thread_t *p = (const br_thread_t *)head;

    while (p != NULL) {
        n++;
        p = p->wait_next;
    }
    return n;
}

void *br_thread_priv(br_thread_t *t)
{
    return (void *)((br_u8 *)t + sizeof(struct br_thread));
}

/* =====================================================================
 * 假调度器(ucontext)
 * ===================================================================== */

#define FAKE_MAX          12u
#define FAKE_STACK_BYTES  (128u * 1024u)
#define FAKE_RQ_CAP       (FAKE_MAX * 4u)

static struct br_thread g_tcb[FAKE_MAX];
static ucontext_t      g_uc[FAKE_MAX];
static ucontext_t      g_sched_uc;
static ucontext_t      g_boot_uc;
static br_u8           g_tstack[FAKE_MAX][FAKE_STACK_BYTES] BR_ALIGN(16);
static br_u8           g_sched_stack[64u * 1024u] BR_ALIGN(16);

static br_thread_t *g_current;
static br_thread_t *g_join_waiter[FAKE_MAX];
static br_thread_t *g_rq[FAKE_RQ_CAP];
static br_u32       g_rq_head;
static br_u32       g_rq_tail;
static br_u32       g_rq_len;

static br_u32 tcb_index(const br_thread_t *t)
{
    return (br_u32)(t - &g_tcb[0]);
}

static br_bool tcb_live(const struct br_thread *t)
{
    return (t->magic == BR_THREAD_MAGIC) ? BR_TRUE : BR_FALSE;
}

static void rq_push(br_thread_t *t)
{
    if (g_rq_len >= FAKE_RQ_CAP) {
        printf("[HOSTTEST] FAIL SCHED 就绪队列溢出\n");
        exit(2);
    }
    g_rq[g_rq_tail % FAKE_RQ_CAP] = t;
    g_rq_tail++;
    g_rq_len++;
}

static br_thread_t *rq_pop(void)
{
    if (g_rq_len == 0u) {
        return NULL;
    }
    br_thread_t *t = g_rq[g_rq_head % FAKE_RQ_CAP];
    g_rq_head++;
    g_rq_len--;
    return t;
}

static br_bool sched_all_done(void)
{
    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_TRUE && g_tcb[i].state != BR_TASK_ZOMBIE) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

/*
 * 超时扫描: 没有就绪线程时, 把虚拟时钟推到"最近的期限", 并把所有到期的 BLOCKED
 * 线程置 -ETIMEDOUT + READY。**只对 BLOCKED 生效** —— 已 READY/已被唤醒的线程不再超时,
 * 这是"超时 vs 所有权交接"不出双赢家的关键(与真实现的 tick 扫描同一纪律)。
 */
static br_bool advance_timeouts(void)
{
    br_time_t best = BR_TIMEOUT_INF;

    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_TRUE && g_tcb[i].state == BR_TASK_BLOCKED
            && g_tcb[i].wake_at != BR_TIMEOUT_INF && g_tcb[i].wake_at < best) {
            best = g_tcb[i].wake_at;
        }
    }
    if (best == BR_TIMEOUT_INF) {
        return BR_FALSE;
    }

    if (best > br_clock_now()) {
        g_ticks = best;                 /* 1 tick == 1 us */
    }
    const br_time_t now = br_clock_now();

    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_TRUE && g_tcb[i].state == BR_TASK_BLOCKED
            && g_tcb[i].wake_at != BR_TIMEOUT_INF && g_tcb[i].wake_at <= now) {
            g_tcb[i].wait_status = BR_ERR(BR_ETIMEDOUT);
            g_tcb[i].state       = BR_TASK_READY;
            g_tcb[i].wake_at     = BR_TIMEOUT_INF;
            rq_push(&g_tcb[i]);
        }
    }
    return BR_TRUE;
}

static void fake_trampoline(void)
{
    br_thread_t *t = g_current;

    t->entry(t->arg);
    br_task_exit(0);
}

static void sched_loop(void)
{
    for (;;) {
        br_thread_t *next = rq_pop();

        if (next == NULL) {
            if (advance_timeouts() == BR_TRUE) {
                continue;
            }
            if (sched_all_done() == BR_TRUE) {
                (void)swapcontext(&g_sched_uc, &g_boot_uc);
                return;                                 /* 不会到这里 */
            }
            printf("[HOSTTEST] FAIL SCHED 死锁: 无就绪线程且无任何期限\n");
            exit(3);
        }

        g_current    = next;
        next->state  = BR_TASK_RUNNING;
        (void)swapcontext(&g_sched_uc, &g_uc[tcb_index(next)]);
    }
}

/* ---- 调度框架契约(与 br_sched.h/sched_internal.h 同签名) ---- */

br_bool br_sched_registered(void)
{
    return BR_TRUE;
}

BR_NORETURN void br_sched_run(void)
{
    sched_loop();
    for (;;) {
    }
}

int br_sched_block_current(br_time_t abs_deadline)
{
    br_thread_t *t = g_current;

    if (t == NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    if (t->wait_status == 0) {
        return 0;                    /* ICB: 挂链之后、切走之前已被唤醒 ⇒ 不切走 */
    }

    t->wait_status = BR_ERR(BR_ETIMEDOUT);
    t->state       = BR_TASK_BLOCKED;
    t->wake_at     = abs_deadline;
    (void)swapcontext(&g_uc[tcb_index(t)], &g_sched_uc);

    /* 切回来时 g_current 已被调度器置回本线程; 重新取一次, 免得依赖"跨 setjmp 的局部量"
     * (swapcontext 被 GCC 当 setjmp 同族, 跨它读局部量会触发 -Wclobbered 一类的假设)。 */
    return g_current->wait_status;
}

void br_sched_wake(br_thread_t *t)
{
    if (t == NULL) {
        return;
    }
    t->wait_status = 0;
    if (t->state == BR_TASK_BLOCKED) {
        t->state   = BR_TASK_READY;
        t->wake_at = BR_TIMEOUT_INF;
        rq_push(t);
    }
}

br_thread_t *br_task_self(void)
{
    return g_current;
}

br_u32 br_sched_task_count(void)
{
    br_u32 n = 0u;
    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_TRUE) {
            n++;
        }
    }
    return n;
}

br_u32 br_sched_ready_count(void)
{
    return g_rq_len;
}

br_u32 br_thread_pool_used(void)
{
    return br_sched_task_count();
}

/* 找一个空 TCB 槽。单独成函数 + 禁止内联: `getcontext/makecontext` 被 GCC 当 setjmp 同族,
 * 内联进来后它会把循环变量算作"跨 setjmp 的局部量"并报 -Wclobbered(值其实不需要跨过它)。 */
#if defined(__GNUC__)
#  define HOST_NOINLINE __attribute__((noinline))
#else
#  define HOST_NOINLINE
#endif

static HOST_NOINLINE br_u32 tcb_alloc_slot(void)
{
    for (br_u32 i = 0u; i < FAKE_MAX; i++) {
        if (tcb_live(&g_tcb[i]) == BR_FALSE) {
            return i;
        }
    }
    return FAKE_MAX;
}

int br_task_create(br_thread_t **out, const br_task_attr_t *attr,
                   void (*entry)(void *), void *arg)
{
    if (out == NULL || attr == NULL || entry == NULL) {
        return BR_ERR(BR_EINVAL);
    }

    const br_u32 slot = tcb_alloc_slot();
    if (slot == FAKE_MAX) {
        return BR_ERR(BR_ENOMEM);
    }

    memset(&g_tcb[slot], 0, sizeof(g_tcb[slot]));
    g_tcb[slot].magic       = BR_THREAD_MAGIC;
    g_tcb[slot].state       = BR_TASK_READY;
    g_tcb[slot].prio        = attr->prio;
    g_tcb[slot].name        = attr->name;
    g_tcb[slot].stack_top   = (void *)g_tstack[slot];
    g_tcb[slot].stack_size  = FAKE_STACK_BYTES;
    g_tcb[slot].entry       = entry;
    g_tcb[slot].arg         = arg;
    g_tcb[slot].wake_at     = BR_TIMEOUT_INF;
    g_tcb[slot].wait_status = BR_ERR(BR_ETIMEDOUT);
    g_join_waiter[slot]     = NULL;

    (void)getcontext(&g_uc[slot]);
    g_uc[slot].uc_stack.ss_sp   = g_tstack[slot];
    g_uc[slot].uc_stack.ss_size = FAKE_STACK_BYTES;
    g_uc[slot].uc_link          = NULL;
    makecontext(&g_uc[slot], fake_trampoline, 0);

    rq_push(&g_tcb[slot]);
    *out = &g_tcb[slot];
    return BR_OK;
}

BR_NORETURN void br_task_exit(int code)
{
    br_thread_t *t = g_current;

    if (t != NULL) {
        const br_u32 i = tcb_index(t);
        t->exit_code = code;
        t->state     = BR_TASK_ZOMBIE;
        t->wake_at   = BR_TIMEOUT_INF;
        if (g_join_waiter[i] != NULL) {
            br_sched_wake(g_join_waiter[i]);
            g_join_waiter[i] = NULL;
        }
    }
    (void)swapcontext(&g_uc[tcb_index(t)], &g_sched_uc);
    for (;;) {
    }
}

int br_task_join(br_thread_t *t, int *exit_code)
{
    if (t == NULL || tcb_live(t) == BR_FALSE || t == g_current) {
        return BR_ERR(BR_EINVAL);
    }

    g_join_waiter[tcb_index(t)] = g_current;
    while (t->state != BR_TASK_ZOMBIE) {
        /* ICB 协议: 让出 CPU 前必须把 wait_status 立成悲观默认(否则上一次唤醒留下的
         * 0 会被 block_current 当成"已经又被唤醒", 于是根本不切走 —— 忙等死循环)。 */
        g_current->wait_status = BR_ERR(BR_ETIMEDOUT);
        (void)br_sched_block_current(BR_TIMEOUT_INF);
    }
    if (exit_code != NULL) {
        *exit_code = t->exit_code;
    }
    t->magic = 0u;                   /* 回收 TCB */
    return BR_OK;
}

void br_task_yield(void)
{
    br_thread_t *t = g_current;

    if (t == NULL) {
        return;
    }
    t->state = BR_TASK_READY;
    rq_push(t);
    (void)swapcontext(&g_uc[tcb_index(t)], &g_sched_uc);
}

int br_task_sleep(br_time_t rel_us)
{
    if (rel_us == BR_TIMEOUT_INF) {
        return BR_ERR(BR_EINVAL);    /* 裁定 G13 */
    }
    if (rel_us == 0u) {
        return BR_OK;
    }
    const int rc = br_sched_block_current(br_deadline_from_now(rel_us));
    return (rc == 0 || rc == BR_ERR(BR_ETIMEDOUT)) ? BR_OK : rc;
}

int br_task_sleep_until(br_time_t abs_us)
{
    if (abs_us == BR_TIMEOUT_INF) {
        return BR_ERR(BR_EINVAL);
    }
    if (abs_us <= br_clock_now()) {
        return BR_OK;
    }
    const int rc = br_sched_block_current(abs_us);
    return (rc == 0 || rc == BR_ERR(BR_ETIMEDOUT)) ? BR_OK : rc;
}

const char *br_task_name(const br_thread_t *t)
{
    return (t != NULL && t->name != NULL) ? t->name : "?";
}

br_u32 br_task_state(const br_thread_t *t)
{
    return (t != NULL) ? t->state : BR_TASK_NEW;
}

/* =====================================================================
 * 宿主侧补充用例(一致性用例之外)
 * ===================================================================== */

static br_mutex_t    s_def_m  = BR_MUTEX_INIT_VALUE;
static br_sem_t      s_def_s  = BR_SEM_INIT_VALUE(3u);
static br_cond_t     s_def_c  = BR_COND_INIT_VALUE;
static br_spinlock_t s_def_sp = BR_SPINLOCK_INIT_VALUE;

static void host_static_define(void)
{
    section("TC-SYNC-STATIC", "BR_*_DEFINE 静态对象 vs init 动态(CA-2 等价性)");

    CHECK(s_def_m.magic == BR_SYNC_MAGIC_MUTEX, "TC-SYNC-STATIC", "静态 mutex 魔数就位");
    CHECK(br_mutex_lock(&s_def_m) == BR_OK, "TC-SYNC-STATIC", "静态 mutex 可加锁");
    CHECK(br_mutex_is_locked(&s_def_m) == BR_TRUE, "TC-SYNC-STATIC", "静态 mutex 状态可变");
    CHECK(br_mutex_unlock(&s_def_m) == BR_OK, "TC-SYNC-STATIC", "静态 mutex 可解锁");

    CHECK(br_sem_count(&s_def_s) == 3u, "TC-SYNC-STATIC", "静态 sem 初值 = 宏参数");
    CHECK(br_sem_take(&s_def_s, BR_TIMEOUT_ZERO) == BR_OK, "TC-SYNC-STATIC", "静态 sem 可取");
    CHECK(br_sem_give(&s_def_s) == BR_OK, "TC-SYNC-STATIC", "静态 sem 可给");

    CHECK(br_cond_signal(&s_def_c) == BR_OK, "TC-SYNC-STATIC", "静态 cond signal 可用");

    CHECK(br_spinlock_trylock(&s_def_sp, &(br_irq_state_t){0u}) == BR_OK,
          "TC-SYNC-STATIC", "静态 spinlock 可加锁");
    CHECK(br_spinlock_is_locked(&s_def_sp) == BR_TRUE, "TC-SYNC-STATIC", "静态 spinlock 状态可变");
    br_spinlock_unlock(&s_def_sp, 0u);

    br_mutex_t dyn;
    CHECK(br_mutex_init(&dyn) == BR_OK, "TC-SYNC-STATIC", "动态 mutex init");
    CHECK(br_mutex_lock(&dyn) == BR_OK, "TC-SYNC-STATIC", "动态 mutex 加锁");
    CHECK(br_mutex_unlock(&dyn) == BR_OK, "TC-SYNC-STATIC", "动态 mutex 解锁");
    CHECK(br_mutex_is_locked(&dyn) == br_mutex_is_locked(&s_def_m),
          "TC-SYNC-STATIC", "静态/动态对象行为等价(解锁后都为未锁)");
}

static void host_magic_corrupt(void)
{
    section("SYNC-MAGIC", "魔数被踩坏(非零垃圾)也必须 -EINVAL, 不静默当'没锁'");

    br_mutex_t m;
    br_sem_t   s;
    br_cond_t  c;
    br_spinlock_t sp;
    memset(&m, 0xAB, sizeof(m));
    memset(&s, 0xAB, sizeof(s));
    memset(&c, 0xAB, sizeof(c));
    memset(&sp, 0xAB, sizeof(sp));

    CHECK(br_mutex_lock(&m) == BR_ERR(BR_EINVAL), "SYNC-MAGIC", "垃圾 mutex ⇒ -EINVAL");
    CHECK(br_mutex_unlock(&m) == BR_ERR(BR_EINVAL), "SYNC-MAGIC", "垃圾 mutex unlock ⇒ -EINVAL");
    CHECK(br_sem_take(&s, BR_TIMEOUT_ZERO) == BR_ERR(BR_EINVAL), "SYNC-MAGIC", "垃圾 sem ⇒ -EINVAL");
    CHECK(br_sem_give(&s) == BR_ERR(BR_EINVAL), "SYNC-MAGIC", "垃圾 sem give ⇒ -EINVAL");
    CHECK(br_cond_signal(&c) == BR_ERR(BR_EINVAL), "SYNC-MAGIC", "垃圾 cond ⇒ -EINVAL");
    CHECK(br_spinlock_trylock(&sp, &(br_irq_state_t){0u}) == BR_ERR(BR_EINVAL),
          "SYNC-MAGIC", "垃圾 spinlock trylock ⇒ -EINVAL");
    CHECK(br_mutex_is_locked(&m) == BR_FALSE, "SYNC-MAGIC", "垃圾 mutex 查询返回 FALSE");
    CHECK(br_mutex_owner(&m) == NULL, "SYNC-MAGIC", "垃圾 mutex owner 返回 NULL");
}

static void host_sleep_resolution(void)
{
    section("TC-TIME-SLEEP", "宿主虚拟时钟下的 sleep 分辨率(1 us/tick)");

    const br_time_t t0 = br_clock_now();
    const int rc1 = br_task_sleep(1000u);
    const br_time_t d1 = br_clock_now() - t0;

    const br_time_t t1 = br_clock_now();
    const int rc2 = br_task_sleep(0u);
    const br_time_t d2 = br_clock_now() - t1;

    printf("[HOSTTEST] sleep(1000us) -> rc=%d measured=%lu us; sleep(0) -> rc=%d measured=%lu us\n",
           rc1, (unsigned long)d1, rc2, (unsigned long)d2);

    CHECK(rc1 == BR_OK && d1 >= 1000u, "TC-TIME-SLEEP", "sleep(1000us) 不早醒");
    CHECK(d1 <= 1100u, "TC-TIME-SLEEP", "虚拟时钟下晚到很小(分辨率 ~1 us)");
    CHECK(rc2 == BR_OK && d2 == 0u, "TC-TIME-SLEEP", "sleep(0) 立即返回, 不推进时钟");
    CHECK(br_task_sleep(BR_TIMEOUT_INF) == BR_ERR(BR_EINVAL), "TC-TIME-SLEEP", "sleep(INF) -EINVAL(G13)");
}

/* ---- 随机化压测(风格照 tests/host/mem_test.c 的 LCG) ---- */

#define STRESS_WORKERS 3u
#define STRESS_OPS     50000u

static br_mutex_t s_str_m;
static br_sem_t   s_str_sem;
static volatile br_u32 s_str_guard;
static volatile br_u32 s_str_overlap;
static volatile br_u32 s_str_incs;
static br_u32 s_str_worker_incs[STRESS_WORKERS];
static br_u32 s_str_errs;
static br_u32 s_str_rng = 0x12345678u;

static br_u32 str_rng(void)
{
    s_str_rng = (s_str_rng * 1103515245u) + 12345u;
    return (s_str_rng >> 8);
}

static void stress_worker(void *arg)
{
    br_u32 *mine = (br_u32 *)arg;

    for (br_u32 i = 0u; i < STRESS_OPS; i++) {
        const br_u32 r = str_rng();

        switch (r % 4u) {
        case 0u:
        case 1u:
            if (br_mutex_lock(&s_str_m) != BR_OK) {
                s_str_errs++;
                return;
            }
            if (s_str_guard != 0u) {
                s_str_overlap++;
            }
            s_str_guard = 1u;
            (*mine)++;
            s_str_incs++;
            if ((r & 0x100u) != 0u) {
                br_task_yield();
            }
            s_str_guard = 0u;
            if (br_mutex_unlock(&s_str_m) != BR_OK) {
                s_str_errs++;
                return;
            }
            break;
        case 2u:
            if (br_sem_take(&s_str_sem, BR_TIMEOUT_ZERO) == BR_OK) {
                if (br_sem_give(&s_str_sem) != BR_OK) {
                    s_str_errs++;
                    return;
                }
            }
            break;
        default:
            br_task_yield();
            break;
        }
    }
}

static void host_stress(void)
{
    section("TC-SYNC-STRESS", "随机化压测: 3 线程 x 50k 次(mutex/sem/yield)");

    CHECK(br_mutex_init(&s_str_m) == BR_OK, "TC-SYNC-STRESS", "压测 mutex init");
    CHECK(br_sem_init(&s_str_sem, STRESS_WORKERS) == BR_OK, "TC-SYNC-STRESS", "压测 sem init(3)");

    s_str_guard = 0u;
    s_str_overlap = 0u;
    s_str_incs = 0u;
    s_str_errs = 0u;
    for (br_u32 i = 0u; i < STRESS_WORKERS; i++) {
        s_str_worker_incs[i] = 0u;
    }

    br_thread_t *ts[STRESS_WORKERS];
    for (br_u32 i = 0u; i < STRESS_WORKERS; i++) {
        ts[i] = NULL;
        br_task_attr_t attr;
        attr.name       = "stress";
        attr.stack      = (void *)g_tstack[0];
        attr.stack_size = FAKE_STACK_BYTES;
        attr.prio       = 0u;
        attr.flags      = 0u;
        (void)br_task_create(&ts[i], &attr, stress_worker, &s_str_worker_incs[i]);
    }

    br_u32 joined = 0u;
    for (br_u32 i = 0u; i < STRESS_WORKERS; i++) {
        int code = 0;
        if (br_task_join(ts[i], &code) == BR_OK) {
            joined++;
        }
    }

    br_u32 expect = 0u;
    for (br_u32 i = 0u; i < STRESS_WORKERS; i++) {
        expect += s_str_worker_incs[i];
    }

    printf("[HOSTTEST] stress: joined=%u incs=%lu expect=%lu overlap=%lu errs=%lu sem_count=%lu\n",
           joined, (unsigned long)s_str_incs, (unsigned long)expect,
           (unsigned long)s_str_overlap, (unsigned long)s_str_errs,
           (unsigned long)br_sem_count(&s_str_sem));

    CHECK(joined == STRESS_WORKERS, "TC-SYNC-STRESS", "3 个压测线程都 join 成功");
    CHECK(s_str_errs == 0u, "TC-SYNC-STRESS", "压测期间无原语错误返回");
    CHECK(s_str_overlap == 0u, "TC-SYNC-STRESS", "压测期间临界区无重叠(互斥成立)");
    CHECK(s_str_incs == expect, "TC-SYNC-STRESS", "压测计数精确(无丢失)");
    CHECK(br_mutex_is_locked(&s_str_m) == BR_FALSE, "TC-SYNC-STRESS", "压测后 mutex 未持有");
    CHECK(br_sem_count(&s_str_sem) == STRESS_WORKERS, "TC-SYNC-STRESS", "压测后 sem 计数回到初值");
    CHECK(br_waitq_len(s_str_m.head) == 0u && s_str_m.waiters == 0u,
          "TC-SYNC-STRESS", "压测后 mutex 等待链为 0(无幽灵节点)");
    CHECK(br_waitq_len(s_str_sem.head) == 0u && s_str_sem.waiters == 0u,
          "TC-SYNC-STRESS", "压测后 sem 等待链为 0(无幽灵节点)");
}

/* =====================================================================
 * 主流程(跑在假调度器物化出来的"main 线程"里)
 * ===================================================================== */

static void host_body(void)
{
    /* 与 target 侧**同一份**自检套件(`core/selftest/sync_selftest.c`): 真的建线程 +
     * join, 阻塞路径真跑(返回值 = 失败项数, 红绿以日志与宿主汇总为准)。 */
    br_sync_selftest();

    host_static_define();
    host_magic_corrupt();
    host_sleep_resolution();
    host_stress();

    /* 全局收尾: 任何用例都不该留下等待者 */
    printf("[HOSTTEST] final: task_count=%lu ready=%lu\n",
           (unsigned long)br_sched_task_count(), (unsigned long)br_sched_ready_count());
}

static void host_main_trampoline(void)
{
    host_body();
    br_task_exit(0);
}

int main(void)
{
    (void)setvbuf(stdout, NULL, _IONBF, 0u);   /* 用例若挂, 也要能看到挂在哪一行 */
    printf("[HOSTTEST] brickOS prototype — 宿主侧同步原语门禁(ucontext 假调度器, 阻塞路径真跑)\n");
    (void)br_clock_init();

    (void)getcontext(&g_sched_uc);
    g_sched_uc.uc_stack.ss_sp   = g_sched_stack;
    g_sched_uc.uc_stack.ss_size = sizeof(g_sched_stack);
    g_sched_uc.uc_link          = NULL;
    makecontext(&g_sched_uc, sched_loop, 0);

    memset(&g_tcb[0], 0, sizeof(g_tcb[0]));
    g_tcb[0].magic       = BR_THREAD_MAGIC;
    g_tcb[0].state       = BR_TASK_READY;
    g_tcb[0].name        = "host-main";
    g_tcb[0].wake_at     = BR_TIMEOUT_INF;
    g_tcb[0].wait_status = BR_ERR(BR_ETIMEDOUT);
    (void)getcontext(&g_uc[0]);
    g_uc[0].uc_stack.ss_sp   = g_tstack[0];
    g_uc[0].uc_stack.ss_size = FAKE_STACK_BYTES;
    g_uc[0].uc_link          = NULL;
    makecontext(&g_uc[0], host_main_trampoline, 0);
    rq_push(&g_tcb[0]);

    /* 启动: 切到调度器, 由它跑 main 线程; 全部线程结束后调度器切回这里 */
    (void)swapcontext(&g_boot_uc, &g_sched_uc);

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d\n", s_pass, s_fail, s_pass + s_fail);
    return (s_fail == 0) ? 0 : 1;
}
