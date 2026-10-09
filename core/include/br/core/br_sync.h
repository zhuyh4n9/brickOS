/*
 * brickOS prototype v0.2.0 — 同步原语(core): mutex / semaphore / cond / spinlock
 *
 * 设计依据: `3-01` §7(同步原语, 117–139 行)、§14 CA-4(超时三态)、INV-1/INV-2(超时语义),
 *           `3-02` §6.4(`br_irq_lock/unlock` 的嵌套计数在 core), `6-01` §3.2(TC-SYNC-*)。
 *
 * ## 归属与偏离(ADR-0006 §2)
 *
 * 设计 `3-01` §115 行把同步原语的**实现**放在调度插件里。本原型改为 **core 实现**:
 *   - 锁对象必须被所有插件看见 ⇒ 放插件头会逼出"插件互依赖";
 *   - 阻塞/唤醒机制对 coop 与 preempt 是同一套(只有"谁先跑"不同);
 *   - 设计的 ops 表本身是带省略号的草案。
 * 于是 `br_sched_ops` 只留"谁先跑", 同步语义留在 core。**对外语义一字不改**
 * (签名、错误码、ISR-safe 的 `br_sem_give` 都照设计)。
 *
 * ## 设计里没有的东西(如实登记)
 *
 * **`spinlock` 在设计文档里 0 命中**(`grep -rni spin Design/` 无结果; 最接近的是
 * `br_irq_lock/br_irq_unlock`)。用户需求里明确要它 ⇒ 本刀**新增面**, 走 CA-5 记录:
 * `br_spinlock_t` = 自旋位 + **关中断**(单核上"关中断"才是互斥的真身, 自旋位为将来
 * 多核留位)。它与 `br_irq_lock` 的关系: spinlock = irq_lock + 跨核自旋 + 可观测的争用计数。
 *
 * ## 时间常量
 *
 * `BR_TIMEOUT_INF` / `BR_TIMEOUT_ZERO` / `br_deadline_from_now()` 属 `br_time.h`
 * (设计 `3-01` §14 CA-1 / §7); 本头文件只使用它们。
 */
#ifndef BR_CORE_BR_SYNC_H
#define BR_CORE_BR_SYNC_H

#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* 对象魔法字: 观测与自检用它认"这个对象有没有被初始化"(设计没有该字段, 见 ADR-0006 §2)。 */
#define BR_SYNC_MAGIC_MUTEX 0x4D555458u /* "MUTX" */
#define BR_SYNC_MAGIC_SEM   0x53454D41u /* "SEMA" */
#define BR_SYNC_MAGIC_COND  0x434F4E44u /* "COND" */
#define BR_SYNC_MAGIC_SPIN  0x5350494Eu /* "SPIN" */

/* ==================================================================== mutex */

/*
 * 互斥量。`head` = 等待链头(节点嵌在线程的 TCB 里 —— 同步路径**零分配**)。
 * `locked` 与 `owner` 分开存: `locked` 供无锁快路径, `owner` 供诊断与将来的 PI(v2)。
 */
typedef struct br_mutex {
    br_u32        magic;
    volatile br_u32 locked;
    br_thread_t  *owner;
    br_u32        waiters;
    void         *head;
} br_mutex_t;

#define BR_MUTEX_INIT_VALUE { BR_SYNC_MAGIC_MUTEX, 0u, BR_NULL, 0u, BR_NULL }
/* 静态初始化(设计 `3-01` §7 的 BR_MUTEX_DEFINE); 静态对象**不必**再调 init。 */
#define BR_MUTEX_DEFINE(name) br_mutex_t name = BR_MUTEX_INIT_VALUE

int br_mutex_init(br_mutex_t *m);
/* 阻塞获取(无限等)。-EINVAL = 未初始化的对象。 */
int br_mutex_lock(br_mutex_t *m);
/* 带超时: BR_TIMEOUT_ZERO = trylock(忙 ⇒ `-ETIMEDOUT` —— 设计 `6-01` TC-SYNC-002
 * 与 INV-1: 零超时只是"等 0 微秒", 不是另一种错); BR_TIMEOUT_INF = 无限。 */
int br_mutex_lock_to(br_mutex_t *m, br_time_t timeout);
/* 非持有者解锁 ⇒ -EPERM(设计 `3-01` §7 的语义)。 */
int br_mutex_unlock(br_mutex_t *m);

br_bool br_mutex_is_locked(const br_mutex_t *m);
br_thread_t *br_mutex_owner(const br_mutex_t *m);

/* ==================================================================== semaphore */

typedef struct br_sem {
    br_u32        magic;
    volatile br_u32 count;
    br_u32        waiters;
    void         *head;
} br_sem_t;

#define BR_SEM_INIT_VALUE(n) { BR_SYNC_MAGIC_SEM, (n), 0u, BR_NULL }
#define BR_SEM_DEFINE(name, n) br_sem_t name = BR_SEM_INIT_VALUE(n)

int br_sem_init(br_sem_t *s, unsigned initial);
/* 取(p 操作): count > 0 ⇒ 减一并返回; 否则阻塞(timeout 三态同上)。 */
int br_sem_take(br_sem_t *s, br_time_t timeout);
/* 给(v 操作): **ISR-safe**(`3-01` §7 / `3-02` §11.3); count += 1, 唤醒一个等待者。
 * v1 的 COOP 下**不立即切换**到被唤醒线程(`3-02` 1097 行)。 */
int br_sem_give(br_sem_t *s);
br_u32 br_sem_count(const br_sem_t *s);

/* ==================================================================== cond */

typedef struct br_cond {
    br_u32 magic;
    br_u32 waiters;
    void  *head;
} br_cond_t;

#define BR_COND_INIT_VALUE { BR_SYNC_MAGIC_COND, 0u, BR_NULL }
#define BR_COND_DEFINE(name) br_cond_t name = BR_COND_INIT_VALUE

int br_cond_init(br_cond_t *c);
/*
 * 原子地"释放 m + 等待 + 重新获取 m"。**超时返回 -ETIMEDOUT 时持锁状态必须已恢复**
 * (TC-SYNC-008 的判据)。无等待者时 signal 是 no-op(不计数 —— 与 sem 的区别就在这,
 * 见 ADR-0006 §2 的裁定 G12)。
 */
int br_cond_wait(br_cond_t *c, br_mutex_t *m, br_time_t timeout);
int br_cond_signal(br_cond_t *c);    /* 唤醒一个(可为 ISR 上下文) */
int br_cond_broadcast(br_cond_t *c); /* 唤醒全部 */
br_u32 br_cond_waiters(const br_cond_t *c);

/* ==================================================================== spinlock */

/*
 * 自旋锁(**设计里没有的新面**, 见文件头注)。语义:
 *   `br_spinlock_lock` = 关中断(嵌套计数在 core) + 自旋直到拿到锁; 返回**进入前的
 *   中断状态**, 必须原样交给 `br_spinlock_unlock`(与 br_irq_lock/unlock 同构的配对纪律)。
 *   在单核原型上自旋位永远一次成功(没有别人在跑), 但它让"临界区"在代码里显式可见,
 *   并为多核留位。
 */
typedef struct br_spinlock {
    br_u32          magic;
    volatile br_u32 lock;
} br_spinlock_t;

#define BR_SPINLOCK_INIT_VALUE { BR_SYNC_MAGIC_SPIN, 0u }
#define BR_SPINLOCK_DEFINE(name) br_spinlock_t name = BR_SPINLOCK_INIT_VALUE

int br_spinlock_init(br_spinlock_t *s);
br_irq_state_t br_spinlock_lock(br_spinlock_t *s);
/* 忙 ⇒ -EBUSY(不阻塞, 不关中断; `saved` 不变) */
int br_spinlock_trylock(br_spinlock_t *s, br_irq_state_t *saved);
void br_spinlock_unlock(br_spinlock_t *s, br_irq_state_t saved);
br_bool br_spinlock_is_locked(const br_spinlock_t *s);
/* 观测: 自旋过至少一圈的次数(单核上恒 0 ⇒ 它证明"这里真的量了争用")。 */
br_u32 br_spinlock_contention(void);

/*
 * 这里**不再**声明同步套件的入口 `br_sync_selftest()`。理由: 自检入口**不是插件的
 * 对外能力**, 留在本头就会进 golden 接口面 —— 于是"改一个用例"变成接口变更(与
 * ADR-0005 裁定 9 对钩子的处置同源)。声明集中在 `core/selftest/core_selftest.c`,
 * 实现随套件一起在 `core/selftest/sync_selftest.c`(ADR-0010)。
 */

#endif /* BR_CORE_BR_SYNC_H */
