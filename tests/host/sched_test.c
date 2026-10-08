/*
 * tests/host/sched_test.c — 宿主侧调度框架 / coop 语义门禁(不需要 QEMU)
 *
 * 为什么在宿主跑: 就绪队列序 / block↔wake 状态机 / 超时表到期 / 池上限这些是**策略性质**,
 * 宿主上可以毫秒级反复跑; 同一批性质在 QEMU 上只能靠几条用例撞。宿主用 `ucontext.h`
 * 做**真的换栈**(不是"假切换"), 于是 `br_task_create`/`yield`/`block`/`join`/`sleep`
 * 全部走真实路径 —— 与目标上 `switch.S` 的语义一一对应。
 *
 * 层次切分(任务书要求): 切换层 = `core/src/sched/switch.S`(**只在 aarch64 编**);
 * 策略层 = `core/src/sched/sched_core.c` + `sched/coop/src/coop.c`(**两边都编**)。
 * 本文件提供宿主版的 `br_sched_switch_to()`(ucontext)以及目标侧的桩
 * (时钟 / L2 临界区 / 日志 / panic)。
 *
 * 目标上被 sched_core.c 引用、宿主必须自备的符号:
 *   br_clock_now / br_deadline_from_now / br_irq_lock / br_irq_unlock /
 *   br_log_write / br_panic / br_sched_switch_to / br_sched_thread_trampoline
 * 外加一个**宿主专用**的 idle 钩子 `br_sched_host_wait()`(sched_core.c 里是弱符号):
 * 宿主没有真中断源, 由它在 core 进 idle 时推进假时钟并投递一次 `br_sched_on_tick()`,
 * 于是"周期 tick 上的超时扫描"在宿主上可测。
 *
 * 输出协议(照 tests/host/mem_test.c):
 *   [HOSTTEST] PASS|FAIL <tag> <desc>
 *   [HOSTTEST] SUMMARY pass=N fail=M total=T
 * 有失败(含 br_sched_conformance 打出的任何 `[TASKCONF] FAIL`)⇒ 退出码非 0。
 */
#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

#include <br/sched/coop.h>

#include "sched_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/* =====================================================================
 * 假时钟 + idle 钩子
 * ===================================================================== */

static br_time_t s_fake_now;
static br_u32    s_idle_calls;

br_time_t br_clock_now(void)
{
    return s_fake_now;
}

br_time_t br_deadline_from_now(br_time_t rel_us)
{
    if (rel_us > (BR_TIMEOUT_INF - s_fake_now)) {
        return BR_TIMEOUT_INF;          /* 饱和, 不回绕成"立刻超时" */
    }
    return s_fake_now + rel_us;
}

/* 宿主: 单线程 + 无真中断 ⇒ L2 临界区是空动作(与目标上"真的关 DAIF"等价地保护了竞态面)。 */
br_irq_state_t br_irq_lock(void) { return 0u; }
void br_irq_unlock(br_irq_state_t st) { (void)st; }

/* core 进 idle 时会调它(弱符号在 sched_core.c): 每次推进 100 us 并投递一次 tick。 */
void br_sched_host_wait(void);
void br_sched_host_wait(void)
{
    s_idle_calls++;
    s_fake_now += 100u;
    br_sched_on_tick(s_fake_now);
}

/* =====================================================================
 * 日志 / panic 桩
 * ===================================================================== */

static int s_conf_fail_seen;

void br_log_write(br_log_level_t level, const char *fmt, ...)
{
    static const char *const tag[BR_LOG_LEVEL_COUNT] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
    char    buf[512];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    const char *t = ((unsigned)level < (unsigned)BR_LOG_LEVEL_COUNT) ? tag[level] : "?";
    printf("[HOST %s] %s\n", t, buf);

    /* 宿主门禁靠退出码判定; br_sched_conformance 的红绿也由这里汇总 */
    if (strstr(buf, "[TASKCONF] FAIL") != NULL) {
        s_conf_fail_seen++;
    }
}

BR_NORETURN void br_panic(const char *fmt, ...)
{
    va_list ap;

    fputs("[HOSTPANIC] ", stderr);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(3);
}

/* =====================================================================
 * 宿主版切换层(ucontext)
 * ===================================================================== */

#define HOST_SLOT_MAX 32u

/*
 * TCB 池会**复用地址**: 一个线程被 join 回收后, 下一个 create 很可能拿到同一个
 * `struct br_thread *`。若宿主按指针缓存 ucontext, 就会把"新线程"接到"上一任的
 * 旧现场"上(表现为随机乱跑/挂死)。这里借 ctx[] 里 core 在宿主路径上**不用**的
 * 槽位(x20)当"现场已物化"标记: 新建/复用的 TCB 都是 0(create 时整槽清零),
 * 物化一次后写成魔数 ⇒ 复用时会被正确地**重建**。
 */
#define HOST_CTX_READY_MAGIC ((br_u64)0x484F5354u)   /* "HOST" */

typedef struct host_slot {
    br_thread_t *t;
    ucontext_t   uc;
} host_slot_t;

static host_slot_t s_slots[HOST_SLOT_MAX];

static host_slot_t *slot_of(br_thread_t *t) __attribute__((noinline));

static host_slot_t *slot_of(br_thread_t *t)
{
    host_slot_t *free_slot = NULL;

    for (br_u32 i = 0u; i < HOST_SLOT_MAX; i++) {
        if (s_slots[i].t == t) {
            return &s_slots[i];
        }
        if (s_slots[i].t == BR_NULL && free_slot == BR_NULL) {
            free_slot = &s_slots[i];
        }
    }
    if (free_slot == BR_NULL) {
        fputs("[HOSTPANIC] ucontext slot 池满\n", stderr);
        exit(3);
    }
    free_slot->t = t;
    return free_slot;
}

/* 新线程在 ucontext 上的入口。core 在换栈前已把 current 置成本线程, 所以这里能取到自己。 */
static void host_thread_tramp(void)
{
    br_sched_thread_entry(br_task_self());
}

void br_sched_switch_to(br_thread_t *from, br_thread_t *to)
{
    host_slot_t *sf = slot_of(from);
    host_slot_t *st = slot_of(to);

    /*
     * 首次进入的条件: 该 TCB 有栈(排除 main —— 它的现场由第一次 swapcontext 保存),
     * 且"现场已物化"标记还没写。复用 TCB 时标记是 0 ⇒ 重建。
     */
    if ((to->stack_top != BR_NULL) && (to->ctx[1] != HOST_CTX_READY_MAGIC)) {
        getcontext(&st->uc);
        st->uc.uc_stack.ss_sp   = (void *)((br_u8 *)to->stack_top - to->stack_size);
        st->uc.uc_stack.ss_size = (size_t)to->stack_size;
        st->uc.uc_link          = NULL;
        makecontext(&st->uc, host_thread_tramp, 0);
        to->ctx[1] = HOST_CTX_READY_MAGIC;
    }

    (void)swapcontext(&sf->uc, &st->uc);
}

/* 宿主不需要真的 trampoline(入口是 host_thread_tramp); 仅为链接提供该符号。 */
void br_sched_thread_trampoline(void)
{
    fputs("[HOSTPANIC] 宿主的 br_sched_thread_trampoline 不该被调用\n", stderr);
    abort();
}

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
static br_u8 s_sup_stack[HOST_STACK_BYTES] BR_ALIGN(16);

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
    s_to_rc = br_sched_block_current(s_fake_now + 5000u);
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

    /* core 的一致性用例(它自身跑在真实线程里, 真的 create/join/yield/sleep) */
    br_sched_conformance();

    host_check(s_idle_calls > 0u, "HOST-SCHED-IDLE",
               "pick_next 返回 NULL ⇒ core 进 idle(宿主由钩子代 WFI)并被 tick 唤醒");
    host_check(br_sched_task_count() == 2u, "HOST-SCHED-POOL",
               "测试线程全部被 join 回收(只剩 main + supervisor)");
    host_check(s_conf_fail_seen == 0, "HOST-SCHED-CONF",
               "br_sched_conformance 无 [TASKCONF] FAIL");
    host_check((br_task_name(br_task_self()) != NULL) &&
               (strcmp(br_task_name(br_task_self()), "supervisor") == 0),
               "HOST-SCHED-NAME", "br_task_name/br_task_self 在真实线程里正确");

    printf("[HOSTTEST] SUMMARY pass=%d fail=%d total=%d (idle_calls=%u)\n",
           s_hpass, s_hfail, s_hpass + s_hfail, (unsigned)s_idle_calls);

    exit((s_hfail == 0) ? 0 : 1);
}

/* =====================================================================
 * main
 * ===================================================================== */

int main(void)
{
    printf("[HOSTTEST] brickOS prototype — 宿主侧调度框架 / coop 语义门禁\n");

    const int r = coop_early_init();
    if (r != 0) {
        printf("[HOSTTEST] FAIL coop_early_init rc=%d\n", r);
        return 1;
    }
    if (br_sched_registered() != BR_TRUE) {
        printf("[HOSTTEST] FAIL 调度器未注册\n");
        return 1;
    }

    /* I1: register 之后, 调用者所在的启动上下文就是 main 线程 */
    const char *mn = br_task_name(br_task_self());
    if (mn == BR_NULL || strcmp(mn, "main") != 0) {
        printf("[HOSTTEST] FAIL 启动上下文未物质化为 main(得到 '%s')\n",
               (mn != BR_NULL) ? mn : "(null)");
        return 1;
    }
    printf("[HOSTTEST] register 后 br_task_self()=%s (I1 成立)\n", mn);

    br_task_attr_t a = {
        .name       = "supervisor",
        .stack      = (void *)(s_sup_stack + HOST_STACK_BYTES),
        .stack_size = HOST_STACK_BYTES,
        .prio       = 0u,
        .flags      = 0u,
    };
    br_thread_t *sup = BR_NULL;
    const int rc = br_task_create(&sup, &a, host_supervisor, BR_NULL);
    if (rc != 0) {
        printf("[HOSTTEST] FAIL create supervisor rc=%d\n", rc);
        return 1;
    }

    printf("[HOSTTEST] 进入 br_sched_run()(current=main, 就绪=supervisor)\n");
    br_sched_run();                     /* 不返回: supervisor 里 exit() */
}
