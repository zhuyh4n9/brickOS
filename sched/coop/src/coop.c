/*
 * sched/coop — 协作式调度器(coop)实现
 *
 * 设计依据: `3-03-sched.md` §1("语义在 core, 策略在插件")/§2;
 *           `3-01-core-api-list.md` §5.1(br_sched_ops 注册点)/§6(任务与线程);
 *           `3-02-int.md` §11.3(coop 的中断-调度语义: IRQ 返回必回被打断的线程);
 *           裁定与 ops 成文表: `docs/decisions/0006-scheduler-framework.md`。
 *
 * ## 本插件负责的全部策略
 *   - **就绪队列 = FIFO**(单链表, 节点在插件私有尾里 —— 零分配, CA-2 同型);
 *   - `pick_next` 出队; 队空 ⇒ BR_NULL ⇒ core 走 idle/WFI(3-02 §11.1 的 L2 临界区);
 *   - **无 tick 策略**(`on_tick = BR_NULL`): coop 不做时间片轮转 —— 抢占不存在,
 *     round-robin 语义只由 `br_task_yield()` 的自愿让出产生。这就是 coop 与 preempt 的分界。
 *   - `sleep_until` 钩子返回 0: 超时唤醒**由 core 的 `wake_at` 表统一负责**;
 *     这里保留槽位给 preempt/tt 挂自己的时间轮/调度表。
 *
 * ## 与 core 的状态机分工(ADR-0006 §1 的 I2)
 *   插件**只动队列成员关系**; `state` 一律由 core 改。`pick_next` 只出队,
 *   置 RUNNING 是 core 的事 —— 这样状态机只有一处真值。
 *
 * ## ISR 安全
 *   就绪队列的每次改动都在 `br_irq_lock()/br_irq_unlock()` 的 L2 临界区里:
 *   定时器 ISR 会在 `br_sched_on_tick()` 里唤醒线程(⇒ 入队), 而线程侧可能正在
 *   `pick_next` 的中途 —— 没有这个临界区, 队列会在"tail 更新到一半"时被 ISR 撕裂。
 */
#include <br/core/br_error.h>
#include <br/core/br_irq.h>
#include <br/core/br_sched.h>
#include <br/core/br_types.h>

#include <br/sched/coop.h>

/* 插件私有尾(挂在 TCB 公共头之后; 大小报给 core 的 `tcb_size`)。 */
struct coop_priv {
    struct br_thread *ready_next;
};

static struct br_thread *s_head;   /* 队头(下一个该跑的人) */
static struct br_thread *s_tail;   /* 队尾(yield 回队的位置) */
static br_u32            s_len;

static br_sched_ops_t    s_ops;    /* 非常量: tcb_size 要运行期算(br_sched_tcb_size()) */

static struct coop_priv *priv_of(br_thread_t *t)
{
    return (struct coop_priv *)br_thread_priv(t);
}

/* ------------------------------------------------------------------ 就绪队列 */

static int coop_thread_ready(br_thread_t *t)
{
    if (t == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    struct coop_priv *p = priv_of(t);
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
 * RUNNING → BLOCKED 的钩子。coop 下 RUNNING 者**本就不在队列里**, 所以这通常是空操作;
 * 仍做防御性线性摘除: 若插件被误用(重复 ready), 也不至于留下一个"不可运行却在队列里"
 * 的幽灵节点。
 */
static void coop_thread_block(br_thread_t *t)
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

/* 出队。**不改 state**(core 负责置 RUNNING); 队空 ⇒ BR_NULL ⇒ core idle。 */
static br_thread_t *coop_pick_next(void)
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
    }

    br_irq_unlock(st);
    return t;
}

/*
 * 超时登记的钩子。core 的 `wake_at` 表是唯一真值(周期 tick 上扫到期);
 * 这里返回 0 表示"接受登记", 槽位留给 preempt/tt 挂自己的时间结构。
 */
static int coop_sleep_until(br_thread_t *t, br_time_t abs_us)
{
    (void)t;
    (void)abs_us;
    return 0;
}

/* ------------------------------------------------------------------ 生命周期 */

int coop_early_init(void)
{
    s_head = BR_NULL;
    s_tail = BR_NULL;
    s_len  = 0u;

    s_ops.name         = "sched/coop";
    s_ops.kind         = BR_SCHED_KIND_COOP;
    s_ops.tcb_size     = (br_u32)(br_sched_tcb_size() + sizeof(struct coop_priv));
    s_ops.thread_ready = coop_thread_ready;
    s_ops.thread_block = coop_thread_block;
    s_ops.pick_next    = coop_pick_next;
    s_ops.on_tick      = BR_NULL;      /* coop 无时间片: 不做 tick 策略 */
    s_ops.sleep_until  = coop_sleep_until;
    s_ops.idle         = BR_NULL;      /* 缺省 = core 的 L2 临界区 + WFI(3-02 §11.1) */

    br_sched_register(&s_ops);         /* 恰一次; 二次注册 = core panic */
    return 0;
}

int coop_init(void)
{
    return 0;
}

int coop_start(void)
{
    return 0;
}
