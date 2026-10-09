/*
 * tests/host/rr_test.c — 宿主侧 **sched/rr**(时间片轮转抢占)语义门禁(不需要 QEMU)
 *
 * ## 宿主怎么"造出中断"
 *
 * 目标上抢占的驱动链是: timer ISR → `br_sched_on_tick()` → 插件的 `on_tick`(递减时间片)
 * → `br_sched_request_resched()`; 然后 **IRQ 出口** 的 `br_sched_irq_epilogue()` 换栈。
 * 宿主没有真中断, 但这两步都是普通 C 调用 —— 用例只要在**某个线程正在跑的时刻**调用
 * 它们, 就等价于"这次 tick 打断了这个线程":
 *
 *     host_tick_from_current():  推进假时钟 → br_sched_on_tick() → br_sched_irq_epilogue()
 *
 * 关键性质: 调用者就是"被打断的线程" —— 于是 `resched_from_irq()` 保存的现场真的是
 * 它的, 而它被再次选中时会从这次调用里返回。这与目标上"ISR 在谁的栈上跑"逐点对应。
 *
 * ## 判据(report 的 trace 是本节的核心证据)
 *
 *   HOST-RR-SLICE      两个不 yield 的忙线程, 各自在自己的循环里"挨 tick";
 *                      时间片 = 2 tick ⇒ 交替序列必须是 AABB AABB(而不是 AAAA BBBB)
 *   HOST-RR-NOTICK     **不发 tick 就绝不抢占**: 忙线程把整段循环跑完, 对手一次都没跑
 *                      —— 与上一条配对, 说明"抢占的唯一来源是 tick"
 *   HOST-RR-RESCHED    need_resched 的置位/消费(含"只有自己可跑时不切栈")
 *   HOST-RR-CONF       core 的 br_sched_selftest()(kind=preempt ⇒ 会跑 TC-TASK-102/103)
 *
 * 输出协议: [HOSTTEST] PASS|FAIL … SUMMARY; 有失败 ⇒ 退出码非 0。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include <br/sched/rr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_sched.h"

int br_sched_selftest(void);

/* =====================================================================
 * 用例框架
 * ===================================================================== */

static int s_hpass;
static int s_hfail;

static void host_check(br_bool ok, const char *tag, const char *what)
{
    if (ok != BR_FALSE) {
        s_hpass++;
        printf("[HOSTTEST] PASS %s %s\n", tag, what);
    } else {
        s_hfail++;
        printf("[HOSTTEST] FAIL %s %s\n", tag, what);
    }
}

#define HOST_STACK_BYTES  8192u
#define HOST_STACK_SLOTS  8u
#define TICK_US           100000u   /* 宿主用例自己的虚拟 tick(与产品 [kernel].hz 解耦; 判据不依赖这个数) */

static br_u8 s_stk[HOST_STACK_SLOTS][HOST_STACK_BYTES] BR_ALIGN(16);

static br_task_attr_t host_attr(br_u32 slot, const char *name)
{
    br_task_attr_t a;

    a.name       = name;
    a.stack      = (void *)(s_stk[slot % HOST_STACK_SLOTS] + HOST_STACK_BYTES);
    a.stack_size = HOST_STACK_BYTES;
    a.prio       = 0u;
    a.flags      = 0u;
    return a;
}

/* "打断当前线程的一次 tick"(见文件头)。 */
static void host_tick_from_current(void)
{
    host_sched_advance_us(TICK_US);
    host_sched_tick();
    br_sched_irq_epilogue();
}

/* ---- ① 时间片轮转: 忙线程被 tick 抢占 ⇒ AABB AABB ---- */

#define RR_ITERS 4

static volatile char s_trace[32];
static volatile int  s_trace_n;
static volatile br_u32 s_runs_a;
static volatile br_u32 s_runs_b;

static void trace_put(char c)
{
    if (s_trace_n < (int)sizeof(s_trace)) {
        s_trace[s_trace_n] = c;
        s_trace_n++;
    }
}

/* 忙线程: 不 yield(与 TC-TASK-101 的 busy 同型), 只在**自己的循环里**挨 tick。
 * 这正是目标上"timer 打断了正在跑的我"的形态。 */
static void host_busy_ticks(void *arg)
{
    const char c = (char)(br_intptr_t)arg;

    for (int i = 0; i < RR_ITERS; i++) {
        trace_put(c);
        if (c == 'A') {
            s_runs_a++;
        } else {
            s_runs_b++;
        }
        host_tick_from_current();
    }
}

static void host_case_slice(void)
{
    br_task_attr_t aa = host_attr(0u, "rkA");
    br_task_attr_t ab = host_attr(1u, "rkB");
    br_thread_t *ta = BR_NULL;
    br_thread_t *tb = BR_NULL;

    s_trace_n = 0;
    s_runs_a = 0u;
    s_runs_b = 0u;
    for (br_u32 i = 0u; i < (br_u32)sizeof(s_trace); i++) {
        s_trace[i] = '.';
    }

    const int r1 = br_task_create(&ta, &aa, host_busy_ticks, (void *)(br_intptr_t)'A');
    const int r2 = br_task_create(&tb, &ab, host_busy_ticks, (void *)(br_intptr_t)'B');

    /* 自己让出, 让 A/B 去跑(它们在循环里自产 tick); 醒来时两者都已结束。 */
    while ((ta != BR_NULL && br_task_state(ta) != BR_TASK_ZOMBIE) ||
           (tb != BR_NULL && br_task_state(tb) != BR_TASK_ZOMBIE)) {
        br_task_yield();
    }

    const int j1 = (ta != BR_NULL) ? br_task_join(ta, BR_NULL) : BR_ERR(BR_EINVAL);
    const int j2 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(BR_EINVAL);

    const char *p = (const char *)s_trace;
    printf("[HOSTTEST] TRACE rr slice seq=%.8s (n=%d, slice=%u ticks)\n",
           p, (int)s_trace_n, (unsigned)RR_SLICE_TICKS);

    /* 判据: 严格 AABB AABB —— 每段同字母**不超过时间片**, 且两者交替。
     * 反例(CCCD 之类)会立刻红: 那意味着"时间片没生效"或"抢占发生在错误的地方"。 */
    const br_bool seq_ok = (s_trace_n == 2 * RR_ITERS) &&
                           (p[0] == 'A') && (p[1] == 'A') &&
                           (p[2] == 'B') && (p[3] == 'B') &&
                           (p[4] == 'A') && (p[5] == 'A') &&
                           (p[6] == 'B') && (p[7] == 'B');

    host_check((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) && seq_ok,
               "HOST-RR-SLICE",
               "不 yield 的忙线程被时间片抢占: 交替序列 == AABBAABB(每段恰 2 tick)");
    host_check((s_runs_a == RR_ITERS) && (s_runs_b == RR_ITERS),
               "HOST-RR-SLICE",
               "两个忙线程各跑满自己的全部迭代(抢占没有吞掉任何一次执行)");
}

/* ---- ② 没有 tick ⇒ 绝不抢占(与 ① 配对: 抢占的唯一来源是 tick) ---- */

static volatile int s_notick_victim;

static void host_victim(void *arg)
{
    (void)arg;
    s_notick_victim++;
}

static void host_busy_notick(void *arg)
{
    (void)arg;

    /* 一整个忙循环**不发 tick** ⇒ 对手不该被切上来(preempt 只在 tick 上发生)。 */
    for (volatile br_u32 i = 0u; i < 200000u; i++) {
        if (s_notick_victim != 0) {
            s_notick_victim = 99;      /* 标记: 真的被抢占了 */
        }
    }
    br_task_yield();                   /* 显式让出: 此刻对手才该跑 */
}

static void host_case_no_tick(void)
{
    br_task_attr_t ab = host_attr(2u, "ntBusy");
    br_task_attr_t av = host_attr(3u, "ntVictim");
    br_thread_t *tb = BR_NULL;
    br_thread_t *tv = BR_NULL;

    s_notick_victim = 0;

    const int r1 = br_task_create(&tb, &ab, host_busy_notick, BR_NULL);
    const int r2 = br_task_create(&tv, &av, host_victim, BR_NULL);
    const int j1 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(BR_EINVAL);
    const int j2 = (tv != BR_NULL) ? br_task_join(tv, BR_NULL) : BR_ERR(BR_EINVAL);

    host_check((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) &&
               (s_notick_victim == 1),
               "HOST-RR-NOTICK",
               "不发 tick ⇒ 忙线程不被抢占(抢占的唯一来源是 tick; 显式 yield 后对手才跑)");
}

/* ---- ③ need_resched 的置位与消费 ---- */

static void host_case_resched(void)
{
    const br_bool before = br_sched_resched_pending();

    br_sched_request_resched();
    const br_bool set = br_sched_resched_pending();

    /* 此刻只有 main(不在队列)+ supervisor ⇒ pick_next 只会选出自己:
     * 出口应当"消费掉请求但不切栈"(切给自己没有意义)。 */
    br_sched_irq_epilogue();
    const br_bool consumed = br_sched_resched_pending();
    const br_bool still_self = (br_task_name(br_task_self()) != NULL) &&
                               (strcmp(br_task_name(br_task_self()), "supervisor") == 0);

    host_check((before == BR_FALSE) && (set == BR_TRUE) && (consumed == BR_FALSE) && still_self,
               "HOST-RR-RESCHED",
               "need_resched: 置位可见 → 出口消费 → 无别人可跑时不切栈");
}

/* =====================================================================
 * 监督线程
 * ===================================================================== */

static void host_supervisor(void *arg)
{
    (void)arg;

    br_log_info("[HOSTTEST] supervisor 开始: self=%s", br_task_name(br_task_self()));

    host_case_slice();
    host_case_no_tick();
    host_case_resched();

    /* core 的套件: kind=preempt ⇒ 里面会跑 TC-TASK-102(时间片抢占)与 TC-TASK-103
     * (抢占与锁/临界区的交互), 而 TC-TASK-101 的那条 coop 判据会被"按 kind 分叉"跳过。
     * ★ 打开**自动 tick 源**: 那两条用例的忙循环不 yield, 只能靠"它自己读时钟"被打断
     *   (见 host_sched.h); 本文件前面的三个用例用的是确定性注入, 所以那时它是关的。 */
    host_sched_auto_tick(BR_TRUE);
    br_sched_selftest();
    host_sched_auto_tick(BR_FALSE);

    host_check(host_sched_idle_calls() > 0u, "HOST-RR-IDLE",
               "pick_next 返回 NULL ⇒ core 进 idle(宿主由钩子代 WFI)并被 tick 唤醒");
    host_check(br_sched_task_count() == 2u, "HOST-RR-POOL",
               "测试线程全部被 join 回收(只剩 main + supervisor)");
    host_check(host_sched_watch_hits() == 0u, "HOST-RR-CONF",
               "br_sched_selftest 无 [TASKCONF] FAIL");

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d (idle_calls=%u)\n",
           s_hpass, s_hfail, s_hpass + s_hfail, (unsigned)host_sched_idle_calls());

    exit((s_hfail == 0) ? 0 : 1);
}

int main(void)
{
    printf("[HOSTTEST] ==== sched/rr: 时间片轮转抢占(kind=preempt) ====\n");

    host_sched_watch("[TASKCONF] FAIL");

    const int r = rr_early_init();
    if (r != 0) {
        printf("[HOSTTEST] FAIL rr_early_init rc=%d\n", r);
        return 1;
    }
    host_sched_main(host_supervisor);
}
