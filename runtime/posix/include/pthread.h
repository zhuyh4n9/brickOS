/*
 * brickOS prototype v0.2.0 — 线程(pthread.h; runtime/posix 插件提供)
 *
 * ★★ 这是**映射层**, 不是第二套线程实现: 每个 `pthread_*` 都落到 core 的
 *    `br_task_*` / `br_sync_*` 上(`3-01` §2/§3)。几条平台差异必须知道:
 *
 *   ① **栈要调用方给**: `br_task_create` 的契约是"栈由调用方提供"(`br_sched.h`),
 *      而 `pthread_create` 的 POSIX 签名里没有栈。本代的做法 = **本服务从 core 堆上
 *      切一块**(`pthread_attr_setstacksize` 可改大小, 默认 4 KiB)。线程数受堆与
 *      `BR_TASK_MAX`(8)双重约束; 创建失败返回 `EAGAIN`。
 *   ② **`join` 的返回值有第二张表**: core 的 `br_task_join` 只回 `int exit_code`,
 *      而 POSIX 要 `void *retval`。于是本服务维护一张 `{线程, retval, 栈}` 的小表,
 *      `pthread_exit(v)` 与"入口函数返回 v"都把 v 记进去, `join` 取出来。
 *   ③ **`pthread_detach` = 惰性回收**(v0.2 起): core 只有 `br_task_join` 一条回收
 *      ZOMBIE 的路径 ⇒ 本层把 detached 线程记在表里, **在下一次进入本层任何 pthread
 *      调用时**把"已退出(state == ZOMBIE)且 detached"的线程 join 掉(顺带还栈)。
 *      语义上"资源最终被回收"; 代价是回收时机不精确(仍有 pthread 调用才回收)——
 *      如实登记在 ADR-0019, 不是假装 `br_task_detach`。
 *   ④ **每线程 `errno` 在本层**(v0.2 起): `errno` 是宏, 指向本层线程记录里的槽位
 *      (`br_posix_errno()`)⇒ 线程 A 的失败不会被线程 B 读走。core 仍没有通用 TLS
 *      槽位(`11-02` P-4), 但 pthread 层自己就有"每线程记录"这张表, 于是不必动 core。
 *   ⑤ **`pthread_key_*`(TLS) 同理**: 槽位存在本层线程记录里(见 ④)。
 *
 * ★ 同步对象: mutex/cond 的**内核对象**直接用 core 的类型(`br_mutex_t`/`br_cond_t`,
 *   布局公开), 但 `pthread_mutex_t` 在外层加了 `type/owner/count` 三个字段 ——
 *   POSIX 的 RECURSIVE/ERRORCHECK 语义(core 的 mutex 是普通互斥量)由此在层内实现。
 *   静态初始化器 `PTHREAD_MUTEX_INITIALIZER` 覆盖全部字段。
 *   `destroy` 是**空操作返回 0**: core 没有 `br_mutex_destroy`(`11-02` 的 P-5),
 *   而 POSIX 要求销毁未使用的互斥量必须成功(Linux 亦然)⇒ 空操作是**正确**答案;
 *   真正的缺口是"销毁后仍被引用"检测不到, 登记在 ADR-0014/0019。
 *
 * ★ **不做**(子集诚实义务; 设计 `11-02` 的 TR-C 与本层边界):
 *   `pthread_cancel`/cleanup handler(取消点与清理栈需要额外语义)、robust mutex
 *   (`EOWNERDEAD`)、读写锁的**同线程递归读**、信号相关(`pthread_kill`/`sigmask`)。
 *   需要它们的调用方会在**编译期**看到符号缺失 —— 这比运行期假装成功诚实。
 */
#ifndef BR_POSIX_PTHREAD_H
#define BR_POSIX_PTHREAD_H

#include <sys/types.h>
#include <time.h>                 /* struct timespec / CLOCK_MONOTONIC */
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>

/* ==================================================================== 线程 */

typedef br_thread_t *pthread_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define BR_POSIX_STACK_DEFAULT  4096u
#define BR_PTHREAD_NAME_MAX     16u

typedef struct pthread_attr {
    size_t stack_size;      /* 0 ⇒ 用默认值(BR_POSIX_STACK_DEFAULT) */
    void  *stack;           /* 非空 ⇒ 用调用方给的栈(此时 join/reap 不归还) */
    int    detachstate;     /* PTHREAD_CREATE_JOINABLE | _DETACHED */
    size_t guard_size;      /* 记录; core 没有 guard 页 ⇒ 只影响 get(如实) */
} pthread_attr_t;

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **retval);
void pthread_exit(void *retval);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_yield(void);
/* detached 线程的资源回收是**惰性**的(见文件头 ③): 语义成立, 时机不精确。 */
int pthread_detach(pthread_t t);

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size);
int pthread_attr_setstack(pthread_attr_t *a, void *stack, size_t size);
int pthread_attr_getstack(const pthread_attr_t *a, void **stack, size_t *size);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state);
int pthread_attr_setguardsize(pthread_attr_t *a, size_t size);
int pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size);

/* 线程名(诊断; glibc 的 `_np` 面。`t == BR_NULL` = 当前线程)。 */
int pthread_setname_np(pthread_t t, const char *name);
int pthread_getname_np(pthread_t t, char *buf, size_t len);

/* ==================================================================== once */

typedef struct pthread_once {
    br_mutex_t   m;         /* 静态初始化 ⇒ 直接可用 */
    volatile int done;
} pthread_once_t;

#define PTHREAD_ONCE_INIT   { BR_MUTEX_INIT_VALUE, 0 }

int pthread_once(pthread_once_t *once, void (*init)(void));

/* ==================================================================== TLS key */

#define PTHREAD_KEYS_MAX              8u
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

typedef unsigned pthread_key_t;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

/* ==================================================================== mutex */

#define PTHREAD_MUTEX_NORMAL      0
#define PTHREAD_MUTEX_RECURSIVE   1
#define PTHREAD_MUTEX_ERRORCHECK  2
#define PTHREAD_MUTEX_DEFAULT     PTHREAD_MUTEX_NORMAL

#define PTHREAD_PROCESS_PRIVATE   0
#define PTHREAD_PROCESS_SHARED    1

#define PTHREAD_PRIO_NONE         0
#define PTHREAD_PRIO_INHERIT      1
#define PTHREAD_PRIO_PROTECT      2

typedef struct pthread_mutexattr {
    unsigned type;
    int      pshared;
    int      protocol;
} pthread_mutexattr_t;

typedef struct pthread_mutex {
    br_mutex_t   m;         /* 内核对象(普通互斥量) */
    unsigned     type;      /* NORMAL | RECURSIVE | ERRORCHECK */
    br_thread_t *owner;     /* RECURSIVE/ERRORCHECK 的属主(诊断 + 重入判定) */
    unsigned     count;     /* RECURSIVE 的重入计数 */
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER \
    { BR_MUTEX_INIT_VALUE, PTHREAD_MUTEX_NORMAL, BR_NULL, 0u }

int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);
int pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int pshared);
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *a, int *pshared);
int pthread_mutexattr_setprotocol(pthread_mutexattr_t *a, int protocol);
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *a, int *protocol);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);      /* 空操作 ⇒ 0(见文件头) */
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime);
int pthread_mutex_unlock(pthread_mutex_t *m);

/* ==================================================================== cond */

typedef struct pthread_condattr {
    int clock;              /* CLOCK_MONOTONIC(唯一可用时基) */
    int pshared;
} pthread_condattr_t;

typedef struct pthread_cond {
    br_cond_t c;
    int       clock;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER    { BR_COND_INIT_VALUE, CLOCK_MONOTONIC }

int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_destroy(pthread_condattr_t *a);
int pthread_condattr_setclock(pthread_condattr_t *a, int clock);
int pthread_condattr_getclock(const pthread_condattr_t *a, int *clock);
int pthread_condattr_setpshared(pthread_condattr_t *a, int pshared);
int pthread_condattr_getpshared(const pthread_condattr_t *a, int *pshared);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);        /* 空操作 ⇒ 0 */
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

/* ==================================================================== rwlock */

#define PTHREAD_RWLOCK_PREFER_READER_NP              0
#define PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP 1

typedef struct pthread_rwlockattr {
    int kind;
    int pshared;
} pthread_rwlockattr_t;

typedef struct pthread_rwlock {
    br_mutex_t   m;             /* 保护下面全部字段 */
    br_cond_t    c;
    unsigned     readers;
    br_thread_t *writer;        /* 非空 = 写者持有 */
    unsigned     writers_waiting;
    int          kind;
} pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER \
    { BR_MUTEX_INIT_VALUE, BR_COND_INIT_VALUE, 0u, BR_NULL, 0u, \
      PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP }

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *attr);
int pthread_rwlock_destroy(pthread_rwlock_t *rw);
int pthread_rwlockattr_init(pthread_rwlockattr_t *a);
int pthread_rwlockattr_destroy(pthread_rwlockattr_t *a);
int pthread_rwlockattr_setkind_np(pthread_rwlockattr_t *a, int kind);
int pthread_rwlockattr_getkind_np(const pthread_rwlockattr_t *a, int *kind);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *a, int pshared);
int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *a, int *pshared);

int pthread_rwlock_rdlock(pthread_rwlock_t *rw);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rw);
int pthread_rwlock_timedrdlock(pthread_rwlock_t *rw, const struct timespec *abstime);
int pthread_rwlock_wrlock(pthread_rwlock_t *rw);
int pthread_rwlock_trywrlock(pthread_rwlock_t *rw);
int pthread_rwlock_timedwrlock(pthread_rwlock_t *rw, const struct timespec *abstime);
int pthread_rwlock_unlock(pthread_rwlock_t *rw);

/* ==================================================================== barrier */

#define PTHREAD_BARRIER_SERIAL_THREAD  (-1)

typedef struct pthread_barrierattr {
    int pshared;
} pthread_barrierattr_t;

typedef struct pthread_barrier {
    br_mutex_t m;
    br_cond_t  c;
    unsigned   count;           /* 每代参与者数 */
    unsigned   waiting;         /* 本代已到数 */
    unsigned   generation;      /* 换代计数(唤醒判据) */
} pthread_barrier_t;

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *attr,
                         unsigned count);
int pthread_barrier_destroy(pthread_barrier_t *b);
int pthread_barrierattr_init(pthread_barrierattr_t *a);
int pthread_barrierattr_destroy(pthread_barrierattr_t *a);
int pthread_barrierattr_setpshared(pthread_barrierattr_t *a, int pshared);
int pthread_barrierattr_getpshared(const pthread_barrierattr_t *a, int *pshared);
int pthread_barrier_wait(pthread_barrier_t *b);

#endif /* BR_POSIX_PTHREAD_H */
