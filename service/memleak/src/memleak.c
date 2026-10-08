/*
 * service/memleak — 泄漏/堆破坏报告服务(实现体)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md` §4「ASan / memleak 策略(分层, 诚实工程)」:
 *   - v1.x 的便宜替代 = TLSF 红区(malloc header/footer guard)+ freelist 毒化 + 栈 canary;
 *   - v2.0 的 target memleak = per-plugin arena 记账(分配带插件归属, 关机/dump 时
 *     按插件出泄漏报告)。
 *   本文件实现的是上面两行的**交集**: core 的便宜机制已到位(违约计数由 core 出),
 *   而 v2 的 arena 在本原型里只做到"归属标签"(见 `br/core/br_mem.h` §③ 记账契约)。
 *
 * ★ 诚实声明(与 `br_memleak.h` 抬头一致, ADR-0003):
 *   归属标签 ≠ v2 arena —— 没有预算上限、没有强制归属、没有 OOM 策略。
 *   本插件能回答"哪些块没还 / 是谁标的 / 在哪分配的", 不能回答"某插件超预算"。
 *   WORKAROUND(br-wa-debug-001) 登记的就是这一段差距与它的还债动作(v2 arena)。
 *
 * ★ 只读边界(ADR-0003 §2.6):
 *   本插件**只消费** core 的堆观测契约 —— `br_heap_stats_get` / `br_heap_walk` /
 *   `br_heap_check` / `br_heap_usage` / `br_heap_owner_*`; 不建池、不改堆、
 *   报告路径不分配内存。消费方是插件 ⇒ 观测面必须落在 core 的对外头文件里,
 *   不能藏在 core 内部(与 3-02 §17.4 对 `br_irq_stats_get` 的处置同型)。
 *
 * ★ 静态预算(5-01 §3 的约束"捕获路径只用静态缓冲, 不碰堆/调度器"):
 *   归属归并用**固定 16 槽**的静态数组, 槽满后的归属并进一个 `"(other)"` 溢出槽
 *   (summary 的 owners 计数把这一槽也算一个归属)。
 *   明细行**不在报告期缓存**: 每个归属重新走一遍 `br_heap_walk` 换取零行缓冲
 *   (每次最多打 BR_MEMLEAK_MAX_ROWS_PER_OWNER 条)。这是"用时间换静态 RAM"的裁定 ——
 *   报告是 debug 路径, 走一遍堆不便宜, 但 4 KiB 的插件预算装不下
 *   16 归属 × 8 明细的块表。
 *
 * 线程纪律: 全部 **thread-only**(会打印、会走堆)。
 */
#include <br/debug/br_memleak.h>

#include <br/core/br_log.h>

/* =====================================================================
 * 归属标签(③ 记账契约的消费侧)
 * ===================================================================== */

/*
 * 本插件自己的归属标签。v0.1 没有运行期插件管理器(br-wa-boot-001), 所以
 * `br_memleak_init()` 不保证被调用过; 采用"首次需要时注册 + 成功即缓存"的懒注册,
 * 既满足 init 的语义(幂等), 也让自检/报告在未显式 init 的组合下仍能工作。
 * 注册失败(堆未就绪 / 标签表满)不缓存, 下次再试 —— core 的 register 对重名幂等。
 */
static br_owner_t s_owner      = BR_OWNER_NONE;
static br_bool    s_owner_done = BR_FALSE;

static br_owner_t ml_ensure_owner(void)
{
    if (!s_owner_done) {
        s_owner = br_heap_owner_register("memleak");
        if (s_owner != BR_OWNER_NONE) {
            s_owner_done = BR_TRUE;
        }
    }
    return s_owner;
}

br_owner_t br_memleak_owner(void)
{
    return ml_ensure_owner();
}

/*
 * 5-01 §4 的口径: 这里只注册归属标签(基础账)。
 * ★ 头文件抬头还提到"trace 事件名", 但 memleak 的 plugin.toml **没有**声明
 *   `service/trace` 依赖边 ⇒ 不在这里造一条未声明的调用边(那正是声明面唯一真值
 *   要拦的东西)。事件名留待该依赖边被显式声明后再补, 见 README「待回灌」。
 */
int br_memleak_init(void)
{
    (void)ml_ensure_owner();
    return 0;
}

/* =====================================================================
 * 存活块观测(br_heap_walk 的三个消费者)
 * ===================================================================== */

static int ml_count_cb(void *ctx, const br_heap_block_t *blk)
{
    (void)blk;
    (*(br_u32 *)ctx)++;
    return 0;
}

br_u32 br_memleak_live(void)
{
    br_u32 n = 0u;

    /* 堆未就绪 ⇒ br_heap_walk 返回 -ENODEV; 语义上"没有存活块" */
    if (br_heap_walk(ml_count_cb, &n) < 0) {
        return 0u;
    }
    return n;
}

typedef struct {
    br_owner_t owner;
    br_u32     live;
} ml_owner_count_t;

static int ml_count_owner_cb(void *ctx, const br_heap_block_t *blk)
{
    ml_owner_count_t *c = (ml_owner_count_t *)ctx;

    if (blk->owner == c->owner) {
        c->live++;
    }
    return 0;
}

br_u32 br_memleak_live_by_owner(br_owner_t owner)
{
    ml_owner_count_t c = { owner, 0u };

    if (br_heap_walk(ml_count_owner_cb, &c) < 0) {
        return 0u;
    }
    return c.live;
}

/*
 * ★ 这是**当前状态**, 不是累计。5-01 §4 的违约(红区/魔数/毒化)落在 core 的块头
 *   与空闲链里, `br_heap_check()` 每次调用都重扫全堆 ⇒ "此刻还剩几处违约"。
 *   累计口径(redzone_hits / canary_hits / double_free)在 `br_heap_stats_t` 里,
 *   由 `br_memleak_report()` 的 summary 之外单独取用; 本函数刻意不做两者相加,
 *   免得"修好的破坏"被永远记成红 —— 用例 TC-DBG-041 依赖这个语义
 *   (复原红区之后必须回到 0)。
 */
br_u32 br_memleak_corruptions(void)
{
    return br_heap_check();
}

/* =====================================================================
 * 报告: 归属归并(静态上界 + "(other)" 溢出槽)
 * ===================================================================== */

#define ML_OWNER_SLOTS   16u   /* 逐归属上界: 5-01 §3 的静态缓冲纪律 */
#define ML_OTHER_NAME    "(other)"

typedef struct {
    br_bool    used;
    br_owner_t owner;
    br_u32     live;
    br_size_t  bytes;
} ml_slot_t;

static ml_slot_t s_slots[ML_OWNER_SLOTS];
static br_u32    s_slot_count;      /* 已占用槽数(<= ML_OWNER_SLOTS) */

static br_bool   s_other_used;      /* 溢出槽是否被用过 */
static br_u32    s_other_live;
static br_size_t s_other_bytes;

static br_u32    s_sum_live;        /* 本次遍历的全局存活块/字节 */
static br_size_t s_sum_bytes;

static void ml_reset(void)
{
    for (br_u32 i = 0u; i < ML_OWNER_SLOTS; i++) {
        s_slots[i].used  = BR_FALSE;
        s_slots[i].owner = BR_OWNER_NONE;
        s_slots[i].live  = 0u;
        s_slots[i].bytes = 0u;
    }
    s_slot_count  = 0u;
    s_other_used  = BR_FALSE;
    s_other_live  = 0u;
    s_other_bytes = 0u;
    s_sum_live    = 0u;
    s_sum_bytes   = 0u;
}

/* 槽表里找归属; 未登记 ⇒ -1(不区分"溢出"与"没见过" —— 两者都并进 "(other)") */
static int ml_slot_find(br_owner_t owner)
{
    for (br_u32 i = 0u; i < s_slot_count; i++) {
        if (s_slots[i].owner == owner) {
            return (int)i;
        }
    }
    return -1;
}

/* 取(必要时新建)归属槽; 表满 ⇒ BR_NULL(调用方并进 "(other)") */
static ml_slot_t *ml_slot_take(br_owner_t owner)
{
    const int idx = ml_slot_find(owner);

    if (idx >= 0) {
        return &s_slots[idx];
    }
    if (s_slot_count < ML_OWNER_SLOTS) {
        ml_slot_t *slot = &s_slots[s_slot_count];
        slot->used  = BR_TRUE;
        slot->owner = owner;
        slot->live  = 0u;
        slot->bytes = 0u;
        s_slot_count++;
        return slot;
    }
    return BR_NULL;
}

static int ml_aggr_cb(void *ctx, const br_heap_block_t *blk)
{
    ml_slot_t *slot;

    (void)ctx;
    slot = ml_slot_take(blk->owner);
    if (slot != BR_NULL) {
        slot->live++;
        slot->bytes += blk->size;
    } else {
        s_other_used = BR_TRUE;
        s_other_live++;
        s_other_bytes += blk->size;
    }
    s_sum_live++;
    s_sum_bytes += blk->size;
    return 0;
}

/* summary 的 owners 口径: 有存活块的归属数(含 "(other)" 这一伪归属) */
static br_u32 ml_owner_total(void)
{
    br_u32 n = 0u;

    for (br_u32 i = 0u; i < s_slot_count; i++) {
        if (s_slots[i].used && s_slots[i].live > 0u) {
            n++;
        }
    }
    if (s_other_used && s_other_live > 0u) {
        n++;
    }
    return n;
}

typedef struct {
    br_owner_t owner;
    br_bool    other;    /* 匹配"不在槽表里"的归属 */
    br_u32     rows;
} ml_detail_ctx_t;

static int ml_detail_cb(void *ctx, const br_heap_block_t *blk)
{
    ml_detail_ctx_t *c = (ml_detail_ctx_t *)ctx;
    br_bool          match;

    if (c->rows >= BR_MEMLEAK_MAX_ROWS_PER_OWNER) {
        return 0;   /* 已列满: 继续走完但不打印(超出部分只体现在 owner 行的计数里) */
    }
    if (c->other) {
        match = (ml_slot_find(blk->owner) < 0) ? BR_TRUE : BR_FALSE;
    } else {
        match = (blk->owner == c->owner) ? BR_TRUE : BR_FALSE;
    }
    if (!match) {
        return 0;
    }

    br_log_info("[LEAK]   blk seq=%u addr=0x%lx size=%lu caller=0x%lx",
                blk->seq, (br_uintptr_t)blk->addr, blk->size, blk->caller);
    c->rows++;
    return 0;
}

br_u32 br_memleak_report(void)
{
    br_heap_stats_t st;
    br_u32          owners;
    br_u32          corrupt;

    /* 第一道门: 堆未就绪就只留一行, 不假装有账 */
    if (br_heap_stats_get(&st) != 0) {
        br_log_info("[LEAK] heap not ready");
        return 0u;
    }

    ml_reset();
    if (br_heap_walk(ml_aggr_cb, BR_NULL) < 0) {
        br_log_info("[LEAK] heap not ready");
        return 0u;
    }

    owners  = ml_owner_total();
    corrupt = br_memleak_corruptions();

    br_log_info("[LEAK] summary live=%u bytes=%lu owners=%u corrupt=%u high_water=%lu",
                s_sum_live, s_sum_bytes, owners, corrupt, st.high_water);

    /* 归属汇总行 + 每个归属最多 BR_MEMLEAK_MAX_ROWS_PER_OWNER 条明细 */
    for (br_u32 i = 0u; i < s_slot_count; i++) {
        ml_detail_ctx_t c;

        if (!s_slots[i].used || s_slots[i].live == 0u) {
            continue;
        }
        br_log_info("[LEAK] owner=%s live=%u bytes=%lu",
                    br_heap_owner_name(s_slots[i].owner),
                    s_slots[i].live, s_slots[i].bytes);
        c.owner = s_slots[i].owner;
        c.other = BR_FALSE;
        c.rows  = 0u;
        (void)br_heap_walk(ml_detail_cb, &c);
    }

    if (s_other_used && s_other_live > 0u) {
        ml_detail_ctx_t c;

        br_log_info("[LEAK] owner=%s live=%u bytes=%lu",
                    ML_OTHER_NAME, s_other_live, s_other_bytes);
        c.owner = BR_OWNER_NONE;   /* other 分支按"不在槽表里"匹配, owner 字段不参与 */
        c.other = BR_TRUE;
        c.rows  = 0u;
        (void)br_heap_walk(ml_detail_cb, &c);
    }

    return s_sum_live;
}

/* =====================================================================
 * 自检(设计 6-01 的 TC-DBG-04x; target-only, 打印 PASS/FAIL)
 * ===================================================================== */

static br_u32 s_ml_pass;
static br_u32 s_ml_fail;

static void ml_report(br_bool ok, const char *tag, const char *what)
{
    if (ok) {
        s_ml_pass++;
        br_log_info("[DBGCONF] PASS %s %s", tag, what);
    } else {
        s_ml_fail++;
        br_log_info("[DBGCONF] FAIL %s %s", tag, what);
    }
}

/*
 * TC-DBG-040 真泄漏可检出。
 * 故意"丢掉指针"的含义是: 判据只用 core 的观测(br_heap_walk / report), 不看返回值;
 * 但用例收尾仍用那块地址归还, 不给系统留真泄漏(用例是判据, 不是破坏现场)。
 */
static void ml_case_040(void)
{
    const br_u32     live0 = br_memleak_live();
    const br_u32     mine0 = br_memleak_live_by_owner(br_memleak_owner());
    const br_owner_t prev  = br_heap_owner_set(br_memleak_owner());
    void            *leaked;
    br_u32           live1;
    br_u32           mine1;
    br_u32           rep;

    /* 归属标签必须真的注册过, 否则本用例退化成"未归属块"的计数 */
    ml_report(br_memleak_owner() != BR_OWNER_NONE, "TC-DBG-040",
              "归属标签已注册(br_memleak_owner != NONE)");

    leaked = br_malloc(123u);   /* 指针故意不参与判据 */

    live1 = br_memleak_live();
    mine1 = br_memleak_live_by_owner(br_memleak_owner());
    rep   = br_memleak_report();   /* 打印现场, 顺带回传存活块数 */

    (void)br_heap_owner_set(prev);

    ml_report(leaked != BR_NULL, "TC-DBG-040", "br_malloc(123) 成功(指针故意丢掉)");
    ml_report(mine1 >= mine0 + 1u, "TC-DBG-040",
              "本归属存活块 >= 基线+1(br_memleak_live_by_owner)");
    /* report 的 live 是被打印口径, 必须与独立观测一致; 且增量只可能来自本归属 */
    ml_report((rep == live1) && ((live1 - live0) == (mine1 - mine0)), "TC-DBG-040",
              "report 的 live 与观测一致且增量归属唯一");

    br_free(leaked);   /* 还堆干净状态 */
}

/*
 * TC-DBG-041 红区越界可检出。
 * 依赖 core 的块布局约定: 用户区 p..p+size-1, 紧随其后是红区 ⇒ 请求 64 B 时
 * 首字节越界落在 p+64; 红区填充值 0xA5(见 core 的 mem.c 块布局注释)。
 * 用例收尾把那一字节复原 —— 否则堆会带着"修不好的破坏"进入后续用例。
 */
static void ml_case_041(void)
{
    void           *p = br_malloc(64u);
    volatile br_u8 *rz;
    br_u32          bad;
    br_u32          fixed;

    if (p == BR_NULL) {
        ml_report(BR_FALSE, "TC-DBG-041", "br_malloc(64) 失败, 无法构造红区场景");
        return;
    }

    rz = (volatile br_u8 *)p;
    rz[64] = 0xFFu;               /* 越界 1 字节: 落在红区首字节 */
    bad = br_heap_check();

    rz[64] = 0xA5u;               /* 复原为 core 的红区填充值 */
    fixed = br_heap_check();

    ml_report(bad > 0u, "TC-DBG-041", "红区被踩 1 字节 ⇒ br_heap_check() > 0");
    ml_report(fixed == 0u, "TC-DBG-041", "红区复原(0xA5)后 br_heap_check() == 0");

    br_free(p);
}

/*
 * TC-DBG-042 双重释放可检出且不破坏堆。
 * 第二次 free 必须被 core 的重复释放拦截(double_free 计数), 且块链仍然自洽、
 * 之后还能正常分配/释放一次。
 */
static void ml_case_042(void)
{
    void            *p = br_malloc(32u);
    br_heap_stats_t  st = { 0 };
    void            *q;
    br_bool          ok;
    int              rc;

    if (p == BR_NULL) {
        ml_report(BR_FALSE, "TC-DBG-042", "br_malloc(32) 失败, 无法构造双重释放场景");
        return;
    }

    br_free(p);
    br_free(p);   /* 第二次: 应被拦截, 不进空闲链 */

    rc = br_heap_stats_get(&st);
    ml_report((rc == 0) && (st.double_free >= 1u), "TC-DBG-042",
              "第二次 br_free 被拦截(double_free >= 1)");
    ml_report(br_heap_check() == 0u, "TC-DBG-042",
              "双重释放之后堆仍干净(br_heap_check == 0)");

    q  = br_malloc(48u);
    ok = (q != BR_NULL) ? BR_TRUE : BR_FALSE;
    if (ok) {
        br_free(q);
    }
    ml_report(ok, "TC-DBG-042", "双重释放之后仍能正常分配/释放一次");
}

/*
 * TC-DBG-043 记账匹配。
 * 增量判据用实际观测: 10 × 100 B 的 used 增量 >= 1000(每块还带 header/footer/红区,
 * 故实际更大); 全部归还后必须**精确**回到基线(不放宽成 <=)。
 */
static void ml_case_043(void)
{
    br_size_t u0 = 0u, total0 = 0u;
    br_size_t u1 = 0u, total1 = 0u;
    br_size_t u2 = 0u, total2 = 0u;
    void     *blk[10];
    br_u32    got = 0u;
    int       rc0;
    int       rc1;
    int       rc2;

    for (br_u32 i = 0u; i < 10u; i++) {
        blk[i] = BR_NULL;
    }

    rc0 = br_heap_usage(&u0, &total0);
    for (br_u32 i = 0u; i < 10u; i++) {
        blk[i] = br_malloc(100u);
        if (blk[i] != BR_NULL) {
            got++;
        }
    }
    rc1 = br_heap_usage(&u1, &total1);
    for (br_u32 i = 0u; i < 10u; i++) {
        if (blk[i] != BR_NULL) {
            br_free(blk[i]);
        }
    }
    rc2 = br_heap_usage(&u2, &total2);

    ml_report((rc0 == 0) && (got == 10u), "TC-DBG-043", "基线可读且 10 × 100 B 全部分配成功");
    ml_report((rc1 == 0) && (u1 >= u0 + 1000u), "TC-DBG-043",
              "used 增量 >= 1000(10 × 100 B)");
    ml_report((rc2 == 0) && (u2 == u0), "TC-DBG-043",
              "全部释放后 used 精确回到基线");
}

int br_memleak_selftest(void)
{
    s_ml_pass = 0u;
    s_ml_fail = 0u;

    br_log_info("[DBGCONF] info memleak selftest (TC-DBG-040..043)");

    /* 幂等: 即使用例被编排者(br_dump_conformance)在未显式 init 的组合下调起 */
    (void)br_memleak_init();

    ml_case_040();
    ml_case_041();
    ml_case_042();
    ml_case_043();

    /* 插件自有的摘要行(编排者的总摘要另打, 门禁只认总摘要的 fail=0) */
    br_log_info("[DBGCONF] SUMMARY memleak pass=%u fail=%u total=%u",
                s_ml_pass, s_ml_fail, s_ml_pass + s_ml_fail);
    return (int)s_ml_fail;
}
