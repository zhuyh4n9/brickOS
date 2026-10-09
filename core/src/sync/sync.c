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
 *    CPU 的那一步在开中断状态下发生(否则 tick(`1/HZ`)进不来, 超时永远不触发)。
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

