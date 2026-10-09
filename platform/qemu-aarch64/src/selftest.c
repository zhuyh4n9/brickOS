/*
 * brickOS prototype v0.2.0 — platform 插件的自检聚合(platform/qemu-aarch64/src/selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与核心代码分离,
 * 由 core 在"全部 start 之后、调度器接管之前"统一驱动)。
 *
 * ## 为什么这个文件是"聚合"而不是"实现"
 *   platform 的两套用例**早就**与生产代码分离了 —— 它们分别在:
 *     `src/irq_conf.c`  `br_plat_irq_conformance()`  中断子系统逐用例(TC-IRQ-… / GIC-*)
 *     `src/mm_conf.c`   `br_plat_mem_conformance()`  内存映射逐用例(TC-MEM-… / TC-MM-…)
 *   而这两个文件里**没有**生产代码(生产在 `board_irq.c`/`gicv3.c`/`memmap.c`/`mmu.c`)。
 *   所以本刀对 platform 要做的不是"搬测试", 而是**给它们一个描述符钩子** ——
 *   于是 core 能像驱动别的插件一样驱动它, 而不是靠 platform 在自己的 start 里代跑。
 *
 *   ★ 文件名/函数名的历史包袱: 两个文件叫 `*_conf.c`、入口叫 `*_conformance()`,
 *     与新的 `selftest` 词汇不完全一致。本刀**不改它们** —— 重命名会让
 *     `git log`/外部引用/门禁 tag 一起漂, 而它们已经是"测试与核心分离"的形态;
 *     两者的关系在本注释里说清即可(ADR-0010 §3 裁定 8)。
 *
 * ## 失败计数怎么合
 *   两个入口都返回**各自的失败项数**, 本函数返回两者之和 —— 与 core 的
 *   `br_plugin_manager_selftest()` 对插件所做的一致(它把每个钩子的返回值相加)。
 *   日志不动: 两套用例自己打 `[IRQCONF]`/`[MEMCONF]` 的 PASS/FAIL 与 SUMMARY,
 *   门禁(gates.toml)按那些行判红绿; 本函数只补一行"platform 自检合计"。
 */
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/platform/br_plat.h>

/* 与生产文件一致的钩子原型约定(名 = symbol_prefix + 相; 生成物引用本函数)。 */
int qemu_aarch64_selftest(void);

int qemu_aarch64_selftest(void)
{
    const int irq_fail = br_plat_irq_conformance();
    const int mem_fail = br_plat_mem_conformance();

    br_log_info("platform: selftest irq=%d mem=%d (failures)", irq_fail, mem_fail);
    return irq_fail + mem_fail;
}
