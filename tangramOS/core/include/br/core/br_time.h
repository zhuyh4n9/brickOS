/*
 * brickOS prototype v0.1.0 — 延时与时钟契约
 *
 * 与设计文档的对应(在 tangramOS-Design 分支上):
 *   - `br_time_t` = uint64 微秒(3-01 §14 CA-1) —— 本头文件照抄该决策, 不另立单位
 *   - `br_clock_now()`(3-01 §4)在 v0.1.0 只取"单调读数"这一半语义
 *   - **刻意缺席**: br_task_sleep / br_task_sleep_until / 超时表 / 调度器。
 *     那三个都要调度器才成立(M1); v0.1.0 无调度器, 只有忙等延迟。
 *
 * 命名说明: 本头文件里的 `br_delay_us`/`br_delay_ms` 是**原型期临时出入口**
 *   (设计清单里没有它们 —— 设计里"睡眠"一律是 br_task_sleep, 因为它必然阻塞切换)。
 *   等 M1 调度器落地, 这两个函数要么删除、要么降级为 platform 内部件。
 */
#ifndef BR_CORE_TG_TIME_H
#define BR_CORE_TG_TIME_H

#include <br/core/br_types.h>

/* 微秒计数的绝对时间点(设计侧 CA-1: uint64 微秒)。 */
typedef br_u64 br_time_t;

#define BR_US_PER_MS   1000u
#define BR_US_PER_SEC  1000000u

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
 * 忙等延迟。无语义上的"可被唤醒", 不交出 CPU(v0.1.0 没有可交出的对象)。
 * 保证**不早醒**(设计侧 br_task_sleep 语义: 不早醒, 晚到无上界) ——
 * 因此换算按 ticks/us **向上取整**。
 */
void br_delay_us(br_time_t us);
void br_delay_ms(br_time_t ms);

#endif /* BR_CORE_TG_TIME_H */
