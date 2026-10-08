/*
 * brickOS prototype v0.1.0 — 最小 trace 环(ISR-safe 的事件留痕设施)
 *
 * 设计依据: `docs/5-debug/5-01-debug.md`(16 B 定长事件 + ctx 字段; ISR 内可记录)
 * 与 `3-02-int.md` 里大量"**计数 + trace 留痕**"的义务:
 *   §4.2 能力降级的降级行为(须留痕)、§7.1 优先级量化的**两个方向都报**、
 *   §5.2 spurious、§8.3 风暴保护(默认动作 = trace + 继续)、§9.3/§9.4 域事件。
 *
 * ⚠ 归属说明: 设计侧的 trace 是 **Service/观测设施**(5-01), 不是 core 内部件。
 *   本原型没有服务注册表(WORKAROUND br-wa-boot-001), 所以这里是一个
 *   **core 内的最小环形缓冲**, 只为把上面那些"必须留痕"的裁决变成可观测的证据;
 *   service/trace 落地后, `br_trace_emit` 的调用点保留, 实现换成服务即可。
 *
 * ISR 安全性: `br_trace_emit` 是单生产者写、单消费者读的无锁环(单核 v0.1)——
 * 只做一次槽位写 + 一次 head 递增; 不取锁、不分配、不读时钟。
 * 消费者(线程上下文)先快照 head 再逐条拷贝, 最后推进 tail; 生产者溢出会覆盖
 * **尚未消费**的旧槽位(记为 `br_trace_overrun()`), 这是"宁可丢旧事件也不阻塞中断"
 * 的有意取舍。
 */
#ifndef BR_CORE_BR_TRACE_H
#define BR_CORE_BR_TRACE_H

#include <br/core/br_types.h>

/* 事件 id(与 5-01 §1 的事件族同型: 纯数字, 便于文本/二进制两种呈现) */
enum {
    BR_TRACE_NONE = 0,
    /* 中断路径 */
    BR_TRACE_IRQ_ENTER = 1,          /* a = virq, b = hwirq */
    BR_TRACE_IRQ_EXIT = 2,           /* a = virq */
    BR_TRACE_IRQ_SPURIOUS_OWNED = 3, /* a = hwirq(有硬件屏蔽却仍到达 ⇒ 真 bug 信号) */
    BR_TRACE_IRQ_SPURIOUS_NOCAP = 4, /* a = hwirq(无屏蔽能力时的预期丢弃) */
    BR_TRACE_IRQ_SUPPRESSED = 5,     /* a = virq(CAP_MASK=0 的软件抑制) */
    BR_TRACE_IRQ_STORM = 6,          /* a = virq, b = 窗口内计数 */
    BR_TRACE_IRQ_PRIO_QUANTIZED = 7, /* a = virq, b = (encoded << 8) | logical */
    BR_TRACE_IRQ_DISPATCH_IGNORED = 8, /* a = virq, b = 绑定的静态 dispatch 值 */
    BR_TRACE_IRQ_UNLOCK_UNDERFLOW = 9, /* a = 多余额度(下溢护栏拦截) */
    /* 级联域 */
    BR_TRACE_IRQ_DEMUX = 10,          /* a = dom_id, b = 子位图 */
    BR_TRACE_IRQ_DEMUX_OVERFLOW = 11, /* a = dom_id(轮数用尽仍未读空) */
    BR_TRACE_IRQ_DOMAIN_ORPHAN = 12,  /* a = dom_id, b = sub(无属主 ⇒ 屏蔽) */
    BR_TRACE_IRQ_DOMAIN_DROP = 13,    /* a = dom_id(SLOW 域提交失败的推迟/丢弃) */
    BR_TRACE_IRQ_DOMAIN_NOBH = 14,    /* a = 请求的 flags(Stage 1 无 bh ⇒ 拒绝 SLOW 域) */
    /* fault 路径 */
    BR_TRACE_FAULT = 15,              /* a = EC, b = ESR_EL1 */
    BR_TRACE_FAULT_FIXUP = 16,        /* a = (br_u32)ELR, b = errno_val */
    BR_TRACE_FAULT_DOUBLE = 17,       /* a = in_fault(≥2) */
    BR_TRACE__COUNT
};

/* 16 B 定长事件(5-01 §1 的形态) */
typedef struct {
    br_u32 id;
    br_u32 a;
    br_u64 b;
} br_trace_evt_t;

_Static_assert(sizeof(br_trace_evt_t) == 16u, "trace 事件必须是 16 B 定长");

#define BR_TRACE_RING_SIZE  256u

/* ISR-safe: 记一条事件(不取锁/不分配/不读时钟)。 */
void br_trace_emit(br_u32 id, br_u32 a, br_u64 b);

/* 累计发出的事件数(单调递增; 用于判断"有没有新事件")。 */
br_u32 br_trace_total(void);

/* 把**尚未消费**的事件拷进 out(最多 max 条), 返回拷贝条数。线程上下文调用。 */
br_u32 br_trace_drain(br_trace_evt_t *out, br_u32 max);

/* 生产者覆盖过尚未消费的槽位的次数(>0 表示 trace 环不够大/消费太慢)。 */
br_u32 br_trace_overrun(void);

/* 事件名的可读文本(呈现用; 未知 id ⇒ "?")。 */
const char *br_trace_name(br_u32 id);

#endif /* BR_CORE_BR_TRACE_H */
