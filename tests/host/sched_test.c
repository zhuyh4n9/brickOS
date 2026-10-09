/*
 * tests/host/sched_test.c — 宿主侧调度框架 / **coop** 语义门禁(不需要 QEMU)
 *
 * 为什么在宿主跑: 就绪队列序 / block↔wake 状态机 / 超时表到期 / 池上限这些是**策略性质**,
 * 宿主上可以毫秒级反复跑; 同一批性质在 QEMU 上只能靠几条用例撞。宿主用 `ucontext.h`
 * 做**真的换栈**(不是"假切换"), 于是 `br_task_create`/`yield`/`block`/`join`/`sleep`
 * 全部走真实路径 —— 与目标上 `switch.S` 的语义一一对应。
 *
 * 共享装置(假时钟 / L2 / 日志 / panic / ucontext 切换层)在 `tests/host/host_sched.c`;
 * 本文件只放 **coop 特有的判据**与用例。
 *
 * ★ ADR-0011 之后镜像选的是 `sched/rr`(时间片抢占), 于是**这个文件是 coop 的唯一腿**:
 *   `TC-TASK-101`("不 yield 的忙线程不被切走")在目标上已经不跑了 —— 它正是 coop 与
 *   preempt 的分界, 而镜像里换成了 preempt。⇒ 本用例的红绿从此意味着"coop 还在"。
 *
 * 输出协议: [HOSTTEST] PASS|FAIL <tag> <desc> … SUMMARY; 有失败 ⇒ 退出码非 0。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include <br/sched/coop.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_sched.h"

/* TCB 公共头(`wait_status` 的 ICB 开窗是**调用方义务**, 用例自己履行)。 */
#include "sched_internal.h"

/* 自检套件入口的前置声明(ADR-0010: 它不再出现在 br_sched.h)。 */
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

/* ---- ① 就绪队列 FIFO 序: 三线程严格轮转 ABCABCABC ---- */

static volatile char s_fifo_seq[32];
static volatile int  s_fifo_n;

static void host_worker_seq(void *arg)
{
    const char c = (char)(br_intptr_t)arg;

    for (int i = 0; i < 3; i++) {
        if (s_fifo_n < 32) {
            s_fifo_seq[s_fifo_n] = c;
            s_fifo_n++;
        }
        br_task_yield();
    }
}

static void host_case_fifo(void)
{
    br_task_attr_t a1 = host_attr(0u, "hA");
    br_task_attr_t a2 = host_attr(1u, "hB");
    br_task_attr_t a3 = host_attr(2u, "hC");
    br_thread_t *ta = BR_NULL;
    br_thread_t *tb = BR_NULL;
    br_thread_t *tc = BR_NULL;
    int j1, j2, j3;

    s_fifo_n = 0;
    for (br_u32 i = 0u; i < 32u; i++) {
        s_fifo_seq[i] = '.';
    }

    const int r1 = br_task_create(&ta, &a1, host_worker_seq, (void *)(br_intptr_t)'A');
    const int r2 = br_task_create(&tb, &a2, host_worker_seq, (void *)(br_intptr_t)'B');
    const int r3 = br_task_create(&tc, &a3, host_worker_seq, (void *)(br_intptr_t)'C');

    j1 = (ta != BR_NULL) ? br_task_join(ta, BR_NULL) : BR_ERR(BR_EINVAL);
    j2 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(BR_EINVAL);
    j3 = (tc != BR_NULL) ? br_task_join(tc, BR_NULL) : BR_ERR(BR_EINVAL);

    const char *p = (const char *)s_fifo_seq;
    printf("[HOSTTEST] TRACE fifo seq=%.9s (n=%d)\n", p, (int)s_fifo_n);

    const br_bool ok = (r1 == 0) && (r2 == 0) && (r3 == 0) &&
                       (j1 == 0) && (j2 == 0) && (j3 == 0) && (s_fifo_n == 9) &&
                       (p[0] == 'A') && (p[1] == 'B') && (p[2] == 'C') &&
                       (p[3] == 'A') && (p[4] == 'B') && (p[5] == 'C') &&
                       (p[6] == 'A') && (p[7] == 'B') && (p[8] == 'C');

    host_check(ok, "HOST-SCHED-FIFO", "coop 就绪队列 FIFO: 三线程严格轮转 ABCABCABC");
}

/* ---- ② block/wake 状态机(直接打 br_sched_block_current / br_sched_wake) ---- */

static volatile int s_bw_rc = -12345;

static void host_waiter_inf(void *arg)
{
    (void)arg;
    br_thread_t *me = br_task_self();

    me->wait_status = BR_ERR(BR_ETIMEDOUT);   /* ICB 握手: 开窗前置"未唤醒" */
    s_bw_rc = br_sched_block_current(BR_TIMEOUT_INF);
}

static void host_case_blockwake(void)
{
    br_task_attr_t a = host_attr(3u, "hwait");
    br_thread_t *t = BR_NULL;
    br_bool blocked, ready, zombie;

    s_bw_rc = -12345;
    const int r = br_task_create(&t, &a, host_waiter_inf, BR_NULL);

    br_task_yield();                     /* 让 waiter 跑起来并阻塞 */
    blocked = (t != BR_NULL) && (br_task_state(t) == BR_TASK_BLOCKED);

    br_sched_wake(t);                    /* BLOCKED → READY(只改状态 + 挂链, 不动栈) */
    ready = (t != BR_NULL) && (br_task_state(t) == BR_TASK_READY);

    br_task_yield();                     /* 让它恢复: block_current 返回 wait_status(0) */
    zombie = (t != BR_NULL) && (br_task_state(t) == BR_TASK_ZOMBIE);

    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(BR_EINVAL);

    host_check((r == 0) && blocked && ready && zombie && (j == 0) && (s_bw_rc == 0),
               "HOST-SCHED-BLOCKWAKE",
               "block_current/wake: RUNNING→BLOCKED→(wake)READY→RUNNING→ZOMBIE");
}

/* ---- ③ 超时表到期唤醒(有限绝对期限) ---- */

static volatile int s_to_rc = -12345;

static void host_waiter_to(void *arg)
{
    (void)arg;
    br_thread_t *me = br_task_self();

    me->wait_status = BR_ERR(BR_ETIMEDOUT);
    s_to_rc = br_sched_block_current(host_sched_now() + 5000u);
}

static void host_case_timeout(void)
{
    br_task_attr_t a = host_attr(4u, "hto");
    br_thread_t *t = BR_NULL;

    s_to_rc = -12345;
    const int r = br_task_create(&t, &a, host_waiter_to, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(BR_EINVAL);

    host_check((r == 0) && (j == 0) && (s_to_rc == BR_ERR(BR_ETIMEDOUT)),
               "HOST-SCHED-TIMEOUT",
               "有限期限到期 ⇒ -ETIMEDOUT(wake_at 表在 tick 扫描里唤醒)");
}

/* ---- ④ sleep_until 登记 ---- */

static volatile br_time_t s_su_delta;
static volatile int      s_su_rc = -12345;

static void host_waiter_su(void *arg)
{
    (void)arg;
    const br_time_t t0 = br_clock_now();

    s_su_rc = br_task_sleep_until(t0 + 3000u);
    s_su_delta = br_clock_now() - t0;
}

static void host_case_sleep_until(void)
{
    br_task_attr_t a = host_attr(5u, "hsu");
    br_thread_t *t = BR_NULL;

    s_su_rc = -12345;
    s_su_delta = 0u;
    const int r = br_task_create(&t, &a, host_waiter_su, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(BR_EINVAL);

    host_check((r == 0) && (j == 0) && (s_su_rc == 0) && (s_su_delta >= 3000u),
               "HOST-SCHED-SLEEPUNTIL",
               "br_task_sleep_until 登记绝对期限, 不早醒");
}

/* =====================================================================
 * 监督线程: 跑完全部用例后按失败数退出(调度循环本身不返回)
 * ===================================================================== */

static void host_supervisor(void *arg)
{
    (void)arg;

    br_log_info("[HOSTTEST] supervisor 线程开始: self=%s state=%u",
                br_task_name(br_task_self()), (unsigned)br_task_state(br_task_self()));

    host_case_fifo();
    host_case_blockwake();
    host_case_timeout();
    host_case_sleep_until();

    /* core 的自检套件(`core/selftest/sched_selftest.c`): 它自身跑在真实线程里, 真的
     * create/join/yield/sleep。返回值 = 失败项数; 日志红绿由上面的 br_log 钩子汇总。 */
    br_sched_selftest();

    host_check(host_sched_idle_calls() > 0u, "HOST-SCHED-IDLE",
               "pick_next 返回 NULL ⇒ core 进 idle(宿主由钩子代 WFI)并被 tick 唤醒");
    host_check(br_sched_task_count() == 2u, "HOST-SCHED-POOL",
               "测试线程全部被 join 回收(只剩 main + supervisor)");
    host_check(host_sched_watch_hits() == 0u, "HOST-SCHED-CONF",
               "br_sched_selftest 无 [TASKCONF] FAIL");
    host_check((br_task_name(br_task_self()) != NULL) &&
               (strcmp(br_task_name(br_task_self()), "supervisor") == 0),
               "HOST-SCHED-NAME", "br_task_name/br_task_self 在真实线程里正确");

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d (idle_calls=%u)\n",
           s_hpass, s_hfail, s_hpass + s_hfail, (unsigned)host_sched_idle_calls());

    exit((s_hfail == 0) ? 0 : 1);
}

int main(void)
{
    printf("[HOSTTEST] ==== coop: 协作式调度器(无抢占) ====\n");

    /* 套件里的 [TASKCONF] FAIL 也要进退出码(宿主是 sched/sync 的主判据)。 */
    host_sched_watch("[TASKCONF] FAIL");

    const int r = coop_early_init();
    if (r != 0) {
        printf("[HOSTTEST] FAIL coop_early_init rc=%d\n", r);
        return 1;
    }
    host_sched_main(host_supervisor);
}
