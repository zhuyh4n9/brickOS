/*
 * brickOS prototype v0.1.0 — fault(同步异常)路径: 分槽 / 分类 / extable / panic
 *
 * 权威设计: `docs/3-os-core/3-02-int.md` §10
 *   §10.3 EC 分类与决策表(**0x24/0x25 data abort 先查 extable**, 其余 fatal;
 *         0x07 FP/SIMD 陷阱必须**点名**);
 *   §10.4 extable fixup: 非法访存降级为"函数返回错误码"(改 ELR + 按表项写通用寄存器);
 *   §10.6 double fault 兜底: in_fault ≥ 2 ⇒ **不调用任何 handler**, 直接 bare panic;
 *         ★ 非致命退出路径**必须递减 in_fault**(漏了就毁掉 ramdump);
 *   §10.5 借 CPU-local 的 isr_nest/curr_virq 回答"哪个 ISR 踩了内存"。
 *
 * 结构纪律(§10.6 "用结构消除错误"): 入口一处 in_fault++, 出口一处 `goto OUT` 递减;
 * 致命路径(panic)**不递减** —— 它不返回。
 */
#include <br/core/br_console.h>
#include <br/core/br_fault.h>
#include <br/core/br_trace.h>

#include "irq_internal.h"

/*
 * extable 段由链接器收集(设计 §10.4: ".quad 表 + ELR 改写", 无运行期初始化)。
 * 段符号由链接脚本提供 —— 本原型当前 link.ld **尚未**收集 .br_extable,
 * 需在平台链接脚本里补 `__br_extable_start/__br_extable_stop`(见交付说明)。
 */
extern const br_extable_t __br_extable_start[];
extern const br_extable_t __br_extable_stop[];

/*
 * P-IRQ-2(实现裁定): 表项顺序 = **链接顺序**(段由链接器收集), 而"按 pc 有序 ⇒ 二分"
 * 要求一个排序点: 运行期在 fault 上下文排序是禁止的(会再 fault), 且 v0.1 没有
 * "链接期生成有序表"的工具链支撑 ⇒ 取**线性扫描**。表项数是个位数(两个已命名消费者:
 * debug bridge 的 MEMRD、驱动 probe 自检), 线性查找在 fault 路径上完全够用;
 * 表项增长后由 ISA 层在链接期生成有序表再换二分(设计 §10.4 的期望形态)。
 */
const br_extable_t *br_extable_lookup(br_uintptr_t pc)
{
    for (const br_extable_t *e = __br_extable_start; e < __br_extable_stop; e++) {
        if (e->pc == pc) {
            return e;
        }
    }
    return BR_NULL;
}

/* EC → 可读名字(§10.3 表; 未列出的 EC 由调用方打印原值) */
static const char *ec_name(br_u32 ec)
{
    switch (ec) {
    case BR_EC_UNKNOWN:    return "undefined-instruction";
    case BR_EC_FP_SIMD:    return "FP/SIMD trap (CPACR_EL1.FPEN off?)";
    case BR_EC_SVC64:      return "SVC64 (no syscall in single-EL)";
    case BR_EC_HVC64:      return "HVC64";
    case BR_EC_SMC64:      return "SMC64";
    case BR_EC_MSR_MRS:    return "MSR/MRS trap (trapped sysreg)";
    case BR_EC_IABT_LOWER: return "instruction abort (lower EL)";
    case BR_EC_IABT_SAME:  return "instruction abort (same EL)";
    case BR_EC_DABT_LOWER: return "data abort (lower EL)";
    case BR_EC_DABT_SAME:  return "data abort (same EL)";
    case BR_EC_BRK64:      return "BRK64 (breakpoint)";
    default:               return "unclassified";
    }
}

void br_fault_enter(br_exc_frame_t *f)
{
    br_irq_cpu_t *cpu = br_irq_cpu();

    cpu->in_fault++;

    const br_u32 ec = (br_u32)((f->esr >> BR_ESR_EC_SHIFT) & (br_u64)BR_ESR_EC_MASK);

    /*
     * §10.6 double fault: prev_* 此刻仍是**上一次** fault 的记录(下面的赋值只在
     * 非 double 路径发生), 这正是"handler 里又踩了同一块内存"最直接的线索。
     * 且**绝不调用任何 fault handler**(handler 本身就是上一次 fault 的嫌疑方)。
     */
    if (cpu->in_fault >= 2u) {
        br_trace_emit(BR_TRACE_FAULT_DOUBLE, (br_u32)cpu->in_fault, 0u);
        br_panic_bare("double fault -- in_handler=%u prev_ec=0x%x prev_elr=%p sp=%p vector=%u",
                      (br_u32)cpu->in_handler,
                      (br_u32)cpu->prev_fault_ec,
                      (void *)(br_uintptr_t)cpu->prev_fault_elr,
                      (void *)(br_uintptr_t)f->sp,
                      (br_u32)f->vector);
    }

    /* 每次(非 double)入口都更新"上一次 fault"; 留痕供 debug/ramdump 消费(§10.5/§10.6) */
    cpu->prev_fault_ec  = ec;
    cpu->prev_fault_elr = f->elr;
    br_trace_emit(BR_TRACE_FAULT, ec, (br_u64)f->esr);

    /* ---- §10.4: 只有 data abort(0x24/0x25)才先查 extable ---- */
    if (ec == (br_u32)BR_EC_DABT_LOWER || ec == (br_u32)BR_EC_DABT_SAME) {
        const br_extable_t *e = br_extable_lookup((br_uintptr_t)f->elr);
        if (e != BR_NULL) {
            const br_uintptr_t elr_original = (br_uintptr_t)f->elr;

            /* 命中 ⇒ 改 ELR 指向 fixup, 并按表项的 reg/errno_val 改写异常帧里的通用寄存器
             * ⇒ ERET 继续执行, fault 被降级为"函数返回错误码"(§10.4)。
             * reg == 31 表示 xzr(不写); 31..255 是保留值, 一律当"不写"处理。 */
            f->elr = (br_u64)e->fixup;
            if (e->reg < 31u) {
                f->x[e->reg] = (br_u64)(br_s64)e->errno_val;
            }
            br_trace_emit(BR_TRACE_FAULT_FIXUP, (br_u32)elr_original,
                          (br_u64)(br_s64)e->errno_val);
            goto OUT;
        }
    }

    /*
     * ---- 其余一律 fatal(§10.3)----
     * 报出"在哪种上下文崩的": isr_nest/curr_virq 回答"哪个 ISR 踩了内存"(§10.5)——
     * 否则排查要从全系统 ISR 里找。EC 未列入决策表时也把原值打出来, 不硬编码"未知"。
     */
    br_panic_bare("fatal fault: %s ec=0x%x esr=0x%lx far=%p elr=%p sp=%p vector=%u "
                  "in_isr=%u isr_nest=%u curr_virq=%u",
                  ec_name(ec), ec, (br_u64)f->esr,
                  (void *)(br_uintptr_t)f->far,
                  (void *)(br_uintptr_t)f->elr,
                  (void *)(br_uintptr_t)f->sp,
                  (br_u32)f->vector,
                  (br_u32)((cpu->isr_nest > 0u) ? 1u : 0u),
                  (br_u32)cpu->isr_nest,
                  (br_u32)cpu->curr_virq);

OUT:
    /*
     * ★ §10.6 的易错点: 只增不减会让"下一个完全无关的 fault"看到 in_fault == 2 ⇒
     *   被误判成 double fault ⇒ 走 bare panic(按设计不调用任何 handler)⇒
     *   **刚建好的 ramdump 被跳过**。所有非致命分支都汇聚到这里统一递减。
     *   (致命路径不返回, 不递减 —— 它不返回。)
     */
    cpu->in_fault--;
}
