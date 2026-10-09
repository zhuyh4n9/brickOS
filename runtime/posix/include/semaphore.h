/*
 * brickOS prototype v0.2.0 — 信号量(semaphore.h; runtime/posix 插件提供)
 *
 * 底座 = core 的 `br_sem_t`(零分配等待链, `3-01` §7)。`sem_post` 可以直接在 ISR 里调
 * (core 的 `br_sem_give` 是 ISR-safe 的, `3-02` §11.3)—— 这是本服务少数几条"从
 * 中断到线程"的合法通道之一。
 *
 * ★ `sem_t` 直接**就是** `br_sem_t`: 布局公开 + 有静态初始化器(`BR_SEM_INIT_VALUE`),
 *   所以 `sem_init` 与静态初始化都能用, 不存在"懒初始化"的歧义。
 * ★ `sem_getvalue` 是**观测**(core 的 `br_sem_count`), 不改变语义。
 * ★ `sem_open`/`sem_close`/`sem_unlink`(具名信号量)**不在本代**: 它们需要一个
 *   内核对象命名空间, 那是 v2 的话题 —— 不声明 = 编译期暴露。
 */
#ifndef BR_POSIX_SEMAPHORE_H
#define BR_POSIX_SEMAPHORE_H

#include <sys/types.h>
#include <br/core/br_sync.h>
#include <br/core/br_time.h>

typedef br_sem_t sem_t;

#define BR_POSIX_SEM_VALUE_MAX  0x7fffffffu

int sem_init(sem_t *s, int pshared, unsigned value);
int sem_destroy(sem_t *s);          /* 空操作 ⇒ 0(理由同 pthread_*_destroy) */
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *abstime);
int sem_post(sem_t *s);
int sem_getvalue(sem_t *s, int *out);

#endif /* BR_POSIX_SEMAPHORE_H */
