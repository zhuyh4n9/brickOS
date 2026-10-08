/*
 * brickOS prototype v0.2.0 — 调度框架(core): 线程状态机 + 超时唤醒 + idle + 首次调度
 *
 * 设计依据: `3-03-sched.md`(调度总纲)、`3-01-core-api-list.md` §2/§5.1/§14(CA-1/CA-4)、
 *           `3-02-int.md` §11.1–§11.3(IRQ ↔ 调度接缝; idle 的 L2 临界区处方在 §11.1 末)、
 *           `1-01-architecture.md` §9(启动序列)、`6-01-test.md` §3.1/§3.3。
 * 逐条决策与设计缺口裁定: `docs/decisions/0006-scheduler-framework.md`(本文件是它的落地)。
 *
 * ## 分工
 *   core : 上下文切换 asm(`switch.S`)/ TCB 公共头(`sched_internal.h`)/ 栈与 trampoline /
 *          超时唤醒框架 / idle / **线程状态机** / conformance
 *   插件 : 就绪结构 / `pick_next` / tick 策略 / 抢占决策(`sched/coop` = FIFO + 只在显式点切)
 *
 * ## 关键不变量(ADR-0006 §1; 逐条都有机械判据)
 *   I1  `br_sched_register()` 把**调用者所在的启动上下文**物质化为 main 线程
 *       (RUNNING, 名字 "main")。register 之前 `br_task_self() == BR_NULL`, 之后恒 == main。
 *       ⇒ `br_task_create/join` 在 `br_sched_run()` **之前**就能工作(APP 的 start() 相),
 *          `br_sched_run()` 只是"进入 idle / 首次调度"的入口, 不是调度器的启动前提。
 *   I2  只有 core 改 `state`。插件只动"就绪结构的成员关系": READY ⇄ 队列;
 *       RUNNING ⇒ 不在队列; `pick_next` 只出队, 由 core 置 RUNNING。
 *   I3  `br_sched_wake(t)` 先写 `t->wait_status = 0`, 只有 `state == BLOCKED` 才挂回就绪队列
 *       (否则只写握手位)。超时扫描只碰 `state == BLOCKED` 且到期的线程。
 *   I4  **无抢占**(coop): 只在显式点(yield / block / exit / 首次调度)换栈;
 *       IRQ 出口(`br_sched_irq_epilogue`)不切栈(3-02 §11.2/§11.3)。
 *   I5  ICB 握手(ADR-0006 §3b, 与 F3 的 sync.c 对齐): `wait_status` 是唯一握手位 ——
 *       调用方在开窗前置 -ETIMEDOUT, wake 置 0, 超时置 -ETIMEDOUT;
 *       谁先写谁赢, 两种顺序收敛到同一结果(见 `br_sched_block_current`)。
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include "sched_ctx.h"
#include "sched_internal.h"

/* =====================================================================
 * TCB 池(静态; 原型口径 "无动态分配", ADR-0006 §2 裁定 S-3)
 *
 * 槽位 = 公共头 + 插件私有尾。插件在 `ops.tcb_size` 里报总尺寸; 池按**编译期上界**
 * `BR_TCB_SLOT_BYTES` 划, 注册时校验 `tcb_size <= BR_TCB_SLOT_BYTES` —— 超了是插件
 * 与 core 的池配置不匹配, 注册即报错(不静默截断)。
 * ===================================================================== */
#define BR_TCB_SLOT_BYTES 256u

typedef union br_tcb_slot {
    struct br_thread t;
    br_u64           align;
    br_u8            raw[BR_TCB_SLOT_BYTES];
} br_tcb_slot_t;

_Static_assert(sizeof(struct br_thread) <= BR_TCB_SLOT_BYTES,
               "BR_TCB_SLOT_BYTES 装不下 TCB 公共头 —— 调整 sched_core.c 的池上界");
_Static_assert(__builtin_offsetof(struct br_thread, ctx) == BR_THREAD_CTX_OFF,
               "ctx 偏移与 switch.S / sched_ctx.h 漂移(换栈会踩错字)");

static br_tcb_slot_t        s_pool[BR_TASK_MAX];
static br_thread_t         *s_all;        /* 全局 TCB 链(遍历/观测/超时扫描) */
static br_thread_t         *s_current;    /* 当前线程(单核; TPIDR_EL1 是 v2/SMP 的落点) */
static const br_sched_ops_t *s_ops;
static br_thread_t         *s_joiner[BR_TASK_MAX];  /* target → 它的 joiner(一次一个) */

/*
 * 宿主侧 idle 等待钩子(弱符号)。目标上根本没有这个符号: idle 直接发 `wfi`
 * (3-02 §11.1 的处方: 关中断 → 检查无就绪 → WFI → 开中断)。
 * 宿主没有真中断源, 也没有 WFI —— 用例在这里推进假时钟并调 `br_sched_on_tick()`,
 * 于是"周期 tick 上的超时扫描"在宿主上可测(见 tests/host/sched_test.c)。
 */
#if !defined(__aarch64__)
void br_sched_host_wait(void) __attribute__((weak));
#endif

/* =====================================================================
 * 小工具
 * ===================================================================== */

br_size_t br_sched_tcb_size(void)
{
    return (br_size_t)sizeof(struct br_thread);
}

void *br_thread_priv(br_thread_t *t)
{
    return (void *)((br_u8 *)t + sizeof(struct br_thread));
}

/* =====================================================================
 * 等待链四原语(零分配: 节点 = TCB 里的 `wait_next`; 同步原语在这里挂/摘等待者)
 *
 * 为什么在 core 而不是各同步对象里: 节点布局属于冻结的 TCB(`sched_internal.h`),
 * 而且 `br_sched_block_current()` 的返回路径要求"摘链"与"握手位"能配合使用。
 * 这四个不依赖调度器状态(可以在没有 READY 线程时用), 所以同步路径永远可用。
 * ===================================================================== */

void br_waitq_push(void **head, br_thread_t *t)
{
    if (head == BR_NULL || t == BR_NULL) {
        return;
    }
    t->wait_next = BR_NULL;
    if (*head == BR_NULL) {
        *head = t;
        return;
    }
    br_thread_t *p = (br_thread_t *)*head;
    while (p->wait_next != BR_NULL) {
        p = p->wait_next;
    }
    p->wait_next = t;
}

br_thread_t *br_waitq_pop(void **head)
{
    if (head == BR_NULL || *head == BR_NULL) {
        return BR_NULL;
    }
    br_thread_t *t = (br_thread_t *)*head;

    *head = t->wait_next;
    t->wait_next = BR_NULL;
    return t;
}

int br_waitq_remove(void **head, br_thread_t *t)   /* 1 = 摘到了, 0 = 不在链上 */
{
    if (head == BR_NULL || t == BR_NULL) {
        return 0;
    }
    br_thread_t *prev = BR_NULL;
    br_thread_t *p = (br_thread_t *)*head;

    while (p != BR_NULL) {
        if (p == t) {
            if (prev == BR_NULL) {
                *head = p->wait_next;
            } else {
                prev->wait_next = p->wait_next;
            }
            p->wait_next = BR_NULL;
            return 1;
        }
        prev = p;
        p = p->wait_next;
    }
    return 0;
}

br_u32 br_waitq_len(const void *head)
{
    br_u32 n = 0u;

    for (const br_thread_t *p = (const br_thread_t *)head; p != BR_NULL; p = p->wait_next) {
        n++;
    }
    return n;
}

/* TCB 指针 → 池下标(不在池里 ⇒ BR_TASK_MAX)。 */
static br_u32 tcb_index(const br_thread_t *t)
{
    const br_uintptr_t base = (br_uintptr_t)(const void *)&s_pool[0];
    const br_uintptr_t p    = (br_uintptr_t)(const void *)t;

    if (p < base || p >= (base + (br_uintptr_t)sizeof(s_pool))) {
        return BR_TASK_MAX;
    }
    return (br_u32)((p - base) / (br_uintptr_t)BR_TCB_SLOT_BYTES);
}

/* CPU 等待(目标 = WFI; 宿主 = 弱钩子)。调用者必须已关中断(L2)。 */
static void cpu_wait(void)
{
#if defined(__aarch64__)
    __asm__ volatile("wfi");
#else
    if (br_sched_host_wait != BR_NULL) {
        br_sched_host_wait();
    }
#endif
}

static br_u32 ready_count_now(void)
{
    br_u32 n = 0u;

    for (const br_thread_t *t = s_all; t != BR_NULL; t = t->all_next) {
        if (t->state == BR_TASK_READY) {
            n++;
        }
    }
    return n;
}

/* 槽位清空(整槽, 含私有尾 —— 复用时不残留上一任的队列指针)。 */
static void tcb_clear(br_thread_t *t)
{
    br_u8 *raw = (br_u8 *)(void *)t;

    for (br_size_t i = 0u; i < BR_TCB_SLOT_BYTES; i++) {
        raw[i] = 0u;
    }
}

static br_thread_t *tcb_alloc(void)
{
    for (br_u32 i = 0u; i < BR_TASK_MAX; i++) {
        if (s_pool[i].t.magic != BR_THREAD_MAGIC) {
            br_thread_t *t = &s_pool[i].t;

            tcb_clear(t);
            t->magic   = BR_THREAD_MAGIC;
            t->state   = BR_TASK_NEW;
            t->wake_at = BR_TIMEOUT_INF;

            const br_irq_state_t st = br_irq_lock();
            t->all_next = s_all;
            s_all = t;
            br_irq_unlock(st);
            return t;
        }
    }
    return BR_NULL;
}

static void tcb_free(br_thread_t *t)
{
    const br_u32 i = tcb_index(t);

    if (i < BR_TASK_MAX) {
        s_joiner[i] = BR_NULL;
    }

    const br_irq_state_t st = br_irq_lock();
    br_thread_t **pp = &s_all;
    while (*pp != BR_NULL) {
        if (*pp == t) {
            *pp = t->all_next;
            break;
        }
        pp = &(*pp)->all_next;
    }
    tcb_clear(t);              /* magic 归零 = 槽位空闲 */
    br_irq_unlock(st);
}

/* =====================================================================
 * 唤醒 / 超时(ICB 握手, I3/I5)
 * ===================================================================== */

/* 把 t 挂回就绪结构(state 由 core 改; 插件只入队)。调用者持 L2 或不要求原子。 */
static void make_ready(br_thread_t *t)
{
    t->state = BR_TASK_READY;
    if (s_ops != BR_NULL && s_ops->thread_ready != BR_NULL) {
        (void)s_ops->thread_ready(t);
    }
}

void br_sched_wake(br_thread_t *t)
{
    if (t == BR_NULL || t->magic != BR_THREAD_MAGIC || s_ops == BR_NULL) {
        return;
    }

    const br_irq_state_t st = br_irq_lock();

    /*
     * ICB 步 3: **先**写握手位。若 t 此刻还是 RUNNING(调用方还在"开窗之后、
     * 真正 BLOCKED 之前"), 这里就是"窗口里的早唤醒" —— 只写位, 不入队;
     * `br_sched_block_current` 会在入口看到 0 并直接返回, 不切栈。
     */
    t->wait_status = 0;

    if (t->state == BR_TASK_BLOCKED) {
        t->wake_at = BR_TIMEOUT_INF;   /* 停止跟踪期限 */
        make_ready(t);
    } else {
        /* 已 READY / RUNNING / ZOMBIE: 只交接握手位(幂等) */
    }

    br_irq_unlock(st);
}

/* 超时到期: 只对 BLOCKED 且到期的线程(ICB 步 4)。 */
static void make_timed_out(br_thread_t *t)
{
    t->wait_status = BR_ERR(BR_ETIMEDOUT);
    t->wake_at = BR_TIMEOUT_INF;
    make_ready(t);
}

static void scan_timeouts(br_time_t now)
{
    if (s_ops == BR_NULL) {
        return;
    }

    for (br_thread_t *t = s_all; t != BR_NULL; t = t->all_next) {
        if (t->state != BR_TASK_BLOCKED) {
            continue;                  /* 已 READY/已被 wake 的不得再被超时 */
        }
        if (t->wake_at == BR_TIMEOUT_INF || t->wake_at > now) {
            continue;
        }
        make_timed_out(t);
    }
}

/* =====================================================================
 * 切换: pick_next → 换栈; 无就绪者 → idle
 * ===================================================================== */

static void idle_wait(void)
{
    if (s_ops->idle != BR_NULL) {
        s_ops->idle();
        return;
    }

    /*
     * 3-02 §11.1 的处方(逐字): **关中断 → 检查"无就绪工作" → WFI → 开中断**。
     * 检查与 WFI 之间不能被打断: 若在检查之后、WFI 之前来了中断并唤醒了一个线程,
     * 那次 WFI 会一直睡到下一个中断(可能很久)才醒 —— 这就是经典的 idle 丢唤醒。
     * WFI 在有 pending 中断时立即返回(即使 PSTATE.I == 1), 所以这里的关中断不
     * 影响它被中断唤醒。
     */
    const br_irq_state_t st = br_irq_lock();
    if (ready_count_now() != 0u) {
        br_irq_unlock(st);
        return;
    }
    cpu_wait();
    br_irq_unlock(st);
}

static void switch_next(void)
{
    for (;;) {
        br_thread_t *to = (s_ops->pick_next != BR_NULL) ? s_ops->pick_next() : BR_NULL;

        if (to == BR_NULL) {
            /* 没人可跑 ⇒ idle(WFI 临界区); 醒来后重新 pick */
            idle_wait();
            continue;
        }
        if (to == s_current) {
            /*
             * 唯一可运行者就是自己: 睡眠者被超时叫醒、或 yield 时没有同级可让。
             * 不切栈(切给自己没有意义), 把状态置回 RUNNING 直接继续。
             */
            to->state = BR_TASK_RUNNING;
            return;
        }

        br_thread_t *from = s_current;
        to->state = BR_TASK_RUNNING;
        s_current = to;
        br_sched_switch_to(from, to);   /* 回来 = 有人又把 from 选出来了 */
        return;
    }
}

/* =====================================================================
 * 注册点
 * ===================================================================== */

br_bool br_sched_registered(void)
{
    return (s_ops != BR_NULL) ? BR_TRUE : BR_FALSE;
}

void br_sched_register(const br_sched_ops_t *ops)
{
    if (s_ops != BR_NULL) {
        br_panic("[sched] br_sched_register(): 二次注册(3-01 §5.1 '恰一次')");
    }
    if (ops == BR_NULL) {
        br_panic("[sched] br_sched_register(): ops == NULL");
    }
    /* 必需槽位(ADR-0006 §2 裁定 S-1: 逐字段注释优先于"除 idle 外都不可为空"的总述) */
    if (ops->name == BR_NULL || ops->thread_ready == BR_NULL ||
        ops->thread_block == BR_NULL || ops->pick_next == BR_NULL) {
        br_panic("[sched] br_sched_register(): name/thread_ready/thread_block/pick_next 必填");
    }
    if (ops->kind > BR_SCHED_KIND_TT) {
        br_panic("[sched] br_sched_register(): kind 非法");
    }
    if (ops->tcb_size < (br_u32)sizeof(struct br_thread) ||
        ops->tcb_size > BR_TCB_SLOT_BYTES) {
        br_panic("[sched] br_sched_register(): tcb_size 与 core TCB 池不匹配");
    }

    s_ops = ops;

    /*
     * I1: 把调用者所在的启动上下文物质化为 main 线程。
     * 这一步之后 `br_task_self()` 恒返回它 —— 于是 APP 的 start() 相里创建的线程
     * 与 join 立刻就能工作(3-01 §9 的启动序列), 不必等 `br_sched_run()`。
     */
    br_thread_t *m = tcb_alloc();
    if (m == BR_NULL) {
        br_panic("[sched] br_sched_register(): TCB 池满, 建不出 main");
    }
    m->state   = BR_TASK_RUNNING;
    m->name    = "main";
    m->prio    = 0u;
    m->wake_at = BR_TIMEOUT_INF;
    s_current  = m;
}

BR_NORETURN void br_sched_run(void)
{
    if (s_ops == BR_NULL) {
        br_panic("[sched] br_sched_run(): 没有注册调度器(插件管理器的过渡桥不该走到这里)");
    }
    if (s_current == BR_NULL) {
        br_panic("[sched] br_sched_run(): register 未建立 main 线程");
    }

    for (;;) {
        switch_next();
    }
}

void br_sched_irq_epilogue(void)
{
    if (s_ops == BR_NULL) {
        return;
    }

    /*
     * 到期扫描(与 on_tick 同一处方): 覆盖"非 timer 中断"的出口, 使超时唤醒不依赖
     * 具体是哪一个 IRQ 把 CPU 从 idle 里叫醒的。
     * COOP 下**不在此切栈**(3-02 §11.2: Stage 1 = 空动作; v1 coop 只在显式点切换)。
     * v2 preempt 在这里检查 need_resched 并切栈(必须在 eoi 之后、ERET 之前)。
     */
    scan_timeouts(br_clock_now());
}

void br_sched_on_tick(br_time_t now)
{
    if (s_ops == BR_NULL) {
        return;
    }

    scan_timeouts(now);

    if (s_ops->on_tick != BR_NULL) {
        s_ops->on_tick();
    }
}

int br_sched_block_current(br_time_t abs_deadline)
{
    br_thread_t *t = s_current;

    if (t == BR_NULL || s_ops == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (t->state != BR_TASK_RUNNING) {
        return BR_ERR(BR_EINVAL);
    }

    const br_irq_state_t st = br_irq_lock();

    /*
     * ICB 步 2(ADR-0006 §3b): 入口先看握手位。调用方在**开窗之前**已经把它置成
     * -ETIMEDOUT; 若这里读到 0, 说明 `br_sched_wake()` 在"调用方开窗之后、本函数
     * 关中断之前"已经来过 —— 直接返回 0, 不置 BLOCKED、不登记期限、不切栈。
     * 这把"push 等待链之后 / 真正 BLOCKED 之前"的丢唤醒窗口彻底消掉。
     */
    if (t->wait_status == 0) {
        br_irq_unlock(st);
        return 0;
    }

    /* 期限已到(含相对 0 的绝对形态)⇒ 立即按超时收场(INV-1), 不切栈、不睡到下一个 tick */
    if (abs_deadline != BR_TIMEOUT_INF && abs_deadline <= br_clock_now()) {
        t->wait_status = BR_ERR(BR_ETIMEDOUT);
        br_irq_unlock(st);
        return BR_ERR(BR_ETIMEDOUT);
    }

    t->wake_at = abs_deadline;          /* BR_TIMEOUT_INF = 无期限 */
    t->state   = BR_TASK_BLOCKED;
    if (s_ops->thread_block != BR_NULL) {
        s_ops->thread_block(t);
    }
    br_irq_unlock(st);

    switch_next();                      /* 交出 CPU; 回来时 state 已被 pick 侧置回 RUNNING */
    return t->wait_status;
}

br_u32 br_sched_task_count(void)
{
    br_u32 n = 0u;

    for (br_u32 i = 0u; i < BR_TASK_MAX; i++) {
        if (s_pool[i].t.magic == BR_THREAD_MAGIC) {
            n++;
        }
    }
    return n;
}

/* 内部契约(`sched_internal.h`)的池观测: 已占用槽位数(= 存活线程数)。 */
br_u32 br_thread_pool_used(void)
{
    return br_sched_task_count();
}

br_u32 br_sched_ready_count(void)
{
    return ready_count_now();
}

/* =====================================================================
 * 线程面(状态机: NEW → READY ⇄ RUNNING / BLOCKED / ZOMBIE)
 * ===================================================================== */

BR_NORETURN void br_sched_thread_entry(struct br_thread *t)
{
    if (t != BR_NULL && t->entry != BR_NULL) {
        t->entry(t->arg);               /* entry 正常返回 ⇒ 走下面, 不落野地址 */
    }
    br_task_exit(0);
}

int br_task_create(br_thread_t **out, const br_task_attr_t *attr,
                   void (*entry)(void *), void *arg)
{
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    *out = BR_NULL;

    if (attr == BR_NULL || entry == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (attr->stack == BR_NULL || attr->stack_size < BR_STACK_MIN) {
        return BR_ERR(BR_EINVAL);
    }
    if (s_ops == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }

    br_thread_t *t = tcb_alloc();
    if (t == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }

    t->name       = (attr->name != BR_NULL) ? attr->name : "task";
    t->prio       = attr->prio;
    t->stack_top  = attr->stack;                 /* 契约: stack = 栈顶(高地址) */
    t->stack_size = attr->stack_size;
    t->entry      = entry;
    t->arg        = arg;
    t->exit_code  = 0;
    t->wait_status = BR_ERR(BR_ETIMEDOUT);       /* ICB: 开窗前先置"未唤醒" */
    t->wake_at    = BR_TIMEOUT_INF;

    /*
     * 初始上下文(与 switch.S 的 ctx 布局逐字一致):
     *   x19 = TCB(trampoline 读它)   x30 = trampoline   sp = 栈顶(16 字节对齐)
     * x29 = 0: fp 链从 0 起(backtrace 靠它停下来)。
     */
    t->ctx[BR_CTX_X19_TCB] = (br_u64)(br_uintptr_t)t;
    t->ctx[BR_CTX_X29]     = 0u;
    t->ctx[BR_CTX_X30]     = (br_u64)(br_uintptr_t)br_sched_thread_trampoline;
    t->ctx[BR_CTX_SP]      = (br_u64)(((br_uintptr_t)attr->stack) & ~(br_uintptr_t)15u);

    t->state = BR_TASK_READY;
    if (s_ops->thread_ready != BR_NULL) {
        const int r = s_ops->thread_ready(t);
        if (r != 0) {
            tcb_free(t);
            return r;
        }
    }

    *out = t;
    return 0;
}

int br_task_join(br_thread_t *t, int *exit_code)
{
    if (t == BR_NULL || t->magic != BR_THREAD_MAGIC) {
        return BR_ERR(BR_EINVAL);
    }
    if (t == s_current) {
        return BR_ERR(BR_EINVAL);                   /* 自 join */
    }

    const br_u32 i = tcb_index(t);
    if (i >= BR_TASK_MAX || s_joiner[i] != BR_NULL) {
        return BR_ERR(BR_EINVAL);                   /* 池外 / 已被别人 join */
    }

    if (t->state != BR_TASK_ZOMBIE) {
        s_joiner[i] = s_current;
        s_current->wait_status = BR_ERR(BR_ETIMEDOUT);   /* ICB: 开窗 */
        const int w = br_sched_block_current(BR_TIMEOUT_INF);
        s_joiner[i] = BR_NULL;
        if (w != 0) {
            return w;                               /* join 无期限 ⇒ 正常不会发生 */
        }
    }

    if (exit_code != BR_NULL) {
        *exit_code = t->exit_code;
    }
    tcb_free(t);                                    /* ZOMBIE → 回收 */
    return 0;
}

BR_NORETURN void br_task_exit(int code)
{
    br_thread_t *t = s_current;

    if (t == BR_NULL || s_ops == BR_NULL) {
        /* 不在线程上下文: 没有"退出"可言 —— 诚实停住(不伪造) */
        for (;;) {
            cpu_wait();
        }
    }

    const br_u32 i = tcb_index(t);

    t->exit_code = code;
    t->state     = BR_TASK_ZOMBIE;
    t->wake_at   = BR_TIMEOUT_INF;
    if (s_ops->thread_block != BR_NULL) {
        s_ops->thread_block(t);                     /* 防御性摘除(通常不在队列里) */
    }

    /* 唤醒 joiner(ICB: 即使它还在 RUNNING 的窗口里, 也只写握手位) */
    if (i < BR_TASK_MAX && s_joiner[i] != BR_NULL) {
        br_sched_wake(s_joiner[i]);
    }

    for (;;) {
        switch_next();                              /* ZOMBIE 不再入队; 换到别人或 idle */
    }
}

void br_task_yield(void)
{
    if (s_current == BR_NULL || s_ops == BR_NULL) {
        return;
    }
    if (s_current->state != BR_TASK_RUNNING) {
        return;
    }

    s_current->state = BR_TASK_READY;
    if (s_ops->thread_ready != BR_NULL) {
        (void)s_ops->thread_ready(s_current);
    }
    switch_next();
}

int br_task_sleep(br_time_t rel_us)
{
    if (rel_us == BR_TIMEOUT_INF) {
        return BR_ERR(BR_EINVAL);       /* 裁定 G13: 相对睡眠没有"无限"语义 */
    }
    if (rel_us == 0u) {
        return 0;                       /* 立即返回(TC-TIME-002) */
    }
    return br_task_sleep_until(br_deadline_from_now(rel_us));
}

int br_task_sleep_until(br_time_t abs_us)
{
    if (s_current == BR_NULL || s_ops == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (abs_us == 0u || (abs_us != BR_TIMEOUT_INF && abs_us <= br_clock_now())) {
        return 0;                       /* 期限已过 ⇒ 立即返回 0(TC-TASK-005 口径) */
    }

    s_current->wait_status = BR_ERR(BR_ETIMEDOUT);   /* ICB: 开窗 */

    const int r = br_sched_block_current(abs_us);

    /*
     * 裁定 G13b(ADR-0006 §2): 睡眠的**到期是成功路径** ⇒ 返回 0;
     * -ETIMEDOUT 只属于"等某个事件而超时"的 API(mutex/sem/cond/join, INV-1)。
     */
    return (r == BR_ERR(BR_ETIMEDOUT)) ? 0 : r;
}

br_thread_t *br_task_self(void)
{
    return s_current;
}

const char *br_task_name(const br_thread_t *t)
{
    if (t == BR_NULL || t->magic != BR_THREAD_MAGIC) {
        return "?";
    }
    return (t->name != BR_NULL) ? t->name : "?";
}

br_u32 br_task_state(const br_thread_t *t)
{
    if (t == BR_NULL || t->magic != BR_THREAD_MAGIC) {
        return BR_TASK_NEW;
    }
    return t->state;
}

/* =====================================================================
 * 一致性用例(TC-TASK-* / TC-TIME-*, 6-01 §3.1/§3.3)
 *
 * 形态: **自身运行在一个真实线程里**(APP 线程 / 宿主监督线程), 真的
 * create/yield/join/sleep —— 于是它验证的是切换/阻塞/唤醒/超时, 不只是查数据结构。
 * 每个子用例的线程栈都是**静态数组**(原型无动态分配)。
 * ===================================================================== */

#define BR_CONF_STACK_BYTES 4096u
#define BR_CONF_BUSY_ITERS  200000u

static int s_conf_pass;
static int s_conf_fail;

static br_u8 s_conf_stack[BR_TASK_MAX][BR_CONF_STACK_BYTES] BR_ALIGN(16);
static br_u8 s_busy_stack[BR_CONF_STACK_BYTES] BR_ALIGN(16);
static br_u8 s_victim_stack[BR_CONF_STACK_BYTES] BR_ALIGN(16);

static volatile br_uintptr_t s_probe_addr;
static volatile br_time_t    s_sleep_delta;
static volatile int          s_sleep_rc;
static volatile int          s_flag_a;
static volatile int          s_seq_n;
static volatile char         s_seq[32];
static volatile br_u32       s_victim_ticks;
static volatile int          s_preempt_seen;
static volatile int          s_victim_after_yield;

static void conf_result(br_bool ok, const char *id, const char *what)
{
    if (ok != BR_FALSE) {
        s_conf_pass++;
        br_log_info("[TASKCONF] PASS %s %s", id, what);
    } else {
        s_conf_fail++;
        br_log_info("[TASKCONF] FAIL %s %s", id, what);
    }
}

static br_task_attr_t conf_attr(br_u32 slot, const char *name)
{
    br_task_attr_t a;

    a.name       = name;
    a.stack      = (void *)(s_conf_stack[slot % BR_TASK_MAX] + BR_CONF_STACK_BYTES);
    a.stack_size = BR_CONF_STACK_BYTES;
    a.prio       = 0u;
    a.flags      = 0u;
    return a;
}

/* --- 各用例的线程体 ---------------------------------------------------  * WORKAROUND(br-wa-test-001): 本套件的 `TC-TASK-*`/`TC-TIME-*` id 与 `6-01` §3.1/§3.3 的
 * 表**尚未逐条对齐**(含义漂移, 自述文字准确); 对齐动作见 WORKAROUNDS.md。
 */

static void conf_entry_ok(void *arg)
{
    (void)arg;                          /* 正常返回 ⇒ trampoline 调 br_task_exit(0) */
}

static void conf_entry_code(void *arg)
{
    br_task_exit((int)(br_intptr_t)arg);
}

static void conf_entry_flag(void *arg)
{
    (void)arg;
    s_flag_a = 1;
}

static void conf_entry_ab(void *arg)
{
    const char c = (char)(br_intptr_t)arg;

    for (int i = 0; i < 4; i++) {
        if (s_seq_n < (int)BR_ARRAY_SIZE(s_seq)) {
            s_seq[s_seq_n] = c;
            s_seq_n++;
        }
        br_log_info("[TASKCONF] RUN worker %c i=%d", c, i);
        br_task_yield();                /* coop 的唯一自愿让出点 */
    }
}

static void conf_entry_probe(void *arg)
{
    (void)arg;
    volatile br_u8 local[128];

    local[0] = 0x5Au;
    if (local[0] != 0x5Au) {
        s_probe_addr = 0u;
        return;
    }
    s_probe_addr = (br_uintptr_t)(const void *)&local[0];
}

static void conf_entry_sleep(void *arg)
{
    (void)arg;
    const br_time_t t0 = br_clock_now();

    s_sleep_rc = br_task_sleep(2000u);          /* 2 ms */
    s_sleep_delta = br_clock_now() - t0;
}

static void conf_entry_victim(void *arg)
{
    (void)arg;
    s_victim_ticks++;
    return;
}

static void conf_entry_busy(void *arg)
{
    (void)arg;

    /* 忙循环: 不 yield。coop 下这段时间里 victim **不该**被切上来。 */
    for (br_u32 i = 0u; i < BR_CONF_BUSY_ITERS; i++) {
        if (s_victim_ticks != 0u) {
            s_preempt_seen = 1;
        }
    }

    br_task_yield();                    /* 显式让出: 此刻 victim 才该跑 */

    if (s_victim_ticks != 0u) {
        s_victim_after_yield = 1;
    }
}

/* --- 子用例 ----------------------------------------------------------- */

static void case_task_001(void)
{
    br_task_attr_t a = conf_attr(0u, "t001");
    br_thread_t *t = BR_NULL;

    const int r = br_task_create(&t, &a, conf_entry_ok, BR_NULL);
    const br_bool ok = (r == 0) && (t != BR_NULL) && (br_task_state(t) == BR_TASK_READY) &&
                       (br_task_self() != BR_NULL);

    conf_result(ok, "TC-TASK-001", "create 后 state=READY(且 br_task_self() 非空)");
    if (t != BR_NULL) {
        (void)br_task_join(t, BR_NULL);
    }
}

static void case_task_002(void)
{
    br_task_attr_t a = conf_attr(1u, "t002");
    br_thread_t *t = BR_NULL;
    int code = -1;

    const int r = br_task_create(&t, &a, conf_entry_code, (void *)(br_intptr_t)42);
    const int j = (t != BR_NULL) ? br_task_join(t, &code) : BR_ERR(BR_EINVAL);

    conf_result((r == 0) && (j == 0) && (code == 42), "TC-TASK-002",
                "join 拿到退出码 42");
}

static void case_task_003_interleave(void)
{
    br_task_attr_t af = conf_attr(3u, "t003flag");
    br_task_attr_t a1 = conf_attr(2u, "workerA");
    br_task_attr_t a2 = conf_attr(3u, "workerB");
    br_thread_t *tf = BR_NULL;
    br_thread_t *ta = BR_NULL;
    br_thread_t *tb = BR_NULL;
    int code_a = -1;
    int code_b = -1;

    /*
     * (a) 直接判据: 本线程 yield 让出后, 另一个线程真的跑过(flag 被置上)。
     *     此刻就绪队只有 flag 线程, 所以 FIFO 一定先跑它。
     */
    s_flag_a = 0;
    const int rf = br_task_create(&tf, &af, conf_entry_flag, BR_NULL);
    br_task_yield();
    const br_bool ran = (s_flag_a == 1);
    const int jf = (tf != BR_NULL) ? br_task_join(tf, BR_NULL) : BR_ERR(BR_EINVAL);
    conf_result((rf == 0) && (jf == 0) && ran, "TC-TASK-003",
                "yield 让出后另一个线程真的跑过");

    /*
     * (b) 交错证据: 两个 worker 各自"追加字母 + yield" × 4。
     *     join(A) 让本线程阻塞 —— 调度器于是把 A/B 交替跑起来。
     */
    s_seq_n = 0;
    for (br_u32 i = 0u; i < BR_ARRAY_SIZE(s_seq); i++) {
        s_seq[i] = '.';
    }

    const int r1 = br_task_create(&ta, &a1, conf_entry_ab, (void *)(br_intptr_t)'A');
    const int r2 = br_task_create(&tb, &a2, conf_entry_ab, (void *)(br_intptr_t)'B');

    const int j1 = (ta != BR_NULL) ? br_task_join(ta, &code_a) : BR_ERR(BR_EINVAL);
    const int j2 = (tb != BR_NULL) ? br_task_join(tb, &code_b) : BR_ERR(BR_EINVAL);

    const char *p = (const char *)s_seq;
    const br_bool seq_ok = (s_seq_n == 8) &&
                           (p[0] == 'A') && (p[1] == 'B') && (p[2] == 'A') && (p[3] == 'B') &&
                           (p[4] == 'A') && (p[5] == 'B') && (p[6] == 'A') && (p[7] == 'B');

    br_log_info("[TASKCONF] TRACE coop interleave seq=%c%c%c%c%c%c%c%c",
                p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

    conf_result((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) && (code_a == 0) &&
                (code_b == 0) && seq_ok,
                "TC-TASK-003", "两线程严格交替(ABABABAB): 强制切换点真的在切");
}

static void case_task_004(void)
{
    br_task_attr_t a = conf_attr(4u, "t004");
    br_thread_t *t = BR_NULL;
    int code = -1;

    const int r = br_task_create(&t, &a, conf_entry_ok, BR_NULL);
    br_task_yield();                    /* 让它跑完(entry 返回 ⇒ trampoline ⇒ exit) */

    const br_bool zombie = (t != BR_NULL) && (br_task_state(t) == BR_TASK_ZOMBIE);
    const int j = (t != BR_NULL) ? br_task_join(t, &code) : BR_ERR(BR_EINVAL);

    conf_result((r == 0) && zombie && (j == 0) && (code == 0), "TC-TASK-004",
                "exit 后 state=ZOMBIE 且 join 回收");
}

static void case_task_005(void)
{
    br_task_attr_t a = conf_attr(5u, "t005");
    br_task_attr_t bad = conf_attr(5u, "bad");
    br_thread_t *x = BR_NULL;
    int r[7];

    r[0] = br_task_create(BR_NULL, &a, conf_entry_ok, BR_NULL);
    r[1] = br_task_create(&x, BR_NULL, conf_entry_ok, BR_NULL);

    bad.stack = BR_NULL;
    r[2] = br_task_create(&x, &bad, conf_entry_ok, BR_NULL);

    bad = conf_attr(5u, "bad");
    bad.stack_size = 128u;              /* < BR_STACK_MIN */
    r[3] = br_task_create(&x, &bad, conf_entry_ok, BR_NULL);

    r[4] = br_task_create(&x, &a, BR_NULL, BR_NULL);
    r[5] = br_task_join(BR_NULL, BR_NULL);
    r[6] = br_task_join(br_task_self(), BR_NULL);

    br_bool all = BR_TRUE;
    for (br_u32 i = 0u; i < BR_ARRAY_SIZE(r); i++) {
        if (r[i] != BR_ERR(BR_EINVAL)) {
            all = BR_FALSE;
        }
    }
    conf_result(all && (x == BR_NULL), "TC-TASK-005",
                "非法入参 -EINVAL(create NULL/attr/栈/entry + join NULL/自身)");
}

static void case_task_006(void)
{
    br_thread_t *ts[BR_TASK_MAX + 2u];
    br_u32 n = 0u;
    int last = 0;

    for (br_u32 i = 0u; i < (BR_TASK_MAX + 2u); i++) {
        br_task_attr_t ai = conf_attr(i, "pool");
        const int r = br_task_create(&ts[n], &ai, conf_entry_ok, BR_NULL);
        if (r == 0) {
            n++;
        } else {
            last = r;
            break;
        }
    }

    conf_result(last == BR_ERR(BR_ENOMEM), "TC-TASK-006",
                "TCB 池满 ⇒ -ENOMEM(池上限是硬上界)");

    for (br_u32 i = 0u; i < n; i++) {
        (void)br_task_join(ts[i], BR_NULL);
    }
}

static void case_task_007(void)
{
    br_task_attr_t a = conf_attr(7u, "t007");
    br_thread_t *t = BR_NULL;

    s_probe_addr = 0u;
    const int r = br_task_create(&t, &a, conf_entry_probe, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(BR_EINVAL);

    const br_uintptr_t top = (br_uintptr_t)a.stack;
    const br_uintptr_t lo  = top - (br_uintptr_t)a.stack_size;
    const br_bool in_range = (s_probe_addr >= lo) && (s_probe_addr < top);

    conf_result((r == 0) && (j == 0) && in_range, "TC-TASK-007",
                "局部变量地址落在该线程自己的栈区间内(栈真的用上了)");
}

static void case_time_001(void)
{
    br_task_attr_t a = conf_attr(0u, "tsleep");
    br_thread_t *t = BR_NULL;

    s_sleep_delta = 0u;
    s_sleep_rc = -1;
    const int r = br_task_create(&t, &a, conf_entry_sleep, BR_NULL);
    const int j = (t != BR_NULL) ? br_task_join(t, BR_NULL) : BR_ERR(BR_EINVAL);

    conf_result((r == 0) && (j == 0) && (s_sleep_rc == 0) && (s_sleep_delta >= 2000u),
                "TC-TIME-001", "sleep(2000us) 不早醒(实测 >= 请求值)");
}

static void case_time_002(void)
{
    const br_time_t t0 = br_clock_now();
    const int r = br_task_sleep(0u);
    const br_time_t d = br_clock_now() - t0;

    /*
     * 判据用"实测耗时 < 1 ms"而不是 `d == 0`: 目标上 timer ISR 可能在两次读数之间
     * 落下(它是抢占的), 于是 d 会是几个 us —— 那与"立即返回"并不矛盾。
     * 真正的反例是"被登记进超时表并切走", 那至少要等一个 tick(100 ms)。
     */
    br_log_info("[TASKCONF] TRACE sleep0 rc=%d elapsed_us=%lu", r, (br_u64)d);
    conf_result((r == 0) && (d < BR_US_PER_MS), "TC-TIME-002",
                "sleep(0) 立即返回 0(实测 < 1ms, 未进超时表)");
}

static void case_time_003(void)
{
    const int r = br_task_sleep(BR_TIMEOUT_INF);

    conf_result(r == BR_ERR(BR_EINVAL), "TC-TIME-003", "sleep(INF) ⇒ -EINVAL(裁定 G13)");
}

static void case_task_101(void)
{
    br_task_attr_t ab = { .name = "busy",
                          .stack = (void *)(s_busy_stack + BR_CONF_STACK_BYTES),
                          .stack_size = BR_CONF_STACK_BYTES,
                          .prio = 0u, .flags = 0u };
    br_task_attr_t av = { .name = "victim",
                          .stack = (void *)(s_victim_stack + BR_CONF_STACK_BYTES),
                          .stack_size = BR_CONF_STACK_BYTES,
                          .prio = 0u, .flags = 0u };
    br_thread_t *tb = BR_NULL;
    br_thread_t *tv = BR_NULL;

    s_victim_ticks = 0u;
    s_preempt_seen = 0;
    s_victim_after_yield = 0;

    /* 先建 busy 再建 victim ⇒ 就绪队序 [busy, victim]; join(busy) 后 busy 先跑。 */
    const int r1 = br_task_create(&tb, &ab, conf_entry_busy, BR_NULL);
    const int r2 = br_task_create(&tv, &av, conf_entry_victim, BR_NULL);
    const int j1 = (tb != BR_NULL) ? br_task_join(tb, BR_NULL) : BR_ERR(BR_EINVAL);
    const int j2 = (tv != BR_NULL) ? br_task_join(tv, BR_NULL) : BR_ERR(BR_EINVAL);

    /*
     * coop 与 preempt 的**机械分界**: busy 不 yield 期间 victim 一次都没跑;
     * busy 主动 yield 之后 victim 才跑。若哪天把 coop 换成 preempt, 这条会红 ——
     * 那正是"无抢占下忙循环不被切是设计内行为, 不是 bug"的判据。
     */
    conf_result((r1 == 0) && (r2 == 0) && (j1 == 0) && (j2 == 0) &&
                (s_preempt_seen == 0) && (s_victim_after_yield == 1) &&
                (s_victim_ticks == 1u),
                "TC-TASK-101", "coop: 不 yield 的忙线程不被抢占, yield 后 victim 才跑");
}

void br_sched_conformance(void)
{
    s_conf_pass = 0;
    s_conf_fail = 0;

    br_log_info("[TASKCONF] ==== sched framework + coop conformance (3-03 / 6-01 §3.1/§3.3)");
    br_log_info("[TASKCONF] ---- scheduler=%s kind=%u self=%s spawn=%u",
                (s_ops != BR_NULL) ? s_ops->name : "?",
                (s_ops != BR_NULL) ? s_ops->kind : 0u,
                br_task_name(br_task_self()),
                (br_u32)(s_ops != BR_NULL ? s_ops->tcb_size : 0u));

    case_task_001();
    case_task_002();
    case_task_003_interleave();
    case_task_004();
    case_task_005();
    case_task_006();
    case_task_007();
    case_time_001();
    case_time_002();
    case_time_003();
    case_task_101();

    br_log_info("[TASKCONF] SUMMARY pass=%d fail=%d total=%d",
                s_conf_pass, s_conf_fail, s_conf_pass + s_conf_fail);

    if (s_conf_fail != 0) {
        br_log_error("[TASKCONF] 有失败项 —— 调度框架/coop 未达 conformance");
    }
}
