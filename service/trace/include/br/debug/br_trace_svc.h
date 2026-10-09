/*
 * service/trace — trace 观测服务(对外声明面)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md` §1(trace 插件: 16 B 定长事件 + 环形缓冲 +
 * "热路径零字符串: id→名字的映射表随插件元数据导出, 离线解码") 与
 * `docs/1-architecture/1-03-roadmap.md` §1 的 v1.0 插件清单行
 * (`service/trace` | Service | 16B 事件环形缓冲 | core | M2)。
 *
 * ★ **存储与发射点在 core, 解码/呈现/过滤在本插件**(ADR-0003 的裁定)：
 *   core 的 `br_trace_emit()` 是 CA-3 ISR 白名单的一员(中断/fault 路径必须能留痕,
 *   且不能依赖"服务是否已 init"), 所以**环本身留在 core**(`br/core/br_trace.h`);
 *   本插件是它的**唯一消费方**: 取走(drain)、解码(id→名字)、计数、成行呈现。
 *   这样分层与 5-01 §1 不冲突 —— 5-01 要的是"trace 是插件(可整层移除)", 而
 *   "谁持有环"只是实现归属; 插件缺席时环照旧记录, 只是没人消费。
 *
 * ISR 安全: 本插件的全部函数都是 **thread-only**(drain 会推进消费游标)。
 */
#ifndef BR_DEBUG_BR_TRACE_SVC_H
#define BR_DEBUG_BR_TRACE_SVC_H

#include <br/core/br_types.h>

/* 动态事件 id 的基址: 核心固定 id 在 1..BR_TRACE__COUNT-1(见 br/core/br_trace.h),
 * 插件动态注册的事件 id 从本基址起分配, 两者不重叠。 */
#define BR_TRACE_SVC_ID_BASE   1024u
#define BR_TRACE_SVC_ID_SLOTS  32u     /* 动态 id 槽位数(manifest 裁剪归 v1.x) */

/* LATE 相 init: 清计数 + 注册本插件自有事件名。返回 0。 */
int br_trace_svc_init(void);

/* 自检入口(TC-DBG-00x)已移到 `src/trace_selftest.c`(ADR-0010): 生成物按 symbol_prefix
 * 发 `.selftest = trace_selftest`, 由 core 在全部 start 之后统一驱动。
 * ★ 测试入口**不进**本插件的 golden 接口面 —— 否则改一条用例就成了对外接口变更
 *   (接口 hash 覆盖的正是声明面), 与"自检与生产分离"的裁定冲突。 */

/*
 * 标记(marker): 线程上下文调用(不是 ISR 路径 —— ISR 路径直接用 core 的
 * `BR_TRACE_EVT`/`br_trace_emit`)。语义:
 *   ① 把 `tag` 注册进动态名字表(hash → 槽位; **槽位被别的 tag 占用时向后线性探测**
 *      空槽, 最多扫过全表 —— 32 槽的表不允许"一次 hash 撞车就报错", 那会让
 *      "谁先注册"决定别人能不能注册; 全表满或同名但指针不同 ⇒ `-ENOSPC`);
 *   ② 发一条事件 `id`(动态 id), a = tag 的 32 位 hash, b = 当前时钟(us)。
 * 返回事件 id(>= BR_TRACE_SVC_ID_BASE) 或负 errno。
 */
int br_trace_svc_marker(const char *tag);

/* 显式注册一个动态事件名(供其它插件给自有 id 起名); 返回 id 或负 errno。
 * 冲突策略与 `br_trace_svc_marker` 同: 线性探测, 表满 ⇒ `-ENOSPC`。 */
int br_trace_svc_register(const char *name);

/* id → 名字: 先查动态表, 再退回 core 的固定表(`br_trace_name`)。未知 ⇒ "?"。 */
const char *br_trace_svc_name(br_u32 id);

/*
 * 取走并呈现: 从 core 环取最多 `max_events` 条(0 = 不限, 用尽为止),
 * 逐条打印 `[TRACE] #<seq> id=<id> name=<name> a=0x<hex> b=0x<hex>`,
 * 并对每个 id 累计计数。返回本次取走的事件条数。
 */
br_u32 br_trace_svc_report(br_u32 max_events);

/* 汇总行: `[TRACE] total=<n> drained=<n> overrun=<n> ids=<n> live=<n>`
 *   total   = 生产者累计发出数(br_trace_total)
 *   drained = 本插件累计取走数
 *   overrun = 生产者覆盖未消费槽位的次数(br_trace_overrun; >0 ⇒ 这段历史不可信)
 *   ids     = 见过的不同事件 id 数
 *   live    = 当前仍未被消费的条数(total - drained, 下界 0)
 * 返回 `drained` 的当前值。 */
br_u32 br_trace_svc_summary(void);

/* 某个 id 的累计**已消费**条数(观测/用例判据)。 */
br_u32 br_trace_svc_seen(br_u32 id);

/* 清零"已消费"计数与 live 表(不动 core 环, 也不动生产游标)。 */
void br_trace_svc_reset(void);

#endif /* BR_DEBUG_BR_TRACE_SVC_H */
