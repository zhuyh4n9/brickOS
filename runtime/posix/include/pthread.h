/*
 * POSIX threads mapped onto core's br_task_* and br_sync_* calls. Notes:
 * - Stack is caller-provided by core's contract, but pthread_create has no stack argument:
 *   this layer carves one from the core heap (pthread_attr_setstacksize changes the size,
 *   default 4 KiB). Thread count is bounded by heap and BR_TASK_MAX (8); failure gives EAGAIN.
 * - join needs a second table: br_task_join returns int exit_code only, so pthread_exit(v)
 *   and "entry returns v" record v here, and join reads it back.
 * - pthread_detach reclaims lazily: finished detached threads are joined on the next entry
 *   into any pthread call of this layer (semantics hold, timing is inexact).
 * - errno and pthread_key_* slots live in this layer's thread record (br_posix_errno()).
 * - pthread_mutex_t adds type/owner/count around core's plain br_mutex_t for
 *   RECURSIVE/ERRORCHECK; destroy is a no-op returning 0 (core has no destroy).
 * - Not provided: pthread_cancel/cleanup handlers, robust mutex (EOWNERDEAD), same-thread
 *   recursive read on rwlocks, signals; callers see the missing symbol at compile time.
 */
#ifndef BR_POSIX_PTHREAD_H
#define BR_POSIX_PTHREAD_H

#include <sys/types.h>
#include <time.h>
#include <br/core/br_sched.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>

/* ---- thread ---- */

typedef br_thread_t *pthread_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

#define BR_POSIX_STACK_DEFAULT  4096u
#define BR_PTHREAD_NAME_MAX     16u

typedef struct pthread_attr {
    size_t stack_size;      /* 0 ⇒ use default (BR_POSIX_STACK_DEFAULT) */
    void  *stack;           /* non-NULL ⇒ use caller-provided stack (then join/reap does not return it) */
    int    detachstate;     /* PTHREAD_CREATE_JOINABLE | _DETACHED */
    size_t guard_size;      /* recorded; core has no guard page ⇒ only affects get (honest) */
} pthread_attr_t;

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **retval);
void pthread_exit(void *retval);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_yield(void);
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

/* Thread name (diagnostics). t == BR_NULL = current thread. */
int pthread_setname_np(pthread_t t, const char *name);
int pthread_getname_np(pthread_t t, char *buf, size_t len);

/* ---- once ---- */

typedef struct pthread_once {
    br_mutex_t   m;         /* statically initialized ⇒ ready to use */
    volatile int done;
} pthread_once_t;

#define PTHREAD_ONCE_INIT   { BR_MUTEX_INIT_VALUE, 0 }

int pthread_once(pthread_once_t *once, void (*init)(void));

/* ---- TLS key ---- */

#define PTHREAD_KEYS_MAX              8u
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

typedef unsigned pthread_key_t;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
void *pthread_getspecific(pthread_key_t key);

/* ---- mutex ---- */

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
    br_mutex_t   m;         /* kernel object (plain mutex) */
    unsigned     type;      /* NORMAL | RECURSIVE | ERRORCHECK */
    br_thread_t *owner;     /* RECURSIVE/ERRORCHECK owner (diagnostics + re-entry check) */
    unsigned     count;     /* RECURSIVE re-entry count */
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
int pthread_mutex_destroy(pthread_mutex_t *m);      /* no-op ⇒ 0 (see file header) */
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *abstime);
int pthread_mutex_unlock(pthread_mutex_t *m);

/* ---- cond ---- */

typedef struct pthread_condattr {
    int clock;              /* CLOCK_MONOTONIC (the only available time base) */
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
int pthread_cond_destroy(pthread_cond_t *c);        /* no-op ⇒ 0 */
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

/* ---- rwlock ---- */

#define PTHREAD_RWLOCK_PREFER_READER_NP              0
#define PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP 1

typedef struct pthread_rwlockattr {
    int kind;
    int pshared;
} pthread_rwlockattr_t;

typedef struct pthread_rwlock {
    br_mutex_t   m;             /* protects all fields below */
    br_cond_t    c;
    unsigned     readers;
    br_thread_t *writer;        /* non-NULL = writer holds it */
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

/* ---- barrier ---- */

#define PTHREAD_BARRIER_SERIAL_THREAD  (-1)

typedef struct pthread_barrierattr {
    int pshared;
} pthread_barrierattr_t;

typedef struct pthread_barrier {
    br_mutex_t m;
    br_cond_t  c;
    unsigned   count;           /* participants per generation */
    unsigned   waiting;         /* arrived in this generation */
    unsigned   generation;      /* generation counter (wakeup criterion) */
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
