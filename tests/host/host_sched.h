/*
 * tests/host/host_sched.h — 宿主侧调度测试装置(共享装置, 不是第二份目标实现)
 *
 * 为什么要有这个共享装置: 宿主上跑"调度框架 + 某个调度器插件"的语义用例时, 每个用例
 * 文件都要重复同一套替身(假时钟 / L2 / 日志 / panic / ucontext 切换层 / trampoline)。
 * 三份 `sched-test`(coop)、`rr-test`、`work-test` 之前会把这段抄三遍 —— 那正是"第二处
 * 真值"的开始(改一处忘一处, 于是某个用例偷偷测着别的语义)。⇒ 收拢到这里。
 *
 * 层次切分(与 ADR-0006 §4 的裁定一致):
 *   切换层 = `core/src/sched/switch.S`(**只在 aarch64 编**);
 *   策略层 = `core/src/sched/sched_core.c` + 调度器插件(**两边都编**)。
 * 本装置提供宿主版的切换层(ucontext: 真的换栈, 不是"假切换")与目标侧的桩。
 *
 * 输出协议(照 tests/host/mem_test.c):
 *   [HOSTTEST] PASS|FAIL <tag> <desc> … [HOSTTEST] SUMMARY pass=N fail=M total=T
 */
#ifndef BR_TESTS_HOST_SCHED_H
#define BR_TESTS_HOST_SCHED_H

#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* 建 supervisor 线程并进入 `br_sched_run()`(不返回; supervisor 体负责跑用例并 exit)。
 * 调用前请先跑所选调度器插件的 `*_early_init()`(它完成 `br_sched_register`)。 */
void host_sched_main(void (*supervisor)(void *));

/* 假时钟: 当前值 / 前进(前进**不**投递 tick —— 用例自己决定何时 tick)。 */
br_time_t host_sched_now(void);
void      host_sched_advance_us(br_time_t us);

/* 进 idle 的次数(core 在"无人可跑"时调宿主钩子; 每次前进 100 us 并投递一次 tick)。 */
br_u32 host_sched_idle_calls(void);

/* 让 `br_log_write` 顺带盯住一个子串(命中即计数); 用来把套件打出的
 * `[XXXCONF] FAIL` 变成宿主的退出码判据。 */
void host_sched_watch(const char *needle);
br_u32 host_sched_watch_hits(void);

/* 向 **core 的 tick 驱动点**投递一次 tick(等价于目标上 timer ISR 的那一行)。 */
void host_sched_tick(void);

/*
 * 把"读时钟"变成**异步 tick 源**(默认关)。
 *
 * 目标上 timer ISR 是异步的: 一个不 yield 的忙循环照样会被 tick 打断。宿主上没有中断,
 * 唯一能"在别人跑的时候插进去"的位置就是**它自己读时钟的那一刻** —— 于是打开这个开关后,
 * `br_clock_now()` 每次推进 `HOST_CLOCK_STEP_US`, 累计到一个 tick 周期就在**关中断深度
 * 为 0** 时投递 `br_sched_on_tick()` + `br_sched_irq_epilogue()`(与目标上 ISR 出口做的事
 * 逐条相同)。L2 临界区里**不投递**(= 目标上中断被屏蔽), 但时间照走 —— 这正是
 * `TC-TASK-103`("临界区内不发生抢占")能在宿主上也有意义的原因。
 *
 * 为什么默认关: 需要**确定性 tick**的用例(如 rr-test 的 AABB 序列)自己注入,
 * 打开自动源会让"额外的一次 tick"混进序列。
 */
void host_sched_auto_tick(br_bool on);

#endif /* BR_TESTS_HOST_SCHED_H */
