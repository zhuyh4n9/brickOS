/*
 * tests/host/host_sched.c — 宿主侧调度测试装置(实现; 接口与理由见 host_sched.h)
 *
 * 目标上被 core 引用、宿主必须自备的符号:
 *   br_clock_now / br_deadline_from_now / br_irq_lock / br_irq_unlock /
 *   br_irq_lock_depth / br_irq_in_isr / br_irq_in_atomic / br_irq_cpu_enable /
 *   br_log_write / br_panic / br_sched_switch_to / br_sched_thread_trampoline
 * 外加一个**宿主专用**的 idle 钩子 `br_sched_host_wait()`(sched_core.c 里是弱符号):
 * 宿主没有真中断源, 由它在 core 进 idle 时推进假时钟并投递一次 `br_sched_on_tick()`,
 * 于是"周期 tick 上的超时扫描"在宿主上可测。
 *
 * ucontext 槽位与"现场已物化"标记的手法与之前逐字相同(见文件内注释): TCB 池会复用地址,
 * 按指针缓存 ucontext 会把新线程接到上一任的旧现场上。
 */
#include "host_sched.h"

#include <br/core/br_error.h>
#include <br/core/br_fault.h>
#include <br/core/br_irq.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_work.h>

/* TCB 公共头与切换层的内部原型(core 私有面; 宿主是"同一份语义的第二条腿",
 * 包含方式与 tests/host/sched_test.c 原先的做法一致)。 */
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

/* 自动 tick 源(见 host_sched.h): 默认关 ⇒ 既有用例的时钟语义逐字不变。 */
#define HOST_CLOCK_STEP_US 50u       /* 每次读表推进的"真实时间"(要远小于 1 ms: TC-TIME-002 的判据是"两次读表 < 1 ms") */
#define HOST_TICK_US       100000u   /* 宿主用例自己的虚拟 tick; 与产品 [kernel].hz 解耦(判据在 tick 计数上) */

static br_bool   s_auto_tick;
static br_time_t s_since_tick;
static int       s_in_clock;         /* 防重入: 投递 tick 的路径里还会读时钟 */

br_time_t host_sched_now(void) { return s_fake_now; }
void      host_sched_advance_us(br_time_t us) { s_fake_now += us; }
br_u32    host_sched_idle_calls(void) { return s_idle_calls; }

void host_sched_tick(void) { br_sched_on_tick(s_fake_now); }

void host_sched_auto_tick(br_bool on)
{
    s_auto_tick = on;
    s_since_tick = 0u;
}

br_time_t br_clock_now(void)
{
    /*
     * ★ 自动 tick(默认关): 见 host_sched.h 的长注释。三个条件缺一不可 ——
     *   开关打开、不在投递中(防重入)、且**不在 L2 临界区**(= 目标上中断被屏蔽)。
     *   L2 里时间照走但 tick 不投递: 累计量会在放开后的第一次读表时补上一次 tick。
     */
    if ((s_auto_tick != BR_FALSE) && (s_in_clock == 0)) {
        s_in_clock = 1;
        s_fake_now   += HOST_CLOCK_STEP_US;
        s_since_tick += HOST_CLOCK_STEP_US;

        if ((s_since_tick >= HOST_TICK_US) && (br_irq_lock_depth() == 0u)) {
            s_since_tick = 0u;
            br_sched_on_tick(s_fake_now);
            br_sched_irq_epilogue();
        }

        s_in_clock = 0;
    }
    return s_fake_now;
}

br_time_t br_deadline_from_now(br_time_t rel_us)
{
    if (rel_us > (BR_TIMEOUT_INF - s_fake_now)) {
        return BR_TIMEOUT_INF;          /* 饱和, 不回绕成"立刻超时" */
    }
    return s_fake_now + rel_us;
}

/* core 进 idle 时会调它(弱符号在 sched_core.c): 每次推进 100 us 并投递一次 tick。 */
void br_sched_host_wait(void);
void br_sched_host_wait(void)
{
    s_idle_calls++;
    s_fake_now += 100u;
    br_sched_on_tick(s_fake_now);
}

/* =====================================================================
 * L2 中断锁(宿主替身)
 *
 * 宿主没有可屏蔽的中断; 但调度是"只在显式点换栈"或"只在 IRQ 出口换栈"的, 所以
 * "L2 临界区里不发生抢占"这条在宿主上同样可由 depth 表达。**depth 必须是真的**:
 * ADR-0011 的裁定 P-1(新线程首次进入时按 depth 修正中断形态)与 bh 的
 * `br_irq_in_atomic()` 都读它 —— 返回假的 0 会让那两条判据变成永远为真的空转。
 * ===================================================================== */

static br_u32 s_irq_depth;

br_irq_state_t br_irq_lock(void)
{
    br_irq_state_t st = (br_irq_state_t)(s_irq_depth << BR_IRQ_ST_DEPTH_SHIFT);

    if (s_irq_depth != 0u) {
        st |= BR_IRQ_ST_I_MASK;          /* 已经关着了 ⇒ "进入前 I 位 = 1" */
    }
    s_irq_depth++;
    return st;
}

void br_irq_unlock(br_irq_state_t st)
{
    (void)st;
    if (s_irq_depth == 0u) {
        return;                          /* 下溢护栏(与真实现同纪律) */
    }
    s_irq_depth--;
}

br_u32 br_irq_lock_depth(void)
{
    return s_irq_depth;
}

/* 宿主没有 ISR(测试是单线程协作的)⇒ 恒假。bh 由 work_core.c 的真值回答。 */
br_bool br_irq_in_isr(void)
{
    return BR_FALSE;
}

br_bool br_irq_in_atomic(void)
{
    return (br_work_in_bh() != BR_FALSE) ? BR_TRUE : BR_FALSE;
}

/* `br_sched_thread_entry` 在新线程首次进入时调它(目标上 = 放行 PSTATE.I)。宿主无中断。 */
void br_irq_cpu_enable(void)
{
}

/* =====================================================================
 * 日志 / panic 桩
 * ===================================================================== */

static const char *s_watch;
static br_u32      s_watch_hits;

void host_sched_watch(const char *needle)
{
    s_watch = needle;
    s_watch_hits = 0u;
}

br_u32 host_sched_watch_hits(void) { return s_watch_hits; }

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

    if (s_watch != NULL && strstr(buf, s_watch) != NULL) {
        s_watch_hits++;
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
        if (s_slots[i].t == BR_NULL && free_slot == NULL) {
            free_slot = &s_slots[i];
        }
    }
    if (free_slot == NULL) {
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
 * 入口
 * ===================================================================== */

#define HOST_SUP_STACK_BYTES 16384u
static br_u8 s_sup_stack[HOST_SUP_STACK_BYTES] BR_ALIGN(16);

void host_sched_main(void (*supervisor)(void *))
{
    printf("[HOSTTEST] brickOS prototype — 宿主侧调度测试装置\n");

    if (br_sched_registered() != BR_TRUE) {
        printf("[HOSTTEST] FAIL 调度器未注册(先调插件的 *_early_init)\n");
        exit(1);
    }

    /* I1: register 之后, 调用者所在的启动上下文就是 main 线程 */
    const char *mn = br_task_name(br_task_self());
    if (mn == BR_NULL || strcmp(mn, "main") != 0) {
        printf("[HOSTTEST] FAIL 启动上下文未物质化为 main(得到 '%s')\n",
               (mn != BR_NULL) ? mn : "(null)");
        exit(1);
    }
    printf("[HOSTTEST] register 后 br_task_self()=%s (I1 成立)\n", mn);

    const br_task_attr_t a = {
        .name       = "supervisor",
        .stack      = (void *)(s_sup_stack + HOST_SUP_STACK_BYTES),
        .stack_size = HOST_SUP_STACK_BYTES,
        .prio       = 0u,
        .flags      = 0u,
    };
    br_thread_t *sup = BR_NULL;
    const int rc = br_task_create(&sup, &a, supervisor, BR_NULL);
    if (rc != 0) {
        printf("[HOSTTEST] FAIL create supervisor rc=%d\n", rc);
        exit(1);
    }

    printf("[HOSTTEST] 进入 br_sched_run()(current=main, 就绪=supervisor)\n");
    br_sched_run();                     /* 不返回: supervisor 里 exit() */
}
