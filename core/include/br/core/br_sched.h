/*
 * brickOS prototype v0.2.0 — 调度框架(core): 线程/任务 + 调度器注册点
 *
 * 设计依据: `3-01` §5.1(`br_sched_ops` 注册点)、§6(任务/线程)、§14 CA-1(时间单位),
 *           `3-03`(调度器总纲)、`3-02` §16.4(IRQ ↔ 调度接缝)。
 *
 * ## 分工(ADR-0006 §1; 与设计的两处偏离在这里说清)
 *
 * | 面 | 归属 |
 * |---|---|
 * | 上下文切换 asm / TCB 公共头 / 栈与 trampoline / 超时唤醒框架 / idle / 首次调度 | **core** |
 * | 就绪队列 / `pick_next` / tick 策略 / 抢占决策 / 调度表 | **调度器插件** |
 * | 阻塞与唤醒的**机制**(block_current / wake / 等待链) | **core**(见 §偏离 1) |
 * | 锁与信号量的**对象与语义**(mutex/sem/cond) | **core**(`br_sync.h`) |
 *
 * **偏离 1**: 设计 `3-01` §5.1 把 `sem_take/mutex_lock/…` 放进 `br_sched_ops`
 * (即"同步实现在调度插件", `3-01` §115 行)。本原型改为 **core 实现同步原语**,
 * 插件只提供"下一个人是谁"。理由: (a) 锁对象要被**所有**插件看见, 放进调度插件会让
 * 每个使用者反向依赖插件头; (b) 阻塞/唤醒机制对 coop 与 preempt 是同一套, 只有
 * "谁先跑"不同; (c) 设计的 ops 字段表本身是**带省略号的草案**(`3-03` §2 待成文)。
 * 逐条登记在 ADR-0006 §2。
 *
 * **偏离 2**: `br_sched_ops` 的字段表在此**成文**(补齐设计省略的 5 类槽位):
 * `tcb_size`(TCB 总尺寸: core 公共头 + 插件私有尾)、`sleep_until`(登记唤醒期限)、
 * `idle`(插件自定义 idle; 缺省走 core 的 WFI 循环)。原草案里的
 * `sem_take/sem_give/mutex_lock/mutex_unlock/work_submit/irq_epilogue` 不再进 ops
 * (见偏离 1 与下面的 `br_sched_irq_epilogue`)。
 */
#ifndef BR_CORE_BR_SCHED_H
#define BR_CORE_BR_SCHED_H

#include <br/core/br_types.h>
#include <br/core/br_time.h>

/* ==================================================================== 线程 */

/* TCB 是**不透明**的: 公共头在 `core/src/sched/sched_internal.h`, 插件私有尾跟在后面
 * (`tcb_size` 由插件给)。插件头只需要这个前向声明。 */
typedef struct br_thread br_thread_t;

/* 线程状态(观测与自检用; 设计 `3-01` §6 的状态机)。 */
#define BR_TASK_NEW      0u
#define BR_TASK_READY    1u
#define BR_TASK_RUNNING  2u
#define BR_TASK_BLOCKED  3u
#define BR_TASK_ZOMBIE   4u

/* 线程栈的最小尺寸(设计 `3-01` §2.2 的 `BR_STACK_MIN [?]`; 原型取 2 KiB)。
 * 小于它 ⇒ `br_task_create` 返回 `-EINVAL`。 */
#define BR_STACK_MIN 2048u

/* 创建参数。`stack` 由**调用方**提供(无动态分配 —— 原型口径, ADR-0006 §2)。
 * `stack` 指向栈区间的**高地址端**(栈顶), 区间 = `[stack - stack_size, stack)`。 */
typedef struct br_task_attr {
    const char *name;
    void       *stack;        /* 栈顶(高地址); aarch64 栈向下生长 */
    br_size_t   stack_size;
    br_u8       prio;         /* 0 = 最低; 只对 preempt/tt 有意义(coop 忽略) */
    br_u32      flags;        /* 保留(0) */
} br_task_attr_t;

/* 创建线程: NEW → READY(进就绪队列)。失败: -EINVAL / -ENOMEM(TCB 池满) / -ENOTSUP。 */
int br_task_create(br_thread_t **out, const br_task_attr_t *attr,
                   void (*entry)(void *), void *arg);

/* 等一个线程结束(ZOMBIE → 回收 TCB)。`exit_code` 可空。 */
int br_task_join(br_thread_t *t, int *exit_code);

/* 结束当前线程(不返回)。 */
BR_NORETURN void br_task_exit(int code);

/* 让出 CPU(READY 队尾; 立刻触发一次调度)。coop 下这是唯一的"自愿让出"。 */
void br_task_yield(void);

/* 睡眠: `rel_us` 微秒后变 READY。0 ⇒ 立即返回 0; BR_TIMEOUT_INF ⇒ -EINVAL
 * (裁定 G13: 相对睡眠没有"无限"的语义 —— 那是不返回)。保证**不早醒**,
 * 晚到无上界(`3-01` INV-2)。 */
int br_task_sleep(br_time_t rel_us);
int br_task_sleep_until(br_time_t abs_us);

br_thread_t *br_task_self(void);
const char *br_task_name(const br_thread_t *t);
br_u32 br_task_state(const br_thread_t *t);

/* ==================================================================== 注册点 */

/* 调度器形态(与插件侧 `[plugin].sched_kind` 对应)。 */
#define BR_SCHED_KIND_COOP    0u
#define BR_SCHED_KIND_PREEMPT 1u
#define BR_SCHED_KIND_TT      2u

/*
 * 调度器插件实现的**全部**接口(偏离 2 的成文表)。core 只在这些点回调。
 * 除 `idle` 外都不可为空(空 ⇒ 注册被拒, -EINVAL)。
 */
typedef struct br_sched_ops {
    const char *name;         /* 呈现用(如 "sched/coop") */
    br_u32      kind;         /* BR_SCHED_KIND_* */
    br_u32      tcb_size;     /* 每个 TCB 的总字节数(>= core 公共头尺寸) */

    /* NEW/唤醒 → READY 时调用(把 t 挂进就绪结构)。 */
    int  (*thread_ready)(br_thread_t *t);
    /* RUNNING → BLOCKED 时调用(从就绪结构里摘掉)。 */
    void (*thread_block)(br_thread_t *t);
    /* 下一个该跑的线程; BR_NULL ⇒ "没人可跑"(core 进 idle/WFI)。 */
    br_thread_t *(*pick_next)(void);
    /* tick 驱动点(定时器中断里调; COOP 可为 BR_NULL)。 */
    void (*on_tick)(void);
    /* 登记"到 abs_us 唤醒 t"(core 的超时框架会遍历唤醒)。可为 BR_NULL(不支持超时)。 */
    int  (*sleep_until)(br_thread_t *t, br_time_t abs_us);
    /* 自定义 idle(可为 BR_NULL ⇒ 用 core 的 WFI 临界区)。 */
    void (*idle)(void);
} br_sched_ops_t;

/* EARLY 相注册, **恰一次**(二次 ⇒ panic, `3-01` §5.1)。 */
void br_sched_register(const br_sched_ops_t *ops);

/* 有没有注册过(插件管理器的过渡桥用它: 没有调度器时由 APP 的 start 占住 CPU)。 */
br_bool br_sched_registered(void);

/* 首次调度 + idle 循环(不返回)。由插件管理器在全部 start() 之后调用。 */
BR_NORETURN void br_sched_run(void);

/* IRQ 出口决策点(`core/src/irq/irq_core.c` 已经在 IRQ 退出路径上调用它)。
 * COOP 下可以是空动作; PREEMPT 在这里做"要不要切"。 */
void br_sched_irq_epilogue(void);

/* 定时器 tick 的驱动点(平台 timer ISR 调): 推进内核时间基并唤醒到期线程,
 * 然后回调 `ops.on_tick`。 */
void br_sched_on_tick(br_time_t now);

/* 当前线程主动阻塞(等待链的机制面; `br_sync.h` 的原语建在它上面)。
 * `abs_deadline == BR_TIMEOUT_INF` ⇒ 无限等待。返回 0 = 被唤醒,
 * -ETIMEDOUT = 超时。
 *
 * ★ **ICB 握手前置义务**(ADR-0006 §3b; 与 `sync.c` 逐条对齐): 调用方必须在
 *   **开窗之前**(关中断临界区内)先把 `br_task_self()->wait_status` 置成
 *   `-ETIMEDOUT`, 然后才挂等待链、开中断、调本函数。本函数入口若读到
 *   `wait_status == 0`, 就认为"在窗口里已经被 `br_sched_wake` 交接过了", 直接返回 0
 *   且**不置 BLOCKED / 不登记期限 / 不切栈** —— 这是丢掉"push 之后、真正阻塞之前"
 *   窗口的唯一手段。`wait_status` 是唯一握手位: 谁先写谁赢。 */
int br_sched_block_current(br_time_t abs_deadline);

/* 把 t 变回 READY(ISR-safe: 只改状态 + 挂链, 不动栈)。
 * ICB 步 3: **先**写 `t->wait_status = 0`, 只有 `t->state == BLOCKED` 才置 READY + 入队;
 * 否则只写握手位(窗口里的早唤醒)。幂等。 */
void br_sched_wake(br_thread_t *t);

/* TCB 公共头的字节数(= 插件私有尾的偏移)。插件在填 `br_sched_ops.tcb_size` 时用它:
 *   `tcb_size = br_sched_tcb_size() + sizeof(私有尾)`
 * (设计 `3-01` §2.2 的 CA-2 同型; 私有尾由插件经 `br_thread_priv` 取用。) */
br_size_t br_sched_tcb_size(void);

/* 插件私有尾指针(紧跟在公共头之后)。core 不碰它; 插件只读写自己的尾。 */
void *br_thread_priv(br_thread_t *t);

/* 就绪/阻塞/存活计数(观测; dump 与自检用)。 */
br_u32 br_sched_task_count(void);
br_u32 br_sched_ready_count(void);

/*
 * 这里**不再**声明调度套件的入口 `br_sched_selftest()`。理由: 自检入口**不是插件的
 * 对外能力**, 留在本头就会进 golden 接口面 —— 于是"改一个用例"变成接口变更(与
 * ADR-0005 裁定 9 对钩子的处置同源)。声明集中在 `core/selftest/core_selftest.c`,
 * 实现随套件一起在 `core/selftest/sched_selftest.c`(ADR-0010)。
 */

#endif /* BR_CORE_BR_SCHED_H */
