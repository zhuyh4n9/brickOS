/*
 * tests/host/work_test.c — 宿主侧 **延迟工作(下半部 / workqueue)** 语义门禁
 *
 * 为什么在宿主跑: 队列的有界性 / FIFO / 非重入 / 上下文标志 / 统计这些是**纯语义**,
 * 宿主上毫秒级就能把边界撞满(目标上要构造一次"队列满"得靠中断风暴)。目标侧另有一条腿
 * (QEMU 的 `TC-IRQ-015/016/017`: 按线的 BH 分发、抑制、不重放), 两者合起来才完整。
 *
 * ★ 被验的三条不可动摇的性质(`br_work.h` 的 W1/W2/W3):
 *   W1 有界: 队列是编译期静态环; 满 ⇒ `-EAGAIN`(绝不静默丢弃 —— IR-10)
 *   W2 ISR-safe: 提交路径不回调、不取锁
 *   W3 非重入: 工作项里再调 `br_work_drain` 返回 0, 栈深度不随队列长度增长
 *
 * 输出协议: [HOSTTEST] PASS|FAIL … SUMMARY; 有失败 ⇒ 退出码非 0。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>

#include <br/sched/rr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_sched.h"

/* TCB 公共头: bh 内调 `br_sched_block_current` 时要按 ICB 契约先开窗(见 br_sched.h)。 */
#include "sched_internal.h"

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
static br_u8 s_stk[4][HOST_STACK_BYTES] BR_ALIGN(16);

/* =====================================================================
 * 工作项体
 * ===================================================================== */

static volatile br_u32 s_order[BR_WORKQ_DEPTH + 4u];
static volatile br_u32 s_order_n;
static volatile br_u32 s_ran;

static void note(br_u32 v)
{
    if (s_order_n < (br_u32)(sizeof(s_order) / sizeof(s_order[0]))) {
        s_order[s_order_n] = v;
        s_order_n++;
    }
    s_ran++;
}

static void item_note(void *arg)
{
    note((br_u32)(br_uintptr_t)arg);
}

/* ---- W3: 非重入 + bh 上下文标志 + 阻塞/让出禁令 ---- */

static volatile int s_nested_rc = -1;
static volatile int s_bh_flag_inside;
static volatile int s_bh_flag_after_yield;
static volatile int s_block_rc = 999;
static volatile br_thread_t *s_self_before;
static volatile br_thread_t *s_self_after;
static volatile br_u32 s_trace_violation;

static void item_context(void *arg)
{
    (void)arg;

    s_bh_flag_inside = (br_work_in_bh() != BR_FALSE) ? 1 : 0;
    s_nested_rc = (int)br_work_drain(4u);        /* W3: 非重入 ⇒ 0 */

    /* bh 里阻塞 ⇒ -EPERM(参数没错, 上下文错了)。 */
    br_thread_t *me = br_task_self();
    me->wait_status = BR_ERR(BR_ETIMEDOUT);      /* 按 ICB 契约先开窗 */
    s_block_rc = br_sched_block_current(BR_TIMEOUT_INF);

    /* bh 里让出 ⇒ 拒绝执行 + 留痕(不静默地当成"让出成功")。 */
    s_self_before = br_task_self();
    br_task_yield();
    s_self_after = br_task_self();
    s_bh_flag_after_yield = (br_work_in_bh() != BR_FALSE) ? 1 : 0;

    note(0xE0u);
}

/* 取 trace 环里有没有 BH_CTX_VIOLATION(bh 禁令的运行期留痕)。 */
static br_u32 drain_violation_count(void)
{
    br_trace_evt_t ev[32];
    br_u32 n;
    br_u32 hits = 0u;

    while ((n = br_trace_drain(ev, 32u)) > 0u) {
        for (br_u32 i = 0u; i < n; i++) {
            if (ev[i].id == BR_TRACE_BH_CTX_VIOLATION) {
                hits++;
            }
        }
    }
    return hits;
}

/* ---- 案例 ---- */

static void case_bound_and_order(void)
{
    br_work_stat_t st;
    br_u32 ran;
    int rc;
    br_u32 first_full = 0u;

    br_work_stats_get(&st);
    const br_u32 base_drop = st.dropped_full;
    const br_u32 base_ran  = st.ran;

    s_order_n = 0u;
    s_ran = 0u;

    /* W1: 填满队列 —— 第 DEPTH+1 次必须 -EAGAIN */
    rc = 0;
    for (br_u32 i = 0u; i < (br_u32)BR_WORKQ_DEPTH; i++) {
        const int r = br_work_submit(item_note, (void *)(br_uintptr_t)(100u + i));
        if (r != 0) {
            rc = r;
            break;
        }
    }
    first_full = (br_u32)br_work_submit(item_note, (void *)(br_uintptr_t)999u);
    br_work_stats_get(&st);

    host_check((rc == 0) && (first_full == (br_u32)BR_ERR(BR_EAGAIN)) &&
               (st.pending == (br_u32)BR_WORKQ_DEPTH) &&
               (st.max_pending == (br_u32)BR_WORKQ_DEPTH),
               "HOST-WQ-BOUND",
               "队列有界: 填满 DEPTH 条成功, 第 DEPTH+1 条 ⇒ -EAGAIN(不静默丢弃)");

    /* FIFO: drain 顺序必须与提交顺序逐条一致(每次只给 1 的预算 ⇒ 也顺带验预算被尊重) */
    ran = 0u;
    for (br_u32 i = 0u; i < (br_u32)BR_WORKQ_DEPTH; i++) {
        const br_u32 k = br_work_drain(1u);
        if (k == 0u) {
            break;
        }
        ran += k;
    }
    br_bool fifo = (ran == (br_u32)BR_WORKQ_DEPTH) &&
                   (s_order_n == (br_u32)BR_WORKQ_DEPTH);
    for (br_u32 i = 0u; fifo && i < (br_u32)BR_WORKQ_DEPTH; i++) {
        if (s_order[i] != (100u + i)) {
            fifo = BR_FALSE;
        }
    }

    br_work_stats_get(&st);
    host_check(fifo && (st.pending == 0u) && (st.dropped_full > base_drop) &&
               (st.ran == base_ran + (br_u32)BR_WORKQ_DEPTH),
               "HOST-WQ-FIFO",
               "drain 按 FIFO 逐条执行(预算=1 时每轮只出一条), 队空后 pending==0");

    /* 参数错与容量满是两种失败(分开计数, 免得把 bug 读成"预算不够") */
    const int bad = br_work_submit(BR_NULL, BR_NULL);
    br_work_stats_get(&st);
    host_check((bad == BR_ERR(BR_EINVAL)) && (st.dropped_invalid == 1u),
               "HOST-WQ-EINVAL", "fn == NULL ⇒ -EINVAL(单独计数, 不与队满混淆)");
}

static void case_bh_context(void)
{
    br_work_stat_t st0, st1;

    s_nested_rc = -1;
    s_bh_flag_inside = -1;
    s_block_rc = 999;
    s_self_before = BR_NULL;
    s_self_after = BR_NULL;
    s_bh_flag_after_yield = -1;
    s_order_n = 0u;

    br_work_stats_get(&st0);
    const br_u32 tr0 = drain_violation_count();

    const int r = br_work_submit(item_context, BR_NULL);
    const br_u32 ran = br_work_drain(4u);

    br_work_stats_get(&st1);
    const br_u32 tr1 = drain_violation_count();
    s_trace_violation = tr1 - tr0;

    host_check((r == 0) && (ran == 1u) && (s_bh_flag_inside == 1),
               "HOST-WQ-BHCTX", "工作项体内 br_work_in_bh() 为真(bh 上下文标志)");
    host_check(s_nested_rc == 0, "HOST-WQ-NORECURSE",
               "工作项里再调 br_work_drain ⇒ 直接返回 0(非重入, 栈不随队列长度增长)");
    host_check(s_block_rc == BR_ERR(BR_EPERM), "HOST-WQ-NOBLOCK",
               "bh 内 br_sched_block_current ⇒ -EPERM(没有可回退的调度上下文)");
    host_check((s_self_before == s_self_after) && (s_bh_flag_after_yield == 1) &&
               (s_trace_violation >= 1u),
               "HOST-WQ-NOYIELD",
               "bh 内 br_task_yield 被拒(线程不变、仍在 bh)+ 留痕 BH_CTX_VIOLATION");
    host_check(st1.drain_passes > st0.drain_passes, "HOST-WQ-STATS",
               "drain_passes 计数递增(统计面可用作诊断)");
}

static void case_budget(void)
{
    s_order_n = 0u;

    for (br_u32 i = 0u; i < 6u; i++) {
        (void)br_work_submit(item_note, (void *)(br_uintptr_t)(700u + i));
    }

    const br_u32 a = br_work_drain(2u);
    br_work_stat_t st;
    br_work_stats_get(&st);
    const br_u32 pending_after = st.pending;

    const br_u32 b = br_work_drain(8u);
    br_work_stats_get(&st);

    host_check((a == 2u) && (pending_after == 4u) && (b == 4u) && (st.pending == 0u),
               "HOST-WQ-BUDGET",
               "drain(max) 严格遵守预算: 2+4 条跑完, 余项留在队列里等下一轮(IRQ 出口的处方)");
}

/* =====================================================================
 * 监督线程
 * ===================================================================== */

static volatile int s_other_hits;

static void other_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) {
        s_other_hits++;
        br_task_yield();
    }
}

static void host_supervisor(void *arg)
{
    (void)arg;

    br_log_info("[HOSTTEST] supervisor 开始: self=%s", br_task_name(br_task_self()));

    case_bound_and_order();
    case_bh_context();
    case_budget();

    /* 让出一次: 若"bh 里的 yield"真的切走了, 对手会在这里跑起来 —— 与上面
     * HOST-WQ-NOYIELD 的判据互补(那条只看线程身份, 这条看"别人有没有真的跑")。 */
    s_other_hits = 0;
    br_task_attr_t a = { .name = "other",
                         .stack = (void *)(s_stk[0] + HOST_STACK_BYTES),
                         .stack_size = HOST_STACK_BYTES,
                         .prio = 0u, .flags = 0u };
    br_thread_t *t = BR_NULL;
    const int r = br_task_create(&t, &a, other_thread, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(BR_EINVAL);

    host_check((r == 0) && (j == 0) && (s_other_hits > 0),
               "HOST-WQ-SCHED",
               "工作队列与调度器共存: 线程照常让出/被换入(bh 不破坏调度)");

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d\n",
           s_hpass, s_hfail, s_hpass + s_hfail);

    exit((s_hfail == 0) ? 0 : 1);
}

int main(void)
{
    printf("[HOSTTEST] ==== 下半部(workqueue / bh)语义 ====\n");

    host_sched_watch("[TASKCONF] FAIL");

    const int r = rr_early_init();
    if (r != 0) {
        printf("[HOSTTEST] FAIL rr_early_init rc=%d\n", r);
        return 1;
    }
    host_sched_main(host_supervisor);
}
