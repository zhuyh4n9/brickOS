/*
 * brickOS prototype v0.1.0 — 最小 trace 环(ISR-safe 的事件留痕设施)
 *
 * 权威设计: `docs/5-debug/5-01-debug.md`(16 B 定长事件 + ISR 内可记录)
 * 与 `docs/3-os-core/3-02-int.md` 里大量"计数 + 留痕"的义务:
 *   §4.2 能力降级必须留痕 / §7.1 优先级量化两个方向都报 / §5.2 spurious /
 *   §8.3 风暴保护默认动作 = trace + 继续 / §9.3/§9.6 域事件 / §10.5/§10.6 fault。
 *
 * ISR 安全性(硬约束): br_trace_emit 只做"一次槽位写 + 一次 head 递增",
 * **不取锁、不分配、不读时钟、不打日志**。这样它才是唯一能从 ISR/fault 路径调用的
 * 留痕设施(CA-3 的白名单精神)。单核 v0.1 ⇒ 单生产者/单消费者, 无需原子。
 */
#include <br/core/br_trace.h>

static br_trace_evt_t s_ring[BR_TRACE_RING_SIZE];
static br_u32 s_head;      /* 生产者: 累计发出数(单调增) */
static br_u32 s_tail;      /* 消费者: 累计已消费数 */
static br_u32 s_overrun;   /* 生产者覆盖过"尚未消费"槽位的次数 */

void br_trace_emit(br_u32 id, br_u32 a, br_u64 b)
{
    /* 环满(head - tail == SIZE)⇒ 本条会覆盖尚未消费的最旧槽位 */
    if ((s_head - s_tail) >= (br_u32)BR_TRACE_RING_SIZE) {
        s_overrun++;
        /* ★ 这里**不推进 tail**: 满环下的语义是"丢最旧、保最新", 而 tail 的推进权
         *   归消费者(drain)。代价是消费端可能重放被覆盖过的槽位 —— 因此
         *   br_trace_overrun() > 0 才是"这段历史不可信"的唯一诚实信号。 */
    }

    br_trace_evt_t *slot = &s_ring[s_head % (br_u32)BR_TRACE_RING_SIZE];
    slot->id = id;
    slot->a  = a;
    slot->b  = b;

    s_head++;
}

br_u32 br_trace_total(void)
{
    return s_head;
}

br_u32 br_trace_drain(br_trace_evt_t *out, br_u32 max)
{
    if (out == BR_NULL || max == 0u) {
        return 0u;
    }

    /* 先快照 head: 生产者(可能是 ISR)随后继续推进也不影响本次消费的一致视图 */
    const br_u32 head    = s_head;
    const br_u32 backlog = head - s_tail;
    br_u32 start;
    br_u32 avail;

    /*
     * ★ 起点必须是**最旧的未消费事件**(s_tail), 不是 `head - avail`
     *   —— 后者在 `avail` 被 `max` 截断时会读"最新的 max 条"并把
     *   `s_tail` 直接推到 head, **静默丢掉中间那段积压**(实测踩到: 风暴期
     *   积压 160 条, 消费端每次只取 16 条 ⇒ 越限那一条被跳过, 用例判红)。
     *   唯一允许丢弃的情形是"生产者已跑过不止一圈": 那时最早的若干条**物理上**
     *   已被覆盖, 只能从 `head - SIZE` 开始(并且 br_trace_overrun() > 0)。
     */
    if (backlog > (br_u32)BR_TRACE_RING_SIZE) {
        start = head - (br_u32)BR_TRACE_RING_SIZE;   /* 被覆盖的部分无法读回 */
        avail = (br_u32)BR_TRACE_RING_SIZE;
    } else {
        start = s_tail;
        avail = backlog;
    }

    if (avail > max) {
        avail = max;                                 /* 只取最旧的 max 条, 其余留待下次 */
    }

    for (br_u32 i = 0u; i < avail; i++) {
        out[i] = s_ring[(start + i) % (br_u32)BR_TRACE_RING_SIZE];
    }
    s_tail = start + avail;

    return avail;
}

br_u32 br_trace_overrun(void)
{
    return s_overrun;
}

const char *br_trace_name(br_u32 id)
{
    switch (id) {
    case BR_TRACE_NONE:                    return "none";
    /* 中断路径 */
    case BR_TRACE_IRQ_ENTER:               return "irq_enter";
    case BR_TRACE_IRQ_EXIT:                return "irq_exit";
    case BR_TRACE_IRQ_SPURIOUS_OWNED:      return "irq_spurious_owned";
    case BR_TRACE_IRQ_SPURIOUS_NOCAP:      return "irq_spurious_nocap";
    case BR_TRACE_IRQ_SUPPRESSED:          return "irq_suppressed";
    case BR_TRACE_IRQ_STORM:               return "irq_storm";
    case BR_TRACE_IRQ_PRIO_QUANTIZED:      return "irq_prio_quantized";
    case BR_TRACE_IRQ_DISPATCH_IGNORED:    return "irq_dispatch_ignored";
    case BR_TRACE_IRQ_UNLOCK_UNDERFLOW:    return "irq_unlock_underflow";
    /* 级联域 */
    case BR_TRACE_IRQ_DEMUX:               return "irq_demux";
    case BR_TRACE_IRQ_DEMUX_OVERFLOW:      return "irq_demux_overflow";
    case BR_TRACE_IRQ_DOMAIN_ORPHAN:       return "irq_domain_orphan";
    case BR_TRACE_IRQ_DOMAIN_DROP:         return "irq_domain_drop";
    case BR_TRACE_IRQ_DOMAIN_NOBH:         return "irq_domain_nobh";
    /* fault 路径 */
    case BR_TRACE_FAULT:                   return "fault";
    case BR_TRACE_FAULT_FIXUP:             return "fault_fixup";
    case BR_TRACE_FAULT_DOUBLE:            return "fault_double";
    /* 下半部(ADR-0011) */
    case BR_TRACE_IRQ_BH_DEFER:            return "irq_bh_defer";
    case BR_TRACE_IRQ_BH_DROP:             return "irq_bh_drop";
    case BR_TRACE_IRQ_DISPATCH_SUPPRESSED: return "irq_dispatch_suppressed";
    case BR_TRACE_BH_CTX_VIOLATION:        return "bh_ctx_violation";
    case BR_TRACE_SCHED_PREEMPT:           return "sched_preempt";
    default:                               return "?";
    }
}
