/*
 * sched/rr — 时间片轮转(round-robin)**抢占**调度器实现
 *
 * 设计依据: `3-03-sched.md` §1("语义在 core, 策略在插件")/§2;
 *           `3-01-core-api-list.md` §5.1(br_sched_ops 注册点)/§2.1(coop 与 preempt 的分界);
 *           `3-02-int.md` §11.2(IRQ 出口的抢占接缝)/§11.3(v1 coop 不抢先切换 ⇒ 本件正是
 *           "下一次中断出口即可能切换"的形态)/§12.4(bh 与调度的先后关系);
 *           逐条裁定与证据: `docs/decisions/0011-round-robin-bh-workqueue.md`。
 *
 * ## 本插件负责的全部策略
 *   - **就绪队列 = FIFO**(单链表, 节点在插件私有尾里 —— 零分配, 与 coop 同型);
 *   - `pick_next` 出队**并重新装载该线程的时间片**; 队空 ⇒ BR_NULL ⇒ core 走 idle/WFI
 *     (3-02 §11.1 的 L2 临界区, 在 core);
 *   - `on_tick`: 递减当前线程的时间片, **用尽即 `br_sched_request_resched()`** ——
 *     这是本插件与 coop 的**唯一**差别(换栈本身仍由 core 在 IRQ 出口做, 见下);
 *   - `sleep_until` 钩子返回 0: 超时唤醒由 core 的 `wake_at` 表统一负责;
 *   - `idle = BR_NULL`: 缺省走 core 的 WFI 临界区。
 *
 * ## 与 core 的分工(ADR-0006 §1 的 I2 + ADR-0011)
 *   插件**只动队列成员关系 + 自己的私有尾**; `state` 一律由 core 改(I2)。
 *   "什么时候该换"归插件(时间片), "怎么换"归 core:
 *     `br_sched_request_resched()` 只置一个位; core 在 **IRQ 出口(eoi 之后、ERET 之前)**
 *     把当前线程放回队尾、`pick_next`、需要时 `br_sched_switch_to`。
 *   ⇒ 本插件里**没有一行切换代码, 也没有一行改 state 的代码** —— 这正是"机制/策略"
 *     切开的样子: 换 `sched/coop` 只需重新选一个插件, core 一行不改。
 *
 * ## ISR 安全
 *   就绪队列的每次改动都在 `br_irq_lock()/br_irq_unlock()` 的 L2 临界区里:
 *   定时器 ISR 会在 `br_sched_on_tick()` 里唤醒线程(⇒ 入队), 而线程侧可能正在
 *   `pick_next` 的中途 —— 没有这个临界区, 队列会在"tail 更新到一半"时被 ISR 撕裂。
 *   `on_tick` 本身在 ISR 里跑, 它只读 `br_task_self()`/`br_task_state()`(core 只读面)
 *   与自己的私有尾。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_types.h>

#include <br/sched/rr.h>

/* 插件私有尾(挂在 TCB 公共头之后; 大小报给 core 的 `tcb_size`)。 */
struct rr_priv {
    struct br_thread *ready_next;  /* 就绪队列节点(零分配) */
    br_u32            slice_left;  /* 剩余时间片(tick 数; 0 = 待轮换) */
    br_u32            turns;       /* 观测: 被调度上来的次数(诊断/自检用) */
};

static struct br_thread *s_head;   /* 队头(下一个该跑的人) */
static struct br_thread *s_tail;   /* 队尾(被抢占/让出者回队的位置) */
static br_u32            s_len;

static br_sched_ops_t    s_ops;    /* 非常量: tcb_size 要运行期算(br_sched_tcb_size()) */

static struct rr_priv *priv_of(br_thread_t *t)
{
    return (struct rr_priv *)br_thread_priv(t);
}

/* ------------------------------------------------------------------ 就绪队列 */

static int rr_thread_ready(br_thread_t *t)
{
    if (t == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    struct rr_priv *p = priv_of(t);
    p->ready_next = BR_NULL;

    if (s_tail == BR_NULL) {
        s_head = t;
    } else {
        priv_of(s_tail)->ready_next = t;
    }
    s_tail = t;
    s_len++;

    br_irq_unlock(st);
    return 0;
}

/*
 * RUNNING → BLOCKED 的钩子。RR 下 RUNNING 者**本就不在队列里**, 所以这通常是空操作;
 * 仍做防御性线性摘除: 若插件被误用(重复 ready), 也不至于留下一个"不可运行却在队列里"
 * 的幽灵节点。
 */
static void rr_thread_block(br_thread_t *t)
{
    if (t == BR_NULL) {
        return;
    }

    const br_irq_state_t st = br_irq_lock();

    struct br_thread **pp = &s_head;
    struct br_thread  *prev = BR_NULL;
    while (*pp != BR_NULL) {
        if (*pp == t) {
            *pp = priv_of(t)->ready_next;
            if (s_tail == t) {
                s_tail = prev;
            }
            if (s_head == BR_NULL) {
                s_tail = BR_NULL;
            }
            s_len--;
            break;
        }
        prev = *pp;
        pp = &priv_of(*pp)->ready_next;
    }
    priv_of(t)->ready_next = BR_NULL;

    br_irq_unlock(st);
}

/*
 * 出队 **+ 装载时间片**。为什么在这里装载(而不是在 `thread_ready` 里):
 *   `thread_ready` 是"进入就绪"的公共入口(create / wake / yield / 被抢占回队),
 *   在那里重置时间片等于"每让出一次就白送一个整片"; 装载点放在**被选中**这一刻,
 *   "一个线程一轮拿一个完整时间片"这条性质就只依赖一处。
 * 不改 `state`(core 负责置 RUNNING); 队空 ⇒ BR_NULL ⇒ core idle。
 */
static br_thread_t *rr_pick_next(void)
{
    br_thread_t *t;

    const br_irq_state_t st = br_irq_lock();

    t = s_head;
    if (t != BR_NULL) {
        s_head = priv_of(t)->ready_next;
        if (s_head == BR_NULL) {
            s_tail = BR_NULL;
        }
        priv_of(t)->ready_next = BR_NULL;
        s_len--;

        struct rr_priv *p = priv_of(t);
        p->slice_left = RR_SLICE_TICKS;   /* ★ 轮到你了: 给一个完整时间片 */
        p->turns++;
    }

    br_irq_unlock(st);
    return t;
}

/*
 * tick 策略 = 本插件的全部"抢占策略"。
 *
 * 由 core 的 `br_sched_on_tick()`(= 平台 timer ISR)调用, 因此**必须是 ISR 安全的**:
 * 只写自己的私有尾与 core 的 need_resched 位, 不取锁、不切栈、不回调。
 *
 * 语义: 每 tick 递减; 归零 ⇒ 请求一次抢占(core 在本次 IRQ 出口消费)。
 * 若此刻没有别人可跑, core 的 `pick_next` 会把同一个线程再选出来并重新装载时间片 ——
 * 于是"没人竞争时不做无谓的换栈"这条也由 core 免费提供。
 */
static void rr_on_tick(void)
{
    br_thread_t *cur = br_task_self();

    if (cur == BR_NULL) {
        return;
    }
    /* 只给"真的在跑"的线程计时(RUNNING 之外没有时间片可言)。 */
    if (br_task_state(cur) != BR_TASK_RUNNING) {
        return;
    }

    struct rr_priv *p = priv_of(cur);

    if (p->slice_left == 0u) {
        return;                            /* 已经请求过了, 等 IRQ 出口消费 */
    }

    p->slice_left--;
    if (p->slice_left == 0u) {
        br_sched_request_resched();         /* ← 唯一的策略出口 */
    }
}

/*
 * 超时登记的钩子。core 的 `wake_at` 表是唯一真值(周期 tick 上扫到期);
 * 这里返回 0 表示"接受登记", 槽位留给将来的时间轮实现挂自己的结构。
 */
static int rr_sleep_until(br_thread_t *t, br_time_t abs_us)
{
    (void)t;
    (void)abs_us;
    return 0;
}

/* ------------------------------------------------------------------ 生命周期 */

int rr_early_init(void)
{
    s_head     = BR_NULL;
    s_tail     = BR_NULL;
    s_len      = 0u;

    s_ops.name         = "sched/rr";
    s_ops.kind         = BR_SCHED_KIND_PREEMPT;
    s_ops.tcb_size     = (br_u32)(br_sched_tcb_size() + sizeof(struct rr_priv));
    s_ops.thread_ready = rr_thread_ready;
    s_ops.thread_block = rr_thread_block;
    s_ops.pick_next    = rr_pick_next;
    s_ops.on_tick      = rr_on_tick;       /* ★ 与 coop 的唯一差别 */
    s_ops.sleep_until  = rr_sleep_until;
    s_ops.idle         = BR_NULL;          /* 缺省 = core 的 L2 临界区 + WFI(3-02 §11.1) */

    br_sched_register(&s_ops);             /* 恰一次; 二次注册 = core panic */
    return 0;
}

int rr_init(void)
{
    return 0;
}

int rr_start(void)
{
    return 0;
}
