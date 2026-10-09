/*
 * brickOS prototype v0.2.0 — 线程(pthread.h; runtime/posix 插件提供)
 *
 * ★★ 这是**映射层**, 不是第二套线程实现: 每个 `pthread_*` 都落到 core 的
 *    `br_task_*` / `br_sync_*` 上(`3-01` §2/§3)。三条平台差异必须知道:
 *
 *   ① **栈要调用方给**: `br_task_create` 的契约是"栈由调用方提供"(`br_sched.h`),
 *      而 `pthread_create` 的 POSIX 签名里没有栈。本代的做法 = **本服务从 core 堆上
 *      切一块**(`pthread_attr_setstacksize` 可改大小, 默认 4 KiB), `join` 时归还。
 *      ⇒ 线程数实际上受堆与 `BR_TASK_MAX`(8)双重约束; 创建失败返回 `EAGAIN`。
 *   ② **`join` 的返回值有第二张表**: core 的 `br_task_join` 只回 `int exit_code`,
 *      而 POSIX 要 `void *retval`。于是本服务维护一张 `{线程, retval, 栈}` 的小表,
 *      `pthread_exit(v)` 与"入口函数返回 v"都把 v 记进去, `join` 取出来。
 *   ③ **`pthread_detach` 不支持**(core 只有 join 回收 ZOMBIE 一条路)⇒ 返回 -ENOTSUP,
 *      而不是假装成功(假装成功会让 TCB 池悄悄漏光)。
 *
 * ★ 同步对象**直接用 core 的类型**(`br_mutex_t`/`br_cond_t`/`br_sem_t` 的布局是公开的,
 *   见 br_sync.h)。好处是静态初始化器可以逐字复用(`BR_MUTEX_INIT_VALUE`), 于是
 *   `PTHREAD_MUTEX_INITIALIZER` 是**真**可用的静态初始化, 不需要"懒初始化"的技巧。
 *   `destroy` 是**空操作返回 0** —— core 没有 `br_mutex_destroy`(`11-02` 的 P-5),
 *   而 POSIX 要求销毁未使用的互斥量必须成功(Linux 亦然)⇒ 空操作是**正确**答案,
 *   不是敷衍; 真正的缺口是"销毁后仍被引用"检测不到, 登记在 ADR-0014。
 */
#ifndef BR_POSIX_PTHREAD_H
#define BR_POSIX_PTHREAD_H

#include <sys/types.h>
#include <time.h>                 /* struct timespec(pthread_cond_timedwait) */
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>

/* ---- 线程 ---- */
typedef br_thread_t *pthread_t;

typedef struct pthread_attr {
    size_t stack_size;      /* 0 ⇒ 用默认值(BR_POSIX_STACK_DEFAULT) */
    void  *stack;           /* 非空 ⇒ 用调用方给的栈(此时 join 不归还) */
} pthread_attr_t;

#define BR_POSIX_STACK_DEFAULT  4096u

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **retval);
void pthread_exit(void *retval);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_yield(void);
int pthread_detach(pthread_t t);        /* 本代不支持 ⇒ -ENOTSUP */

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size);
int pthread_attr_setstack(pthread_attr_t *a, void *stack, size_t size);

/* ---- 互斥量 ---- */
typedef struct { br_mutex_t m; } pthread_mutex_t;
typedef struct { unsigned kind; } pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER   { BR_MUTEX_INIT_VALUE }

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);      /* 空操作 ⇒ 0(见文件头 ③) */
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

/* ---- 条件变量 ---- */
typedef struct { br_cond_t c; } pthread_cond_t;
typedef struct { unsigned clock; } pthread_condattr_t;

#define PTHREAD_COND_INITIALIZER    { BR_COND_INIT_VALUE }

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);        /* 空操作 ⇒ 0 */
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

/* ---- 读写锁 / 屏障 / key: **本代不提供**(TR-C)----
 * 需要时的工作量见 `11-02` §2.3: rwlock/barrier 要 core 出新原语, key 要 core 给出
 * per-thread 通用槽位(P-4)。这里不声明 = 让"未实现"在**编译期**就暴露(子集诚实义务)。 */

#endif /* BR_POSIX_PTHREAD_H */
