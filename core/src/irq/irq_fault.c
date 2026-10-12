/*
 * brickOS prototype v0.1.0 — fault (synchronous exception) path: slot counting / classification / extable / panic
 *
 * Authoritative design: `docs/3-os-core/3-02-int.md` §10
 *   §10.3 EC classification and decision table (**0x24/0x25 data abort checks extable first**, everything else fatal;
 *         0x07 FP/SIMD trap must be **named**);
 *   §10.4 extable fixup: illegal memory access degrades to "function returns error code" (rewrite ELR + write GPRs per entry);
 *   §10.6 double fault backstop: in_fault ≥ 2 ⇒ **call no handler**, bare panic directly;
 *         ★ the non-fatal exit path **must decrement in_fault** (missing it destroys the ramdump);
 *   §10.5 use CPU-local isr_nest/curr_virq to answer "which ISR tripped on memory".
 *
 * Structural discipline (§10.6 "eliminate errors with structure"): one in_fault++ at entry, one `goto OUT` decrement at exit;
 * the fatal path (panic) does **not** decrement — it does not return.
 */
#include <br/core/br_console.h>
#include <br/core/br_fault.h>
#include <br/core/br_trace.h>

#include "../panic_internal.h"
#include "irq_internal.h"

/*
 * The extable section is collected by the linker (design §10.4: ".quad table + ELR rewrite", no runtime init).
 * Section symbols come from the linker script — this prototype's link.ld does **not yet** collect .br_extable;
 * add `__br_extable_start/__br_extable_stop` in the platform linker script (see delivery notes).
 */
extern const br_extable_t __br_extable_start[];
extern const br_extable_t __br_extable_stop[];

/*
 * P-IRQ-2 (implementation ruling): entry order = **link order** (section collected by the linker), while
 * "ordered by pc ⇒ binary search" requires a sorting point: runtime sorting in fault context is forbidden
 * (it would fault again), and v0.1 has no toolchain support for "link-time generation of a sorted table"
 * ⇒ take a **linear scan**. The entry count is single digits (two named consumers: the debug bridge's
 * MEMRD and driver probe selftest), so a linear search is entirely sufficient on the fault path;
 * once entries grow, the ISA layer generates a sorted table at link time and switches to binary search
 * (the intended shape of design §10.4).
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

/* EC → readable name (§10.3 table; unlisted ECs are printed raw by the caller) */
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

    /*
     * ★ **panic re-entry guard** (found by measurement, ADR-0003 §5): a fault during panic output ⇒ silent halt.
     *
     * Why it is required: panic's output channel is the polling console, and "the console is unreachable too" is a common
     * crash shape. Without this guard, "panic prints → faults again → panic again → faults again" is
     * **unbounded recursion**, pushing a 0x140 B exception frame each time ⇒ the 64 KiB boot stack is eaten through and frames land in .bss
     * (measured: SP fell into page table `s_l2_dev`; once the page table was overwritten even instruction fetch could not translate) ⇒ **the original fault
     * site is lost entirely**. With it, behavior degrades to "if the first panic line prints, print it; otherwise halt quietly":
     * diagnostic value maximal, damage minimal.
     *
     * Position discipline: must be **after** `in_fault++` and **before** any tracing/print/handler call —
     * tracing writes the trace ring (which may itself be untrustworthy then), and printing is the re-entry source itself.
     */
    if (br_panic_in_progress()) {
        br_panic_halt();
    }

    const br_u32 ec = (br_u32)((f->esr >> BR_ESR_EC_SHIFT) & (br_u64)BR_ESR_EC_MASK);

    /*
     * §10.6 double fault: prev_* is still the record of the **previous** fault here (the assignment below only
     * happens on the non-double path), which is the most direct clue that "the handler tripped on the same memory again".
     * And **never call any fault handler** (the handler is itself a suspect for the previous fault).
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

    /* every (non-double) entry updates the "previous fault"; traced for debug/ramdump consumption (§10.5/§10.6) */
    cpu->prev_fault_ec  = ec;
    cpu->prev_fault_elr = f->elr;
    br_trace_emit(BR_TRACE_FAULT, ec, (br_u64)f->esr);

    /* ---- §10.4: only data abort (0x24/0x25) checks extable first ---- */
    if (ec == (br_u32)BR_EC_DABT_LOWER || ec == (br_u32)BR_EC_DABT_SAME) {
        const br_extable_t *e = br_extable_lookup((br_uintptr_t)f->elr);
        if (e != BR_NULL) {
            const br_uintptr_t elr_original = (br_uintptr_t)f->elr;

            /* Hit ⇒ rewrite ELR to the fixup and write the GPR named by the entry's reg/errno_val
             * ⇒ ERET continues and the fault degrades to "function returns error code" (§10.4).
             * reg == 31 means xzr (no write); 31..255 are reserved values, always treated as "no write". */
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
     * ---- everything else is fatal (§10.3) ----
     * Report "which context it crashed in": isr_nest/curr_virq answer "which ISR tripped on memory" (§10.5) —
     * otherwise diagnosis has to search every ISR in the system. If EC is not in the decision table, print the raw value too; do not hardcode "unknown".
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
     * ★ §10.6's easy mistake: incrementing without decrementing makes "the next entirely unrelated fault" see in_fault == 2 ⇒
     *   misjudged as double fault ⇒ bare panic (which by design calls no handler) ⇒
     *   **the freshly built ramdump is skipped**. All non-fatal branches converge here to decrement uniformly.
     *   (The fatal path does not return, does not decrement — it does not return.)
     */
    cpu->in_fault--;
}
