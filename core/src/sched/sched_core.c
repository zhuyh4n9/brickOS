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
 *   I4  **切换只发生在下列显式点**: yield / block / exit / 首次调度, **加上**——
 *       ADR-0011 之后 —— **IRQ 出口的抢占**(仅当调度器置了 need_resched)。
 *       COOP 从不置位 ⇒ 对 coop 而言 I4 的旧措辞(无抢占、IRQ 出口不切栈)逐字成立。
 *       IRQ 出口的切换点在 C 调用边界(`br_sched_switch_to`), 被抢占线程的异常帧
 *       完整地留在它自己的栈上 —— 见 `resched_from_irq()` 的长注释。
 *   I5  ICB 握手(ADR-0006 §3b, 与 F3 的 sync.c 对齐): `wait_status` 是唯一握手位 ——
 *       调用方在开窗前置 -ETIMEDOUT, wake 置 0, 超时置 -ETIMEDOUT;
 *       谁先写谁赢, 两种顺序收敛到同一结果(见 `br_sched_block_current`)。
 *   I6  **bh 不是线程上下文**(ADR-0011): 下半部跑在关中断的异常出口上, 在它里面
 *       阻塞/让出/退出都会让异常帧的返回路径丢失 ⇒ 三条路径都在运行期拒绝
 *       (`br_sched_block_current` = -EPERM, `br_task_yield` = 拒绝 + 留痕,
 *        `br_task_exit` = panic)。
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_trace.h>
#include <br/core/br_types.h>
#include <br/core/br_work.h>

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
 * 抢占请求位(ADR-0011)。写点 = 调度插件的 tick 策略(`br_sched_request_resched`,
 * ISR 内); 消费点 = IRQ 出口的 `resched_from_irq()`。**只有这一个位**
 * (`volatile` 是给"ISR 写 / 出口读"这对上下文看的; 单核上它也不需要原子指令)。
 */
static volatile br_bool s_need_resched;

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

/*
 * 注册表**只读**观测(`sched_internal.h`)。为什么需要它: 自检套件要打
 * "注册进来的是哪个调度器"这条证据行(name/kind/tcb_size), 而那些字段只在这里 ——
 * 与其让套件复制一份插件表(于是"注册了什么"有两处真值), 不如开一个只读口子。
 * 只读: 调用者拿不到写入通道(改注册只能走 br_sched_register 的"恰一次")。
 */
const br_sched_ops_t *br_sched_ops_get(void)
{
    return s_ops;
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

/* IRQ 出口的抢占换栈(定义在本节末; 前置条件与安全性论证在定义处)。 */
static void resched_from_irq(void);

void br_sched_irq_epilogue(void)
{
    if (s_ops == BR_NULL) {
        return;
    }

    /*
     * 到期扫描(与 on_tick 同一处方): 覆盖"非 timer 中断"的出口, 使超时唤醒不依赖
     * 具体是哪一个 IRQ 把 CPU 从 idle 里叫醒的。
     * ★ 必须在抢占之前: 超时唤醒的线程要能被同一次出口的 pick_next 选中。
     */
    scan_timeouts(br_clock_now());

    resched_from_irq();
}

/* ------------------------------------------------------------------ 抢占 */

void br_sched_request_resched(void)
{
    s_need_resched = BR_TRUE;
}

br_bool br_sched_resched_pending(void)
{
    return s_need_resched;
}

/*
 * IRQ 出口的抢占换栈(ADR-0011; 由 `br_sched_irq_epilogue` 调)。
 *
 * 前置(由调用点保证, 不是猜测): 在**异常上下文**里、已经 eoi、`irq_depth == 0`
 * (INV-C 刚查过)、`PSTATE.I == 1`。
 *
 * ★ 为什么这样切是安全的(这是本机制唯一难懂的一处, 必须写死):
 *   `from` 的现场被 `br_sched_switch_to` 保存为"callee-saved + sp" —— 而 sp 指向
 *   **`br_irq_enter` 的栈帧**(异常帧在它下面)。于是被抢占的线程此后停在"IRQ 出口
 *   的 C 代码里", 等它被再次选中时从 `br_sched_switch_to` 返回, 依次走完
 *   `resched_from_irq` → `br_sched_irq_epilogue` → `br_irq_enter` → 汇编桩的
 *   "恢复现场 + ERET"。⇒ **它自己的 ELR/SPSR 寄存器值一直躺在它自己的栈上的异常帧里**,
 *   不需要在切换时保存到别处(单核上这些寄存器是全局的, 但它们只在"回到自己的桩"
 *   那一刻才被读出来)。
 *   ⇒ 因此 `switch.S` 只存 callee-saved 依然够, vectors.S 也不必改 —— 切栈点根本不在
 *   那几行汇编里(与 vectors.S 文件头 P-IRQ-ASM-2 的"v2 注意义务"一致: 那条义务针对的是
 *   "在 ERET 前用汇编切栈"的另一种实现)。
 *
 * ★ 新线程的首次进入另有一处必须补: 此刻 `PSTATE.I == 1`(还在异常处理里), 而新线程
 *   的入口不经 ERET ⇒ 必须显式修正为"中断放开"。见 `br_sched_thread_entry`(裁定 P-1)。
 */
static void resched_from_irq(void)
{
    if (s_need_resched == BR_FALSE) {
        return;
    }
    s_need_resched = BR_FALSE;

    br_thread_t *cur = s_current;

    /* 只在"当前线程还在跑"时才有抢占可言(block/exit 路径已把 state 改掉)。 */
    if (s_ops == BR_NULL || cur == BR_NULL || cur->state != BR_TASK_RUNNING) {
        return;
    }

    /*
     * 把被抢占者放回就绪结构(队尾)。**这是 round-robin 的语义落点**:
     * 时间片用尽 ⇒ 排到队尾等下一轮; `state` 仍由 core 改(I2)。
     */
    cur->state = BR_TASK_READY;
    if (s_ops->thread_ready != BR_NULL) {
        (void)s_ops->thread_ready(cur);
    }

    br_thread_t *to = (s_ops->pick_next != BR_NULL) ? s_ops->pick_next() : BR_NULL;

    if (to == BR_NULL) {
        /* 防御(插件坏了才会到): 刚把 cur 放进队列, pick_next 却说没人 —— 把 cur 摘出来
         * 继续跑。绝不能"什么都不做": 那会让 cur 留在队列里却处于 RUNNING(违反 I2)。 */
        if (s_ops->thread_block != BR_NULL) {
            s_ops->thread_block(cur);
        }
        cur->state = BR_TASK_RUNNING;
        br_trace_emit(BR_TRACE_SCHED_PREEMPT, 0u, 0u);
        return;
    }
    if (to == cur) {
        /* 只有自己可跑(pick_next 已把它摘出队列): 不切栈, 直接继续(与 switch_next 同构)。 */
        cur->state = BR_TASK_RUNNING;
        br_trace_emit(BR_TRACE_SCHED_PREEMPT, 0u, 0u);
        return;
    }

    to->state = BR_TASK_RUNNING;
    s_current = to;
    br_trace_emit(BR_TRACE_SCHED_PREEMPT, 0u, 1u);   /* 证据: IRQ 出口真的换了栈 */
    br_sched_switch_to(cur, to);        /* 回来 = 有人又把 cur 选出来了 */
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
    /*
     * ★ bh 禁令(I6, ADR-0011): 下半部在关中断的异常出口上跑, 它**没有**可回退的调度
     *   上下文 —— 若在这里阻塞, 异常帧永远不会返回(且 ISR 的 eoi 已完成, 表现为
     *   "系统静默失去一条线")。运行期拒绝, 返回 -EPERM 而不是 -EINVAL:
     *   参数没错, 上下文错了(调用方要能区分这两种 bug)。
     */
    if (br_work_in_bh() != BR_FALSE) {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 2u, 0u);
        return BR_ERR(BR_EPERM);
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
    /*
     * ★ 裁定 P-1(ADR-0011): 新线程的首次进入必须把 L2 形态摆正。
     *
     * 问题: preempt 类调度器可能在 **IRQ 出口**选中一个"从没跑过"的线程
     * (`resched_from_irq` → `br_sched_switch_to` → trampoline)。那一刻 `PSTATE.I == 1`
     * (我们仍在异常处理里), 而新线程**不经 ERET** 进入 ⇒ 若不管, 它会带着关中断一路
     * 跑下去: timer 再也不来 ⇒ 抢占与超时同时失效(而且没有任何报错)。
     *
     * 处置: `irq_depth == 0` ⇒ 线程的应然形态是"中断放开", 显式放行。
     *   `irq_depth > 0` ⇒ **不动**(并且这不是"顺手修好"): 那意味着有人在 L2 临界区里
     *   让出了 CPU —— 那是既有禁令(与 INV-C 同源), 此刻打开中断会把临界区撕开。
     *   留痕而不改状态: 诚实失败优于静默地把中断打开。
     */
    if (br_irq_lock_depth() == 0u) {
        br_irq_cpu_enable();
    } else {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 1u, (br_u64)br_irq_lock_depth());
    }

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

    /*
     * ★ bh 禁令(I6, ADR-0011): 从下半部结束一个线程没有任何正确的收尾方式 ——
     *   它的异常帧还压在栈上, "切走"会让 ERET 永远不回来。设计的一贯口径是
     *   "诚实失败优于静默降级" ⇒ panic(而不是假装退出成功)。
     */
    if (br_work_in_bh() != BR_FALSE) {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 3u, 0u);
        br_panic("[sched] br_task_exit(): 不许在中断下半部(bh)里结束线程(ADR-0011 §4)");
    }

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
    /*
     * ★ bh 禁令(I6, ADR-0011): 下半部里让出 CPU == 把异常出口的返回路径丢掉。
     *   `br_task_yield` 是 void ⇒ 无法返回错误码, 故**拒绝执行 + 留痕**(不静默地
     *   当成"让出成功": 调用者以为切换发生过, 而实际什么都没发生 —— 那也是一种谎)。
     */
    if (br_work_in_bh() != BR_FALSE) {
        br_trace_emit(BR_TRACE_BH_CTX_VIOLATION, 4u, 0u);
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

