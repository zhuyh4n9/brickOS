/*
 * brickOS prototype v0.2.0 — 调度框架 + coop 的**自检套件**(core/selftest/)
 *
 * 设计依据与逐条裁定: `docs/decisions/0010-plugin-selftest.md`。
 * 用例口径: `6-01-test.md` §3.1/§3.3(`TC-TASK-*` / `TC-TIME-*`); 实现依据 `3-03-sched.md`。
 *
 * ## 为什么这几十行从 `sched_core.c` 搬到这里
 *   它们本来就是**测试**, 却与生产实现同住一个 .c —— 于是"关掉自检"关不掉它们,
 *   而"读生产实现"的人要在一千行机制里翻找判定逻辑。搬到本目录后:
 *     - `core/src/sched/sched_core.c` 只留机制(状态机/超时/切换/线程面);
 *     - 自检关掉 ⇒ `core/selftest/` 整目录不参与编译 ⇒ 用例代码**不进镜像**
 *       (机械事实, 不是"编了不跑"; 见 product.toml 的 selftest_sources 与 build.rs);
 *     - 用例的读者与实现的读者不再互相干扰。
 *   代码是**搬移**, 不是重写: 判据/日志/顺序逐字未动(见下面的日志契约)。
 *
 * ## 唯一的跨边界观测
 *   套件真的 create/yield/join/sleep, 并要打出"注册进来的是哪个调度器"这条证据
 *   (`scheduler=%s kind=%u spawn=%u`)。这些字段只存在于生产侧的注册表里 ——
 *   经 `sched_internal.h` 的 `br_sched_ops_get()` 读取, 而**不是**在这里复制第二份
 *   插件表(那会让"注册了什么"有两处真值)。
 *
 * ## 日志契约(门禁按**逐字**匹配 ⇒ 改一个字就是改判据)
 *   `[TASKCONF] PASS/FAIL <id> <desc>` … `[TASKCONF] SUMMARY pass=%d fail=%d total=%d`
 *   `tests/gates.toml` 的 require/forbid/require_tags 与 `sched/coop/tests/smoke.toml`
 *   的 tag 表都锚在这两行上。
 *
 * ## 返回值
 *   `int` = **失败项数**(ADR-0010 §2.4: 套件只汇总不下判决, 由 `br_core_selftest()`
 *   相加、由门禁判红绿)。任何失败都**不停机** —— 日志里如实打出来即可。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_sched.h>
#include <br/core/br_time.h>
#include <br/core/br_types.h>

/* TCB 公共头(BR_TASK_MAX / struct br_thread)与"注册表只读观测"是 core 私有面;
 * 跨目录相对包含在本仓有先例(core/src/sync/sync.c -> "../sched/sched_internal.h")。 */
#include "../src/sched/sched_internal.h"

/* 入口的可见声明集中在 `core/selftest/core_selftest.c`(它是唯一聚合器); 这里再声明
 * 一次是为了满足本文件"每个函数使用前都有原型"的纪律(-Wmissing-prototypes)。 */
int br_sched_selftest(void);

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

int br_sched_selftest(void)
{
    s_conf_pass = 0;
    s_conf_fail = 0;

    /* 注册表只读快照(与旧实现逐字同源: 这里原本直接读 sched_core.c 的 s_ops)。 */
    const br_sched_ops_t *ops = br_sched_ops_get();

    br_log_info("[TASKCONF] ==== sched framework + coop conformance (3-03 / 6-01 §3.1/§3.3)");
    br_log_info("[TASKCONF] ---- scheduler=%s kind=%u self=%s spawn=%u",
                (ops != BR_NULL) ? ops->name : "?",
                (ops != BR_NULL) ? ops->kind : 0u,
                br_task_name(br_task_self()),
                (br_u32)(ops != BR_NULL ? ops->tcb_size : 0u));

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

    /* 返回**失败项数**: 聚合器相加, 红绿由门禁判(ADR-0010 §2.4)。 */
    return s_conf_fail;
}
