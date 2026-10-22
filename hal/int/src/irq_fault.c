/*
 * Fault (synchronous exception) path: slot counting, EC classification, extable fixup, panic.
 * in_fault >= 2 = double fault => no handler is called, bare panic. Non-fatal exits must decrement
 * in_fault (a leaked count makes the next fault look like a double fault and skips the ramdump);
 * the fatal path never returns.
 */
#include <br/core/br_console.h>
#include <br/hal/br_fault.h>
#include <br/core/br_trace.h>

#include "irq_internal.h"

/* Extable section bounds are declared in irq_internal.h (ADR-0038 / R1: a `.c` carries no
 * cross-translation-unit declaration). This prototype's link.ld does not yet collect .br_extable;
 * that needs __br_extable_start/__br_extable_stop in the platform linker script. */

/* P-IRQ-2: entries are in link order. Runtime sorting is forbidden on the fault path, so lookup is
 * a linear scan; fine for the single-digit entry count. Once entries grow, generate a sorted table
 * at link time and switch to binary search. */
const br_extable_t *br_extable_lookup(br_uintptr_t pc)
{
    for (const br_extable_t *e = __br_extable_start; e < __br_extable_stop; e++) {
        if (e->pc == pc) {
            return e;
        }
    }
    return BR_NULL;
}

/* EC -> readable name; unlisted ECs are printed raw by the caller */
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

    /* Panic re-entry guard: a fault during panic output would recurse without bound and push
     * exception frames over the boot stack, losing the original fault site. Must run after
     * in_fault++ and before any tracing/print/handler call. */
    if (br_panic_in_progress()) {
        br_panic_halt();
    }

    const br_u32 ec = (br_u32)((f->esr >> BR_ESR_EC_SHIFT) & (br_u64)BR_ESR_EC_MASK);

    /* Double fault: prev_* still records the previous fault (the assignment below is skipped on
     * this path), and no handler is called -- the handler is itself a suspect for the previous
     * fault. */
    if (cpu->in_fault >= 2u) {
        br_trace_emit(BR_TRACE_FAULT_DOUBLE, (br_u32)cpu->in_fault, 0u);
        br_panic_bare("double fault -- in_handler=%u prev_ec=0x%x prev_elr=%p sp=%p vector=%u",
                      (br_u32)cpu->in_handler,
                      (br_u32)cpu->prev_fault_ec,
                      (void *)(br_uintptr_t)cpu->prev_fault_elr,
                      (void *)(br_uintptr_t)f->sp,
                      (br_u32)f->vector);
    }

    /* Non-double entries record the previous fault for ramdump/debug consumption. */
    cpu->prev_fault_ec  = ec;
    cpu->prev_fault_elr = f->elr;
    br_trace_emit(BR_TRACE_FAULT, ec, (br_u64)f->esr);

    /* Only data abort (0x24/0x25) checks extable first. */
    if (ec == (br_u32)BR_EC_DABT_LOWER || ec == (br_u32)BR_EC_DABT_SAME) {
        const br_extable_t *e = br_extable_lookup((br_uintptr_t)f->elr);
        if (e != BR_NULL) {
            const br_uintptr_t elr_original = (br_uintptr_t)f->elr;

            /* On hit, rewrite ELR to the fixup and write the GPR named by reg/errno_val, so ERET
             * resumes and the fault degrades to a returned error code. reg == 31 means xzr (no
             * write); 31..255 likewise. */
            f->elr = (br_u64)e->fixup;
            if (e->reg < 31u) {
                f->x[e->reg] = (br_u64)(br_s64)e->errno_val;
            }
            br_trace_emit(BR_TRACE_FAULT_FIXUP, (br_u32)elr_original,
                          (br_u64)(br_s64)e->errno_val);
            goto OUT;
        }
    }

    /* Everything else is fatal. isr_nest/curr_virq answer "which ISR faulted"; an EC outside the
     * table is printed raw rather than hardcoded as unknown. */
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
    /* All non-fatal branches converge here to decrement uniformly: a leaked count makes the next
     * unrelated fault look like a double fault and skips the freshly built ramdump. */
    cpu->in_fault--;
}
