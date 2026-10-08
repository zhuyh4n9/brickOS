/*
 * brickOS prototype v0.1.0 — 延时与时钟契约
 *
 * 与设计文档的对应(在 brickOS-Design 分支上):
 *   - `br_time_t` = uint64 微秒(3-01 §14 CA-1) —— 本头文件照抄该决策, 不另立单位
 *   - `br_clock_now()`(3-01 §4)在 v0.1.0 只取"单调读数"这一半语义
 *   - **刻意缺席**: br_task_sleep / br_task_sleep_until 的**实现**(在 br_sched.h 里声明,
 *     由 core 的调度框架实现) —— v0.2 起 br_delay_* 是"忙等"的临时出入口, 调用点应
 *     逐步换成 br_task_sleep(见 WORKAROUNDS.md 的 br-wa-boot-001)。
 *
 * 命名说明: 本头文件里的 `br_delay_us`/`br_delay_ms` 是**原型期临时出入口**
 *   (设计清单里没有它们 —— 设计里"睡眠"一律是 br_task_sleep, 因为它必然阻塞切换)。
 *   **v0.2 起 `br_task_sleep` 已在 core 的调度框架里落地** ⇒ 这两个函数**只是忙等**
 *   (占着 CPU 数拍子, 不交出 CPU、不可被唤醒), **不再是"睡眠"的实现**; 需要"让出
 *   CPU 等一段时间"一律用 `br_task_sleep()`(`br_sched.h`)。它们保留只为调度器就位
 *   之前的窗口(platform early_init)与 platform 内部件。
 */
#ifndef BR_CORE_BR_TIME_H
#define BR_CORE_BR_TIME_H

#include <br/core/br_types.h>

/* 微秒计数的绝对时间点(设计侧 CA-1: uint64 微秒)。 */
typedef br_u64 br_time_t;

#define BR_US_PER_MS   1000u
#define BR_US_PER_SEC  1000000u

/*
 * 超时的三态(设计 `3-01` §14 CA-4):
 *   BR_TIMEOUT_ZERO = 非阻塞(trylock 语义)
 *   BR_TIMEOUT_INF  = 无限等待(不是"很久" —— 是"没有期限")
 * 用户给的相对值走 `br_deadline_from_now()`, 统一变成**绝对期限**(tickless 友好)。
 */
#define BR_TIMEOUT_ZERO  ((br_time_t)0u)
#define BR_TIMEOUT_INF   ((br_time_t)~(br_time_t)0u)

/* 相对微秒 → 绝对期限(饱和: 溢出不回绕成"立刻超时")。 */
br_time_t br_deadline_from_now(br_time_t rel_us);

/*
 * 用平台 arch timer 频率初始化时钟换算。必须在 br_clock_now/br_delay_* 之前调用一次,
 * 且**必须早于 br_log_init()**(日志时间戳要用它)。
 */
void br_clock_init(void);

/* 单调不减的"自启动以来的微秒数"。无溢出保护语义承诺(2^64 us ≈ 58 万年)。 */
br_time_t br_clock_now(void);

/* 平台时钟频率(Hz), 仅用于日志与诊断。br_clock_init() 之后才有效。 */
br_u64 br_clock_freq_hz(void);

/* 换算基准: 每毫秒多少拍(= freq/1000)。诊断用 —— 它决定了分辨率。 */
br_u64 br_clock_ticks_per_ms(void);

/*
 * 忙等延迟(**只忙等, 不是睡眠**; v0.2 起睡眠的正式入口是 `br_task_sleep()`)。
 * 无语义上的"可被唤醒", 不交出 CPU ⇒ 只适合"调度器还不存在"的窗口(platform
 * early_init)与 platform 内部件。保证**不早醒**(与 br_task_sleep 同一口径:
 * 不早醒, 晚到无上界) —— 因此换算按 ticks/us **向上取整**。
 */
void br_delay_us(br_time_t us);
void br_delay_ms(br_time_t ms);

#endif /* BR_CORE_BR_TIME_H */
