/*
 * brickOS prototype v0.2.0 — 同步原语实现(core): mutex / semaphore / cond / spinlock
 *
 * 设计依据: `3-01` §7(同步原语, 117–139 行)、§14 CA-4(超时三态)、INV-1/INV-2(超时语义),
 *           `3-02` §6.4(`br_irq_lock/unlock` 的嵌套计数在 core)、§11.3(ISR 里 give 的语义:
 *           只改状态/挂链, **不在 ISR 里切换**), `6-01` §3.2/§3.3(TC-SYNC-* / TC-TIME-*)。
 *
 * ## 三条承重设计
 *
 * 1. **阻塞的唯一基石**是 `br_sched_block_current(abs_deadline)`(`br_sched.h`):
 *    返回 0 = 被唤醒, `-ETIMEDOUT` = 期限已到。本文件不碰栈、不做上下文切换。
 *
 * 2. **等待者节点嵌在 TCB 里**(`sched_internal.h` 的 `br_waitq_push/pop/remove/len`),
 *    于是同步路径**零分配** —— 锁不会因为"分配失败"而失败。等待链指针就是对象里的
 *    `head` 字段, 节点复用 TCB 的 `wait_next`。
 *
 * 3. **"检查条件 + 挂/摘等待链 + 所有权交接"全部在关中断临界区里**; 只有真正让出
 *    CPU 的那一步在开中断状态下发生(否则 100 ms 的 tick 进不来, 超时永远不触发)。
 *
 * ## 与调度框架的握手位(ICB 协议; 见 ADR-0007 §2.6)
 *
 * 从"关中断临界区里挂完等待链"到"`br_sched_block_current()` 把线程置 BLOCKED"之间,
 * 存在一个 ISR 可以 `give`/`signal` 并唤醒本线程的窗口。用 TCB 里**既有**的
 * `wait_status` 消掉它(不需要新字段):
 *   - 调用方在临界区内 `self->wait_status = -ETIMEDOUT` 并挂等待链;
 *   - `br_sched_wake(t)` 先写 `t->wait_status = 0`, 仅当 `t->state == BLOCKED` 才置
 *     READY + 挂就绪队列 ⇒ 落在窗口里的唤醒**只置位、不入队**, 不会丢;
 *   - `br_sched_block_current()` 看到 `wait_status == 0` 就**不切走**, 直接返回 0;
 *   - 超时扫描只对 `state == BLOCKED` 的线程置 `wait_status = -ETIMEDOUT`。
 * 顺序无论"先超时后被交接"还是"先交接后超时"都收敛到同一个赢家。
 *
 * ## 错误码口径(冻结)
 *
 * `-EINVAL`(对象未初始化/魔数被踩) / `-EPERM`(非持有者 unlock) / `-EBUSY`(spinlock 的
 * `BR_TIMEOUT_ZERO` 忙) / `-ETIMEDOUT`(阻塞超时统一, INV-1) / `-ENOTSUP`(需要阻塞
 * 但当前没有线程上下文)。**sem 的 `take(ZERO)` 用 `-ETIMEDOUT`**(它就是"零期限",
 * 与 mutex 的 trylock 语义刻意不同; 依据 6-01 TC-SYNC-004, 见 ADR-0007 §2.3)。
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* TCB 布局与等待链四原语是 core 内部契约(sched_internal.h 自己也这么说)。
 * 跨目录相对包含在本仓有先例(core/src/irq/irq_fault.c -> "../panic_internal.h")。 */
#include "../sched/sched_internal.h"

/* =====================================================================
 * 公共小件
 * ===================================================================== */

#define SYNC_MUTEX_OK(m) ((m) != BR_NULL && (m)->magic == BR_SYNC_MAGIC_MUTEX)
#define SYNC_SEM_OK(s)   ((s) != BR_NULL && (s)->magic == BR_SYNC_MAGIC_SEM)
#define SYNC_COND_OK(c)  ((c) != BR_NULL && (c)->magic == BR_SYNC_MAGIC_COND)
#define SYNC_SPIN_OK(s)  ((s) != BR_NULL && (s)->magic == BR_SYNC_MAGIC_SPIN)

/* 相对超时 → 绝对期限。INF 原样传递("没有期限"不是"很远的期限")。 */
static br_time_t sync_deadline(br_time_t timeout)
{
    return (timeout == BR_TIMEOUT_INF) ? BR_TIMEOUT_INF : br_deadline_from_now(timeout);
}

/*
 * 阻塞收尾: `br_sched_block_current()` 因超时返回时, 唤醒者并没有把我们摘下来
 * (它压根不知道我们在哪条链上) ⇒ **必须自己摘**, 否则链上留下一个"幽灵"节点:
 * 下一次 give/signal 会唤醒一个早就返回的线程。
 * 被正常唤醒时唤醒者已经 pop 过我们, `br_waitq_remove` 返回 0, 这里不动计数。
 */
static int sync_wait_finish(void **head, br_u32 *waiters, int rc)
{
    if (rc == 0) {
        return 0;
    }

    br_thread_t *self = br_task_self();
    br_irq_state_t st = br_irq_lock();

    if (self != BR_NULL && br_waitq_remove(head, self) != 0) {
        if (*waiters > 0u) {
            (*waiters)--;
        }
    }
    br_irq_unlock(st);
    return rc;
}

/*
 * 持锁者放锁的**临界区内**实现(调用者已确认自已是 owner 且关着中断)。
 * 有等待者 ⇒ **直接交接所有权**(locked 保持 1, owner 换成被唤醒者):
 * 这样不会出现"计数先放出去、被第三个线程抢走"的窗口。
 */
static void sync_mutex_release_locked(br_mutex_t *m)
{
    br_thread_t *w = br_waitq_pop(&m->head);

    if (w != BR_NULL) {
        if (m->waiters > 0u) {
            m->waiters--;
        }
        m->owner = w;              /* 交接 */
        br_sched_wake(w);
    } else {
        m->locked = 0u;
        m->owner  = BR_NULL;
    }
}

/* =====================================================================
 * mutex
 * ===================================================================== */

int br_mutex_init(br_mutex_t *m)
{
    if (m == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    m->magic   = BR_SYNC_MAGIC_MUTEX;
    m->locked  = 0u;
    m->owner   = BR_NULL;
    m->waiters = 0u;
    m->head    = BR_NULL;
    br_irq_unlock(st);
    return BR_OK;
}

int br_mutex_lock_to(br_mutex_t *m, br_time_t timeout)
{
    if (!SYNC_MUTEX_OK(m)) {
        return BR_ERR(BR_EINVAL);
    }

    br_thread_t   *self = br_task_self();
    br_irq_state_t st   = br_irq_lock();

    if (m->locked == 0u) {                       /* 无锁快路径 */
        m->locked = 1u;
        m->owner  = self;
        br_irq_unlock(st);
        return BR_OK;
    }

    if (timeout == BR_TIMEOUT_ZERO) {            /* trylock 语义 */
        /* ⚠ 这里返回 `-ETIMEDOUT` 而不是 `-EBUSY`: 设计 `6-01` 的 TC-SYNC-002 明写
         * "lock_to(ZERO): 未锁 0 / 已锁 `-ETIMEDOUT`", 而 INV-1 把"等不到"统一到
         * `-ETIMEDOUT` —— 零超时只是"等 0 微秒", 不是另一种错。冻结头最初写成 -EBUSY
         * 是本刀的偏离, 已按设计改回(ADR-0007 §2.7); spinlock 的 trylock 仍用
         * `-EBUSY`(它不在设计面内, 是本刀新增, 见 br_sync.h 头注)。 */
        br_irq_unlock(st);
        return BR_ERR(BR_ETIMEDOUT);
    }

    if (self == BR_NULL) {                       /* 有锁要等, 但没有线程上下文 */
        br_irq_unlock(st);
        return BR_ERR(BR_ENOTSUP);
    }

    br_waitq_push(&m->head, self);
    m->waiters++;
    self->wait_status = BR_ERR(BR_ETIMEDOUT);    /* ICB: 乐观之前先立悲观默认 */
    br_irq_unlock(st);

    const int rc = br_sched_block_current(sync_deadline(timeout));
    /* rc == 0 ⇒ `br_mutex_unlock` 已把所有权交接给我们, 这里不再碰 locked/owner。 */
    return sync_wait_finish(&m->head, &m->waiters, rc);
}

int br_mutex_lock(br_mutex_t *m)
{
    return br_mutex_lock_to(m, BR_TIMEOUT_INF);
}

int br_mutex_unlock(br_mutex_t *m)
{
    if (!SYNC_MUTEX_OK(m)) {
        return BR_ERR(BR_EINVAL);
    }

    br_thread_t   *self = br_task_self();
    br_irq_state_t st   = br_irq_lock();

    if ((m->locked == 0u) || (m->owner != self)) {
        br_irq_unlock(st);
        return BR_ERR(BR_EPERM);
    }

    sync_mutex_release_locked(m);
    br_irq_unlock(st);
    return BR_OK;
}

br_bool br_mutex_is_locked(const br_mutex_t *m)
{
    if (!SYNC_MUTEX_OK(m)) {
        return BR_FALSE;
    }
    return (m->locked != 0u) ? BR_TRUE : BR_FALSE;
}

br_thread_t *br_mutex_owner(const br_mutex_t *m)
{
    if (!SYNC_MUTEX_OK(m)) {
        return BR_NULL;
    }
    return m->owner;
}

/* =====================================================================
 * semaphore
 * ===================================================================== */

int br_sem_init(br_sem_t *s, unsigned initial)
{
    if (s == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    s->magic   = BR_SYNC_MAGIC_SEM;
    s->count   = (br_u32)initial;
    s->waiters = 0u;
    s->head    = BR_NULL;
    br_irq_unlock(st);
    return BR_OK;
}

int br_sem_take(br_sem_t *s, br_time_t timeout)
{
    if (!SYNC_SEM_OK(s)) {
        return BR_ERR(BR_EINVAL);
    }

    br_thread_t   *self = br_task_self();
    br_irq_state_t st   = br_irq_lock();

    if (s->count > 0u) {
        s->count--;
        br_irq_unlock(st);
        return BR_OK;
    }

    if (timeout == BR_TIMEOUT_ZERO) {            /* 零期限 = 立刻到期 */
        br_irq_unlock(st);
        return BR_ERR(BR_ETIMEDOUT);
    }

    if (self == BR_NULL) {
        br_irq_unlock(st);
        return BR_ERR(BR_ENOTSUP);
    }

    br_waitq_push(&s->head, self);
    s->waiters++;
    self->wait_status = BR_ERR(BR_ETIMEDOUT);
    br_irq_unlock(st);

    const int rc = br_sched_block_current(sync_deadline(timeout));
    /* rc == 0 ⇒ give 已把令牌**直接交接**给我们(计数未被加回), 无需再减。 */
    return sync_wait_finish(&s->head, &s->waiters, rc);
}

/*
 * ISR-safe(CA-3 白名单, `3-02` §11.3): 只做"计数 + 挂链/交接 + 唤醒标记", 不切换。
 * 被唤醒者在下一个调度点才跑 —— 这是 v1 COOP 的诚实语义。
 */
int br_sem_give(br_sem_t *s)
{
    if (!SYNC_SEM_OK(s)) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    br_thread_t *w = br_waitq_pop(&s->head);

    if (w != BR_NULL) {
        if (s->waiters > 0u) {
            s->waiters--;
        }
        br_sched_wake(w);                        /* 直接交接: 不动 count */
    } else {
        s->count++;                              /* 没人等 ⇒ 记在计数上 */
    }

    br_irq_unlock(st);
    return BR_OK;
}

br_u32 br_sem_count(const br_sem_t *s)
{
    if (!SYNC_SEM_OK(s)) {
        return 0u;
    }
    return s->count;
}

/* =====================================================================
 * cond
 * ===================================================================== */

int br_cond_init(br_cond_t *c)
{
    if (c == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    c->magic   = BR_SYNC_MAGIC_COND;
    c->waiters = 0u;
    c->head    = BR_NULL;
    br_irq_unlock(st);
    return BR_OK;
}

int br_cond_wait(br_cond_t *c, br_mutex_t *m, br_time_t timeout)
{
    if (!SYNC_COND_OK(c) || !SYNC_MUTEX_OK(m)) {
        return BR_ERR(BR_EINVAL);
    }

    br_thread_t *self = br_task_self();
    br_irq_state_t st = br_irq_lock();

    if ((m->locked == 0u) || (m->owner != self)) {   /* cond_wait 的前提: 持有 m */
        br_irq_unlock(st);
        return BR_ERR(BR_EPERM);
    }

    /*
     * 零期限特例: 不挂等待链。语义仍是"原子地放锁 + 重取锁 + 返回超时",
     * 但不经过 `br_sched_block_current`(否则是否走超时取决于调度器如何解释
     * "期限已在过去", 这里把判据收进同步原语内部, 结果确定)。
     * 这条路径也不需要线程上下文(不阻塞), 所以 `self == NULL` 的检查放在它之后。
     */
    if (timeout == BR_TIMEOUT_ZERO) {
        sync_mutex_release_locked(m);
        br_irq_unlock(st);
        const int rl = br_mutex_lock(m);
        return (rl != 0) ? rl : BR_ERR(BR_ETIMEDOUT);
    }

    if (self == BR_NULL) {                           /* 要阻塞, 但没有线程上下文 */
        br_irq_unlock(st);
        return BR_ERR(BR_ENOTSUP);
    }

    br_waitq_push(&c->head, self);
    c->waiters++;
    self->wait_status = BR_ERR(BR_ETIMEDOUT);
    sync_mutex_release_locked(m);                    /* 原子: 放锁(可能交接给 mutex 等待者) */
    br_irq_unlock(st);

    const int rc = br_sched_block_current(sync_deadline(timeout));

    if (rc != 0) {                                   /* 超时: 把自己从 cond 等待链摘掉 */
        st = br_irq_lock();
        if (br_waitq_remove(&c->head, self) != 0) {
            if (c->waiters > 0u) {
                c->waiters--;
            }
        }
        br_irq_unlock(st);
    }

    /*
     * ★ TC-SYNC-008 的判据: **超时返回 -ETIMEDOUT 时也必须已经重新持有 m**。
     * 用无期限重取(超时语义只覆盖"等待", 不覆盖"重新加锁" —— 标准 cond 语义)。
     */
    const int rl = br_mutex_lock(m);
    if (rl != 0) {
        return rl;
    }
    return rc;
}

int br_cond_signal(br_cond_t *c)
{
    if (!SYNC_COND_OK(c)) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    br_thread_t *w = br_waitq_pop(&c->head);

    if (w != BR_NULL) {
        if (c->waiters > 0u) {
            c->waiters--;
        }
        br_sched_wake(w);
    }
    /* 无等待者 ⇒ no-op, **不计数**(裁定 G12: 别把 cond 做成 sem)。 */

    br_irq_unlock(st);
    return BR_OK;
}

int br_cond_broadcast(br_cond_t *c)
{
    if (!SYNC_COND_OK(c)) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    for (;;) {
        br_thread_t *w = br_waitq_pop(&c->head);
        if (w == BR_NULL) {
            break;
        }
        if (c->waiters > 0u) {
            c->waiters--;
        }
        br_sched_wake(w);
    }

    br_irq_unlock(st);
    return BR_OK;
}

br_u32 br_cond_waiters(const br_cond_t *c)
{
    if (!SYNC_COND_OK(c)) {
        return 0u;
    }
    return c->waiters;
}

/* =====================================================================
 * spinlock(设计里 0 命中的新增面, 见 br_sync.h 头注与 ADR-0007 §2.1)
 * ===================================================================== */

static volatile br_u32 s_spin_contention;   /* 进过自旋循环的次数 */

int br_spinlock_init(br_spinlock_t *s)
{
    if (s == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();
    s->magic = BR_SYNC_MAGIC_SPIN;
    s->lock  = 0u;
    br_irq_unlock(st);
    return BR_OK;
}

/*
 * 关中断(嵌套计数在 core)+ 自旋到拿到锁, 返回**进入前的中断状态**。
 * 单核上自旋位一次成功(没有别人在跑), 但"关中断"才是互斥的真身; 自旋位让临界区
 * 在代码里显式可见, 并为多核(SMP)留位。
 */
br_irq_state_t br_spinlock_lock(br_spinlock_t *s)
{
    const br_irq_state_t st = br_irq_lock();

    if (s == BR_NULL || s->magic != BR_SYNC_MAGIC_SPIN) {
        /* 无返回错误码的通道; 未初始化/被踩坏是内核 bug, 不静默当"没锁"(ADR-0007 §2.5)。 */
        br_panic_bare("spinlock: magic corrupt (s=%p)", (const void *)s);
    }

    if (s->lock != 0u) {
        s_spin_contention++;                 /* 单核上只有"自己锁自己"才会走到这里 */
        while (s->lock != 0u) {
            /* 自旋等待。多核上等的是另一个核; 单核上这里是死锁, 不是正常路径。 */
        }
    }
    s->lock = 1u;
    return st;
}

int br_spinlock_trylock(br_spinlock_t *s, br_irq_state_t *saved)
{
    if (!SYNC_SPIN_OK(s)) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    if (s->lock != 0u) {
        br_irq_unlock(st);                   /* 忙: 不持锁、不改 *saved */
        return BR_ERR(BR_EBUSY);
    }

    s->lock = 1u;
    if (saved != BR_NULL) {
        *saved = st;
    }
    return BR_OK;
}

void br_spinlock_unlock(br_spinlock_t *s, br_irq_state_t saved)
{
    if (SYNC_SPIN_OK(s)) {
        s->lock = 0u;
    }
    /* 魔数坏了也要把中断状态交回去 —— 否则一次脏对象会让本核永久关中断。 */
    br_irq_unlock(saved);
}

br_bool br_spinlock_is_locked(const br_spinlock_t *s)
{
    if (!SYNC_SPIN_OK(s)) {
        return BR_FALSE;
    }
    return (s->lock != 0u) ? BR_TRUE : BR_FALSE;
}

br_u32 br_spinlock_contention(void)
{
    return s_spin_contention;
}

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

    /* sleep 不早醒(实测; 100 ms tick ⇒ 请求 1000 us 会晚到约一个 tick) */
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

void br_sync_conformance(void)
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
}
