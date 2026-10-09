/*
 * service/dump — 现场倾倒服务(对外声明面): region 清单 / 内存 dump / 堆 / trace /
 * 回溯 / 泄漏 —— 即 5-01 §3「mini ramdump」在 v1 的**可落地子集**。
 *
 * 设计出处: `docs/5-debug/5-01-debug.md`
 *   §2 `MEMRD`/`TRACE_READ`(`GETINFO` 的 region/堆摘要) 与
 *   §3 捕获集("寄存器组、trace 环、**内存 region 表**、插件表、arena 统计")+
 *   "约束: 捕获路径**只用静态缓冲**, 不碰堆/调度器"。
 *
 * 归属边界(为什么 dump 是"编排者"而不是又一份实现):
 *   - 呈现原语 → `service/hexdump`(本插件 dependency);
 *   - 栈回溯   → `service/backtrace`;
 *   - 环的消费 → `service/trace`;
 *   - 泄漏/红区账 → `service/memleak`;
 *   - **本插件自己只做三件事**: ① 按 region 表做边界判定(未声明区拒绝 dump);
 *     ② 把上面几件的输出编排成一份"现场"; ③ 自身状态的 LATE 相 init + 启动快照。
 *     ★ 调试域自检入口(ADR-0010 前 dump 对外发的那个 conformance 函数)已移出本头: 自检是
 *     **测试面**而不是插件能力, 进 golden 接口会让"改一条用例"变成接口变更。
 *
 * ISR 安全: 全部 **thread-only**(会打印、会走堆观测)。
 */
#ifndef BR_DEBUG_BR_DUMP_H
#define BR_DEBUG_BR_DUMP_H

#include <br/core/br_types.h>

/* br_dump_memory 的 flags(append-only) */
#define BR_DUMP_F_STRICT   0x0000u   /* 缺省: 未声明的地址 ⇒ -EINVAL(拒绝) */
#define BR_DUMP_F_FORCE    0x0001u   /* 越过 region 表边界判定(P4/machine 语义) */

/*
 * ★ `BR_DUMP_F_FORCE` 在 v1 的**确切**语义(裁定 + 理由, 见 ADR-0003 §2.4/§2.6):
 *   它绕过 region 表判定(不返回 `-EINVAL`), 但**不会真的去读未声明的地址** ——
 *   页表按 region 快照构造, 表外地址 invalid(例如 `0x42000000` 就在 RAM 窗口之外,
 *   是 `irq_conf.c` 用来制造翻译 fault 的锚点), 而呈现原语(hexdump)没有 extable
 *   保护的读路径(设计 `5-01 §2` 的 `MEMRD` 完整形态要它, v1 未交付)⇒ 真读一次就是
 *   一次不可恢复的 data abort, 会把"调试服务"变成"崩溃源"。
 *   因此 v1 的 FORCE = 打表头 + 如实标注"未读取", 返回 0; 要它真读需要
 *   extable 保护的 probe(回灌项)。
 */

/* 单次 dump 的字节上界(v1 静态预算; 防"一个 dump 打满 UART") */
#define BR_DUMP_MAX_BYTES  4096u

/* LATE 相 init: 注册本插件自己的 trace 事件名(供离线解码可达)。返回 0。
 * (不再兼"自检前置": 自检由插件管理器在全部 START 之后统一驱动, 见文件头。) */
int br_dump_init(void);

/*
 * 调试域自检入口(ADR-0010 前 dump 对外发的那个 conformance 函数)已拆掉, 不再是对外 API:
 *   - 各插件的用例在 `src/<short>_selftest.c` 里, 由描述符钩子 `.selftest` 挂出,
 *     **core 在一个 pass 里驱动**(`br_plugin_manager_selftest`);
 *   - dump 自己的用例(TC-DBG-030/031/032/100)搬到 `src/dump_selftest.c`, 入口名
 *     `dump_selftest` 由生成物按 symbol_prefix 推导;
 *   - 旧入口兼的"代调四家 init"编排一并移除: init 由 plugin_manager 按 `[[dep]]`
 *     拓扑序驱动(ADR-0005 裁定 G6: 非 0 rc ⇒ 停机)。
 * ★ 测试入口**不进**本插件的 golden 接口面 —— 否则改一条用例就成了对外接口变更
 *   (接口 hash 覆盖的正是声明面), 与"自检与生产分离"的裁定冲突。
 */

/* 打印 region 清单(设计 §3 捕获集的"内存 region 表"): 每行
 *   `[DUMP] region[<i>] kind=<name> attrs=0x<hex> base=0x<hex> size=<n>`
 * 并给一行 `[DUMP] regions=<n>`。返回打印的 region 条数。 */
br_u32 br_dump_regions(void);

/*
 * 按 region 表 dump 一段内存(表头 + hexdump)。语义:
 *   - 未声明该地址 ⇒ `-EINVAL`(除非 flags 含 BR_DUMP_F_FORCE);
 *   - 跨越两条 region ⇒ 只 dump 落在**首条 region** 内的部分(并打印截断提示);
 *   - len > BR_DUMP_MAX_BYTES ⇒ 截断到上界(并打印提示);
 *   - len == 0 ⇒ 只打印表头, 返回 0。
 * 返回实际 dump 的字节数, 或负 errno。
 */
int br_dump_memory(br_uintptr_t addr, br_size_t len, br_u32 flags);

/* 打印堆统计 + 池几何(设计 §3 的 "arena 统计"): 各一行 `[DUMP] heap ...`。 */
br_u32 br_dump_heap(void);

/* 取走并呈现 trace(转调 service/trace 的 report; `max_events` 语义同它)。返回取走条数。 */
br_u32 br_dump_trace(br_u32 max_events);

/* 捕获+打印当前栈回溯(转调 service/backtrace)。返回帧数。 */
br_u32 br_dump_backtrace(void);

/* 打印泄漏报告(转调 service/memleak)。返回存活块数。 */
br_u32 br_dump_leaks(void);

/* 一份完整现场: regions → heap → leaks → trace → backtrace, 首行
 * `[DUMP] ===== snapshot start =====`, 末行 `[DUMP] ===== snapshot end =====`。
 * 返回打印的行数(便于用例断言"确实全打了")。 */
br_u32 br_dump_all(void);

#endif /* BR_DEBUG_BR_DUMP_H */
