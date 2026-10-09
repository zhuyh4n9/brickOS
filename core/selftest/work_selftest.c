/*
 * brickOS prototype v0.2.0 — 延迟工作(下半部 / workqueue)的**自检套件**(core/selftest/)
 *
 * 设计依据与逐条裁定: `docs/decisions/0011-round-robin-bh-workqueue.md`;
 * 设计出处: `3-01-core-api-list.md` §5(延迟工作 = bottom half)、
 *           `3-02-int.md` §11.1(路径一: ISR → 延迟工作)/§11.4.1(BH 形态下 disable 的语义)/
 *           §12.4(执行时机与优先级)/IR-10(提交失败必须回滚)。
 *
 * ## 为什么这些用例在 core 的自检目录里
 *   与 `sched_selftest.c` 同源(ADR-0010): 用例不是插件的对外能力, 不该进 `br_work.h`
 *   的 golden 面; 而"关掉自检 ⇒ 整目录不参与编译"这条对 core 侧同样适用。
 *
 * ## 用例清单(与 `tests/host/work_test.c` 的宿主腿一一对应)
 *   TC-WQ-001  队列有界: 填满 DEPTH 条成功, 第 DEPTH+1 条 ⇒ -EAGAIN(不静默丢弃)
 *   TC-WQ-002  FIFO: drain 顺序 == 提交顺序; drain(max) 严格遵守预算
 *   TC-WQ-003  非重入: 工作项里再调 drain ⇒ 0(栈不随队列长度增长)
 *   TC-WQ-004  bh 上下文: 体内 `br_work_in_bh()` 为真、`br_irq_in_atomic()` 为真,
 *              而 `br_irq_in_isr()` 为假(这就是"下半部"的定义)
 *   TC-WQ-005  bh 禁令: 阻塞 ⇒ -EPERM; 让出 ⇒ 被拒 + 留痕 BH_CTX_VIOLATION
 *   TC-WQ-006  统计面自洽(submitted/ran/pending/max_pending 与动作对得上)
 *   TC-WQ-007  在 **IRQ 出口**被真正消费: 目标上由 `br_irq_enter` 的出口 drain 驱动,
 *              这里用"提交 + 显式 drain"表达同一条链路的可判定部分
 *
 * ## 返回值
 *   `int` = **失败项数**(ADR-0010 §2.4)。任何失败都**不停机**。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>

/* TCB 公共头(就绪/阻塞/状态字段)与"注册表只读观测"是 core 私有面(与
 * sched_selftest.c 同源: 跨目录相对包含在本仓有先例)。 */
#include "../src/sched/sched_internal.h"

/* 入口的可见声明集中在 `core/selftest/core_selftest.c`(唯一聚合器); 这里再声明一次
 * 是为了满足"每个函数使用前都有原型"的纪律(-Wmissing-prototypes)。 */
int br_work_selftest(void);

static int s_wq_pass;
static int s_wq_fail;

static void wq_result(br_bool ok, const char *id, const char *what)
{
    if (ok != BR_FALSE) {
        s_wq_pass++;
        br_log_info("[WQCONF] PASS %s %s", id, what);
    } else {
        s_wq_fail++;
        br_log_info("[WQCONF] FAIL %s %s", id, what);
    }
}

/* ------------------------------------------------------------------ 工作项体 */

static volatile br_u32 s_wq_order[BR_WORKQ_DEPTH + 4u];
static volatile br_u32 s_wq_order_n;

static void wq_item_note(void *arg)
{
    const br_u32 v = (br_u32)(br_uintptr_t)arg;

    if (s_wq_order_n < (br_u32)(sizeof(s_wq_order) / sizeof(s_wq_order[0]))) {
        s_wq_order[s_wq_order_n] = v;
        s_wq_order_n++;
    }
}

static volatile int s_wq_nested = -1;
static volatile int s_wq_bh_flag = -1;
static volatile int s_wq_atomic_flag = -1;
static volatile int s_wq_isr_flag = -1;
static volatile int s_wq_block_rc = 999;
static volatile br_thread_t *s_wq_self_before;
static volatile br_thread_t *s_wq_self_after;
static volatile int s_wq_bh_after_yield = -1;
static volatile br_u32 s_wq_violation_before;
static volatile br_u32 s_wq_violation_after;

static br_u32 wq_trace_violations(void)
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

static void wq_item_context(void *arg)
{
    (void)arg;

    s_wq_bh_flag     = (br_work_in_bh() != BR_FALSE) ? 1 : 0;
    s_wq_atomic_flag = (br_irq_in_atomic() != BR_FALSE) ? 1 : 0;
    s_wq_isr_flag    = (br_irq_in_isr() != BR_FALSE) ? 1 : 0;

    s_wq_nested = (int)br_work_drain(4u);          /* TC-WQ-003 */

    br_thread_t *me = br_task_self();
    if (me != BR_NULL) {
        me->wait_status = BR_ERR(BR_ETIMEDOUT);    /* ICB 契约: 开窗前先置"未唤醒" */
    }
    s_wq_block_rc = br_sched_block_current(BR_TIMEOUT_INF);   /* TC-WQ-005 */

    s_wq_self_before = br_task_self();
    br_task_yield();                               /* TC-WQ-005: 应被拒 */
    s_wq_self_after = br_task_self();
    s_wq_bh_after_yield = (br_work_in_bh() != BR_FALSE) ? 1 : 0;

    wq_item_note((void *)(br_uintptr_t)0xE0u);
}

/* ------------------------------------------------------------------ 用例 */

static void case_wq_001_bound(void)
{
    br_work_stat_t st;
    int first_rc = 0;
    int full_rc;

    br_work_stats_get(&st);
    const br_u32 base_drop = st.dropped_full;

    /* 队列可能不是空的(前面用例的余项) ⇒ 先算清剩多少, 再把它填满。 */
    while (br_work_submit(wq_item_note, (void *)(br_uintptr_t)1u) == 0) {
        /* 填到满 */
    }
    full_rc = br_work_submit(wq_item_note, (void *)(br_uintptr_t)2u);
    br_work_stats_get(&st);

    const br_bool ok = (full_rc == BR_ERR(BR_EAGAIN)) &&
                       (st.pending == (br_u32)BR_WORKQ_DEPTH) &&
                       (st.max_pending == (br_u32)BR_WORKQ_DEPTH) &&
                       (st.dropped_full > base_drop);
    (void)first_rc;

    wq_result(ok, "TC-WQ-001",
              "队列有界: 填满 DEPTH 后 -EAGAIN, 且 dropped_full/max_pending 如实增长");

    /* 清空(收尾: 后面的用例需要干净队列) */
    (void)br_work_drain((br_u32)BR_WORKQ_DEPTH);
}

static void case_wq_002_fifo_budget(void)
{
    br_work_stat_t st;

    s_wq_order_n = 0u;

    for (br_u32 i = 0u; i < 6u; i++) {
        const int r = br_work_submit(wq_item_note, (void *)(br_uintptr_t)(100u + i));
        if (r != 0) {
            wq_result(BR_FALSE, "TC-WQ-002", "提交失败(队列被前面的用例占着?)");
            return;
        }
    }

    const br_u32 a = br_work_drain(2u);          /* 预算 2 */
    br_work_stats_get(&st);
    const br_u32 pending_mid = st.pending;
    const br_u32 b = br_work_drain(8u);
    br_work_stats_get(&st);

    br_bool fifo = (s_wq_order_n == 6u);
    for (br_u32 i = 0u; fifo && i < 6u; i++) {
        if (s_wq_order[i] != (100u + i)) {
            fifo = BR_FALSE;
        }
    }

    wq_result(fifo && (a == 2u) && (pending_mid == 4u) && (b == 4u) && (st.pending == 0u),
              "TC-WQ-002",
              "FIFO 顺序 + drain(max) 严格遵守预算(2 条走后剩 4 条, 再由下一轮取完)");
}

static void case_wq_003_006_context(void)
{
    br_work_stat_t st0, st1;

    s_wq_nested = -1;
    s_wq_bh_flag = -1;
    s_wq_atomic_flag = -1;
    s_wq_isr_flag = -1;
    s_wq_block_rc = 999;
    s_wq_self_before = BR_NULL;
    s_wq_self_after = BR_NULL;
    s_wq_bh_after_yield = -1;

    br_work_stats_get(&st0);
    s_wq_violation_before = wq_trace_violations();

    const int r = br_work_submit(wq_item_context, BR_NULL);
    const br_u32 ran = br_work_drain(4u);

    s_wq_violation_after = wq_trace_violations();
    br_work_stats_get(&st1);

    wq_result((r == 0) && (ran == 1u) && (s_wq_bh_flag == 1) && (s_wq_atomic_flag == 1) &&
              (s_wq_isr_flag == 0),
              "TC-WQ-004",
              "bh 上下文: in_bh=真 / in_atomic=真 / in_isr=假(这就是下半部的定义)");

    wq_result(s_wq_nested == 0, "TC-WQ-003",
              "非重入: 工作项里再调 drain ⇒ 0(栈不随队列长度增长)");

    wq_result(s_wq_block_rc == BR_ERR(BR_EPERM), "TC-WQ-005",
              "bh 内 br_sched_block_current ⇒ -EPERM(没有可回退的调度上下文)");

    wq_result((s_wq_self_before == s_wq_self_after) && (s_wq_bh_after_yield == 1) &&
              (s_wq_violation_after > s_wq_violation_before),
              "TC-WQ-005",
              "bh 内 br_task_yield 被拒(线程未变、仍在 bh)+ 留痕 BH_CTX_VIOLATION");

    wq_result((st1.submitted == st0.submitted + 1u) && (st1.ran == st0.ran + 1u) &&
              (st1.pending == 0u) && (st1.max_pending >= st0.max_pending),
              "TC-WQ-006",
              "统计面自洽: submitted/ran 各 +1, pending 归零, max_pending 单调不减");
}

static void case_wq_007_irq_exit(void)
{
    /*
     * IRQ 出口的消费链在目标上由 `br_irq_enter` 的出口调 `br_work_drain(BR_WORK_BH_BUDGET)`
     * 完成(那条路径由 QEMU 的 TC-IRQ-015/017 覆盖)。这里验证它在 core 侧的**可判定前提**:
     * 预算参数本身是正的、且 drain 一次最多吃 BUDGET 条 —— 否则"IRQ 出口有上界"这句话
     * 就是空的。
     */
    s_wq_order_n = 0u;

    for (br_u32 i = 0u; i < (br_u32)BR_WORK_BH_BUDGET + 2u; i++) {
        (void)br_work_submit(wq_item_note, (void *)(br_uintptr_t)(200u + i));
    }
    const br_u32 ran = br_work_drain((br_u32)BR_WORK_BH_BUDGET);
    br_work_stat_t st;
    br_work_stats_get(&st);

    const br_bool ok = (BR_WORK_BH_BUDGET > 0u) && (ran == (br_u32)BR_WORK_BH_BUDGET) &&
                       (st.pending == 2u);

    /* 收尾 */
    (void)br_work_drain(8u);

    wq_result(ok, "TC-WQ-007",
              "IRQ 出口的单次执行有上界(BR_WORK_BH_BUDGET); 余项留给下一次中断出口");
}

int br_work_selftest(void)
{
    s_wq_pass = 0;
    s_wq_fail = 0;

    br_log_info("[WQCONF] ==== bottom half / workqueue conformance (3-01 §5 / 3-02 §11.1/§11.4.1)");
    br_log_info("[WQCONF] ---- queue_depth=%u bh_budget=%u",
                (unsigned)BR_WORKQ_DEPTH, (unsigned)BR_WORK_BH_BUDGET);

    case_wq_001_bound();
    case_wq_002_fifo_budget();
    case_wq_003_006_context();
    case_wq_007_irq_exit();

    br_log_info("[WQCONF] SUMMARY pass=%d fail=%d total=%d",
                s_wq_pass, s_wq_fail, s_wq_pass + s_wq_fail);

    if (s_wq_fail != 0) {
        br_log_error("[WQCONF] 有失败项 —— 下半部/工作队列未达 conformance");
    }

    return s_wq_fail;
}
