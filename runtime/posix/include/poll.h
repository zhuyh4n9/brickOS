/*
 * brickOS prototype v0.2.0 — 事件等待(poll.h; runtime/posix 插件提供)
 *
 * ★ v1 的语义 = 设计 SD-7 的既有口径("**轮询 + 粗粒度 sleep**"): 逐 fd 查就绪位
 *   (`br_file_poll`), 没人就绪就 `br_task_sleep` 一小片再查, 直到超时。
 *   与 `11-02` §2.1 的 TR-B 判定一致; `poll_attach`(真正的 wait-queue)是 v2 面,
 *   且设计明确要求它与 preempt **同期**(R-S1)。
 * ★ 因此**实际唤醒延迟 ≈ 一个 tick(100 ms)**, 不是 `poll` 的 timeout 值。
 *   `timeout = 0` 是"只查一次"(立刻返回), `timeout < 0` 是"无限等"。
 */
#ifndef BR_POSIX_POLL_H
#define BR_POSIX_POLL_H

#include <sys/types.h>

struct pollfd {
    int   fd;
    short events;    /* 想等的事件(入参) */
    short revents;   /* 实际发生的事件(出参) */
};

typedef unsigned long nfds_t;

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif /* BR_POSIX_POLL_H */
