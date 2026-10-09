/*
 * brickOS prototype v0.2.0 — 调度框架**内部**契约(core 私有, 不给插件)
 *
 * 这个头只给 `core/src/sched/` 下的 .c 用。它冻结两件跨文件的事:
 *   1. TCB 的**公共头**布局(`br_thread_t` 的真身) —— 调度插件只拿不透明指针, 但
 *      `tcb_size` 要覆盖"公共头 + 插件私有尾";
 *   2. 等待链的节点与操作(`br_waitq_*`) —— 同步原语(`br_sync.h` 的实现)零分配地
 *      挂/摘等待者, 节点就嵌在 TCB 里。
 *
 * 为什么把 TCB 布局写死: 上下文切换汇编(`switch.S`)要按固定偏移存取, 而同步原语要
 * 直接改 `state`/`wake_at`/`wait_next`。三处若各自算偏移, 就有了三处真值。
 */
#ifndef BR_SCHED_INTERNAL_H
#define BR_SCHED_INTERNAL_H

#include <br/core/br_sched.h>
#include <br/core/br_types.h>

/* TCB 魔法字(认"这是不是一个 TCB"; 观测/自检用)。 */
#define BR_THREAD_MAGIC 0x54484221u /* "THB!" */

/*
 * 上下文保存区。COOP 切换 = 保存 callee-saved + 栈指针:
 *   [0..9]  x19..x28
 *   [10]    x29(fp)   —— `-fno-omit-frame-pointer` 是 backtrace 的前提, 也是它的前提
 *   [11]    x30(lr)   —— 新线程首次进入时指向 trampoline
 *   [12]    sp        —— 线程自己的栈
 */
#define BR_THREAD_CTX_WORDS 13u

struct br_thread {
    br_u32       magic;
    br_u32       state;         /* BR_TASK_* */
    br_u8        prio;
    const char  *name;
    void        *stack_top;
    br_size_t    stack_size;
    void       (*entry)(void *);
    void        *arg;
    int          exit_code;
    br_time_t    wake_at;       /* BR_TIMEOUT_INF = 无期限 */
    /*
     * ★ **ICB 唯一握手位**(ADR-0006 §3b): 全 0 = "已被唤醒", 非 0(约定 -ETIMEDOUT)
     * = "尚未被唤醒"。三个写点: 调用方在开窗前置 -ETIMEDOUT; `br_sched_wake` 置 0;
     * 超时扫描置 -ETIMEDOUT。`br_sched_block_current` 入口只在它为 0 时直接返回 0
     * (窗口里的早唤醒), 否则才置 BLOCKED。读点: 阻塞返回时把值当返回值。
     * 它同时是"unlock 的所有权交接"与"超时"打架时的唯一赢家。
     */
    int          wait_status;
    struct br_thread *wait_next;/* 等待链: mutex/sem/cond/join 共用 */
    struct br_thread *all_next; /* 全局 TCB 链(遍历/观测) */
    br_u64       ctx[BR_THREAD_CTX_WORDS];
    /* 插件私有尾 **从这里开始**(大小 = ops.tcb_size - sizeof(struct br_thread))。
     * core 不碰它; 插件用 `br_thread_priv(t)` 取(声明在 `br_sched.h` —— 它是插件
     * 可见面; `br_sched_tcb_size()` 与它配对 = `ops.tcb_size` 的两半)。 */
};

/* ------------------------------------------------------------------ 切换层
 * `switch.S` 提供(目标上); 宿主用例提供同签名的假实现(ucontext, 见 tests/host)。 */

/* 保存 from 的 callee-saved + sp, 恢复 to 的, 换栈。ctx 布局见 `sched_ctx.h`。 */
void br_sched_switch_to(struct br_thread *from, struct br_thread *to);

/* 新线程的首次入口(create 时写进 ctx[x30]); x19 里是 TCB 指针。 */
BR_NORETURN void br_sched_thread_trampoline(void);

/* trampoline 的 C 半边: 调 entry(arg), 返回则 br_task_exit(0)(不返回)。 */
BR_NORETURN void br_sched_thread_entry(struct br_thread *t);

/* ==================================================================== 等待链 */
/*
 * 等待链是**单链表 + 节点在线程里**: 入队把 t 挂到尾, 出队摘头。
 * 零分配是本设计的关键性质(同步路径不能分配 —— 分配可能失败, 锁不能失败)。
 */
void          br_waitq_push(void **head, br_thread_t *t);
br_thread_t  *br_waitq_pop(void **head);
int           br_waitq_remove(void **head, br_thread_t *t); /* 1 = 摘到了, 0 = 不在链上 */
br_u32        br_waitq_len(const void *head);

/* TCB 池(静态上界; 观测用)。 */
#ifndef BR_TASK_MAX
#define BR_TASK_MAX 8u
#endif
br_u32 br_thread_pool_used(void);

/*
 * 已注册的调度器 ops(sched_core.c 的真值; 未注册 ⇒ BR_NULL)。
 * 为什么在这里开这个口子: 自检套件(`core/selftest/sched_selftest.c`)要打
 * `scheduler=%s kind=%u spawn=%u` 这条证据行 —— name/kind/tcb_size 只存在于注册表里。
 * 让它读生产侧的**同一份**真值, 胜过在套件里复制一份插件表(那会有两处真值)。
 * 只读: 本函数不给调用者任何写入通道。
 */
const br_sched_ops_t *br_sched_ops_get(void);

#endif /* BR_SCHED_INTERNAL_H */
