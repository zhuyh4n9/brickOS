/*
 * brickOS prototype v0.2.0 — 时间(time.h; runtime/posix 插件提供)
 *
 * ★★ **没有墙钟**: 原型只有单调计数器(`br_clock_now()`, 自启动以来的微秒数)与平台
 *    arch timer, **没有 RTC/纪元源**(设计侧 P-1 缺口)。于是:
 *     - `CLOCK_MONOTONIC` **可用且语义正确**(单调不减, 见 `3-01` INV-2);
 *     - `CLOCK_REALTIME` **声明了但返回 -ENOTSUP** —— 与其返回一个假装是"1970 年至今"
 *       的数字, 不如让调用方在运行期就看见"这个台上没有墙钟"(子集诚实义务)。
 *   这条口径与 `11-02` §2.4 的 TR-C 判定一致; `gettimeofday`/`time` **不在本头里**。
 *
 * ★ 分辨率 = **一拍**(`1/HZ`): 平台 timer 是周期 tick(节拍 `HZ` 来自
 *   `product.toml [kernel].hz`, 缺省 200 ⇒ 5 ms; ADR-0017), tickless 还没做(P-2)。
 *   所以 `nanosleep` 的**语义**是"不早醒", 而**实际晚到**可达一个 tick ——
 *   `clock_getres` 如实报这个粒度。这是本代最容易被误用的地方, 故写在这里。
 */
#ifndef BR_POSIX_TIME_H
#define BR_POSIX_TIME_H

#include <br/core/br_time.h>   /* BR_CFG_TICK_HZ: 分辨率只有一处真值(ADR-0017) */
#include <sys/types.h>

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

typedef int clockid_t;

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

/* tickless 未落地 ⇒ 这是**真实**粒度, 不是"设计值"(= 一拍 = 1/HZ; 见上头说明) */
#define BR_POSIX_CLOCK_RES_NS  (1000000000L / (long)BR_CFG_TICK_HZ)

int clock_gettime(clockid_t id, struct timespec *ts);
int clock_getres (clockid_t id, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* BR_POSIX_TIME_H */
