/*
 * brickOS prototype v0.2.0 — 延迟工作(bottom half / workqueue, core)实现
 *
 * 设计与裁定: `core/include/br/core/br_work.h` 的文件头 + `docs/decisions/0011-...md`。
 * 设计依据: `3-01` §5(延迟工作 = bottom half; 队列深度静态、满 ⇒ -EAGAIN)、
 *           `3-02` §11.1(ISR → 延迟工作是四条交互路径的第一条)、§11.4.1(BH 形态下
 *           `br_irq_disable` 的语义: 抑制不取消/不重放 —— 抑制判定在 irq_core.c, 本文件
 *           只提供"工作项在 bh 里被执行"这个事实 + `br_work_in_bh()` 的判据)、
 *           §9.4(SLOW 域 demux 是它的第一个真实消费者)、IR-10(提交失败必须回滚)。
 *
 * 三个不可动摇的性质(实现与用例都建在它们上):
 *   W1 **有界**: 队列是编译期静态环(`BR_WORKQ_DEPTH`), 不分配、不增长; 满 ⇒ -EAGAIN。
 *   W2 **ISR-safe**: 唯一的共享可变状态(环 + 计数)只在 `br_irq_lock()` 的 L2 临界区里改;
 *      提交路径不取锁、不读时钟、不打日志、不回调(回调一律在 drain 里做)。
 *   W3 **非重入**: `br_work_drain` 在执行工作项期间把"在 bh"标志立起来; 工作项再调
 *      `br_work_drain`(或再提交后由外层循环继续)都安全 —— 栈深度不随队列长度增长。
 *
 * 为什么 drain 在**关中断**下执行: 它发生在 IRQ 出口(eoi 之后、ERET 之前), 此刻
 * `PSTATE.I == 1` 是硬件进入异常时就定下的。我们**刻意不去打开**它(打开就要考虑
 * 异常嵌套 IR-6 与 ISR 栈帧归属), 于是 bh 的契约是"短 + 不可阻塞"(见头注释的推论)。
 * 这也是 §11.4.1 那句"抑制判定必须原子"免费成立的原因: bh 与 ISR 天然互斥。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>

/* ------------------------------------------------------------------ 静态环 */

typedef struct br_work_item {
    br_work_fn_t fn;
    void        *arg;
} br_work_item_t;

static br_work_item_t s_q[BR_WORKQ_DEPTH];
static br_u32         s_head;    /* 出队游标(消费者) */
static br_u32         s_tail;    /* 入队游标(生产者) */
static br_u32         s_count;   /* 当前待执行条数 */

static br_bool        s_in_bh;   /* W3: drain 执行工作项期间为真 */

/* 诊断计数(不参与判定; 由 br_work_stats_get 暴露) */
static br_u32 s_submitted;
static br_u32 s_ran;
static br_u32 s_dropped_full;
static br_u32 s_dropped_invalid;
static br_u32 s_max_pending;
static br_u32 s_drain_passes;

/* ------------------------------------------------------------------ 提交 */

int br_work_submit(br_work_fn_t fn, void *arg)
{
    /* 参数错与队列满是两种不同的失败: 前者是**调用点的 bug**, 后者是**容量预算**
     * (IR-10 要求调用方对后者做回滚)。分开计数, 免得把 bug 读成"预算不够"。 */
    if (fn == BR_NULL) {
        s_dropped_invalid++;
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();   /* W2: 与 drain 的取件互斥 */

    if (s_count >= (br_u32)BR_WORKQ_DEPTH) {
        br_irq_unlock(st);
        s_dropped_full++;
        return BR_ERR(BR_EAGAIN);              /* W1: 绝不静默丢弃 */
    }

    s_q[s_tail].fn  = fn;
    s_q[s_tail].arg = arg;
    s_tail = (s_tail + 1u) % (br_u32)BR_WORKQ_DEPTH;
    s_count++;
    s_submitted++;
    if (s_count > s_max_pending) {
        s_max_pending = s_count;               /* "队列深度够不够"的证据 */
    }

    br_irq_unlock(st);
    return 0;
}

/* ------------------------------------------------------------------ 执行 */

br_u32 br_work_drain(br_u32 max_items)
{
    /* W3: 非重入 —— 已经在 bh 里说明是"工作项内部"调的, 交给外层循环继续。 */
    if (s_in_bh != BR_FALSE) {
        return 0u;
    }
    if (max_items == 0u) {
        return 0u;
    }

    s_in_bh = BR_TRUE;
    s_drain_passes++;

    br_u32 ran = 0u;

    while (ran < max_items) {
        br_work_fn_t fn;
        void        *arg;

        /* 取件在 L2 临界区里(L2 的语义就是"这一小段不与中断竞争")。 */
        const br_irq_state_t st = br_irq_lock();

        if (s_count == 0u) {
            br_irq_unlock(st);
            break;
        }

        fn  = s_q[s_head].fn;
        arg = s_q[s_head].arg;
        s_q[s_head].fn  = BR_NULL;             /* 槽位清空: 复用时不残留上一任的指针 */
        s_q[s_head].arg = BR_NULL;
        s_head = (s_head + 1u) % (br_u32)BR_WORKQ_DEPTH;
        s_count--;

        br_irq_unlock(st);

        /* ★ 出临界区之后再执行: 工作项自己要能取 L2(否则自锁), 也要能被观测
         *   (计数器、trace)。执行期间 `s_in_bh` 为真 ⇒ 阻塞/让出类 API 会拒绝。 */
        if (fn != BR_NULL) {
            fn(arg);
            s_ran++;
        }
        ran++;
    }

    s_in_bh = BR_FALSE;
    return ran;
}

br_bool br_work_in_bh(void)
{
    return s_in_bh;
}

void br_work_stats_get(br_work_stat_t *out)
{
    if (out == BR_NULL) {
        return;
    }

    out->submitted       = s_submitted;
    out->ran             = s_ran;
    out->dropped_full    = s_dropped_full;
    out->dropped_invalid = s_dropped_invalid;
    out->pending         = s_count;
    out->max_pending     = s_max_pending;
    out->drain_passes    = s_drain_passes;
}
