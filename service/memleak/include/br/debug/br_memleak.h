/*
 * service/memleak — 泄漏与堆破坏报告服务(对外声明面)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md` §4 "ASan / memleak 策略(分层, 诚实工程)":
 *   - **target 便宜替代**(v1.x): "TLSF 红区(malloc header/footer guard)+ freelist
 *     毒化 + 栈 canary";
 *   - **target memleak**(v2.0): "per-plugin arena 记账: 分配带插件归属, 关机/dump 时
 *     按插件出泄漏报告"。
 *
 * ★ 诚实声明(ADR-0003): 本原型实现的是**上面两行的交集**——
 *   v1.x 的便宜机制(红区/毒化/重复释放拦截, 由 core 堆实现)**已经到位**;
 *   而 v2 的 per-plugin arena **只做到"归属标签"**(core 的 `br_heap_owner_*`:
 *   分配时把"当前归属"记进块头), **没有**预算上限、没有强制归属、没有 OOM 策略。
 *   所以本插件能回答"哪些块没还、是谁标的、在哪分配的", 不能回答"某插件超预算"。
 *
 * 依赖: core 的**观测契约**(`br_heap_stats_get`/`br_heap_walk`/`br_heap_check`)。
 * 本插件不改堆, 只读它 —— 这是"消费方是插件 ⇒ 契约不能藏在 core 内部"的又一例
 * (与 3-02 §17.4 对 `br_irq_stats_get` 的处置同型)。
 *
 * ISR 安全: 全部 **thread-only**。
 */
#ifndef BR_DEBUG_BR_MEMLEAK_H
#define BR_DEBUG_BR_MEMLEAK_H

#include <br/core/br_mem.h>
#include <br/core/br_types.h>

/* 报告里每个归属标签最多列出的明细块数(超出只给计数) */
#define BR_MEMLEAK_MAX_ROWS_PER_OWNER  8u

/* LATE 相 init: 注册本插件的归属标签(基础账) + trace 事件名。返回 0。 */
int br_memleak_init(void);

/* 自检入口(TC-DBG-04x)已移到 `src/memleak_selftest.c`(ADR-0010): 生成物按 symbol_prefix
 * 发 `.selftest = memleak_selftest`, 由 core 在全部 start 之后统一驱动。
 * ★ 测试入口**不进**本插件的 golden 接口面 —— 否则改一条用例就成了对外接口变更
 *   (接口 hash 覆盖的正是声明面), 与"自检与生产分离"的裁定冲突。 */

/*
 * 泄漏报告。打印:
 *   `[LEAK] summary live=<n> bytes=<n> owners=<n> corrupt=<n> high_water=<n>`
 *   每个有存活块的归属一行: `[LEAK] owner=<name> live=<n> bytes=<n>`
 *   该归属下的明细行(最多 BR_MEMLEAK_MAX_ROWS_PER_OWNER 条):
 *       `[LEAK]   blk seq=<n> addr=0x<hex> size=<n> caller=0x<hex>`
 * 返回**存活块数**(便于用例判据)。
 */
br_u32 br_memleak_report(void);

/* 存活块数 / 某个归属的存活块数(判据用) */
br_u32 br_memleak_live(void);
br_u32 br_memleak_live_by_owner(br_owner_t owner);

/* 破坏计数 = core `br_heap_check()` 的违约数(红区/魔数/毒化) + 本插件累计观察到的峰值 */
br_u32 br_memleak_corruptions(void);

/* 本插件自己的归属标签(用例用它断言"归属标签能区分出自己造成的泄漏") */
br_owner_t br_memleak_owner(void);

#endif /* BR_DEBUG_BR_MEMLEAK_H */
