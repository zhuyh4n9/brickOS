/*
 * aarch64 exception vector table + entry save/restore stubs (P-IRQ-ASM-1, P-IRQ-ASM-2).
 * Frame layout lives in core/c_core/include/br/core/br_exc.h (offset macros are assembler-safe).
 * Each 0x80 B slot holds only a trampoline to its own stub body (a full 31-GPR + system-register
 * save/restore does not fit in a slot); fault and IRQ share the body, the fault path adding
 * msr daifset, #0xf. Only caller-saved x0..x30 are saved: v1 ISRs return to the interrupted site.
 * DAIF is not written back (no MSR DAIF; eret restores PSTATE from SPSR_EL1). v2: if IRQ exit
 * switches stacks, revisit this premise.
 */
#include <br/hal/br_exc.h>

/* WORKAROUND(br-wa-isa-001): in the design this file belongs to the ISA shared library (exception
 * vector entry assembly); the prototype has no such layer, so it sits with the platform plugin -
 * see WORKAROUNDS.md. */
    .section .text.vectors, "ax"

/* ---- slot macros ---- */

/*
 * Non-handling slots: real usable stubs - slot index in x0, then the platform bare landing point.
 * Only mov + b, no dependency on anything that may be corrupted. The four EL1t slots go here too
 * (the first exception after reset may land in EL1t, where the C path is still unusable).
 */
.macro BR_VEC_PANIC idx
    .balign 0x80
    mov     x0, #\idx
    b       br_plat_unexpected_exception
.endm

/* Handling-slot trampoline: jump to this slot's own stub body. */
.macro BR_VEC_ENTER body
    .balign 0x80
    b       \body
.endm

/* Vector table: 16 slots x 0x80 B = 2 KiB, 2 KiB aligned (VBAR_EL1 requirement). Slot order is
 * architecture-fixed: high 2 bits = source, low 2 bits = type (Sync/IRQ/FIQ/SError). */
    .balign 2048
    .global br_plat_vectors
    .type   br_plat_vectors, %object
br_plat_vectors:
    BR_VEC_PANIC 0                  /* 0x000 Current EL/SP_EL0: Synchronous */
    BR_VEC_PANIC 1                  /* 0x080 Current EL/SP_EL0: IRQ         */
    BR_VEC_PANIC 2                  /* 0x100 Current EL/SP_EL0: FIQ         */
    BR_VEC_PANIC 3                  /* 0x180 Current EL/SP_EL0: SError      */
    BR_VEC_ENTER br_exc_body_fault  /* 0x200 Current EL/SP_ELx: Synchronous -> fault */
    BR_VEC_ENTER br_exc_body_irq    /* 0x280 Current EL/SP_ELx: IRQ         -> interrupt */
    BR_VEC_PANIC 6                  /* 0x300 Current EL/SP_ELx: FIQ         */
    BR_VEC_PANIC 7                  /* 0x380 Current EL/SP_ELx: SError      */
    BR_VEC_PANIC 8                  /* 0x400 Lower EL AArch64: Synchronous  */
    BR_VEC_PANIC 9                  /* 0x480 Lower EL AArch64: IRQ          */
    BR_VEC_PANIC 10                 /* 0x500 Lower EL AArch64: FIQ          */
    BR_VEC_PANIC 11                 /* 0x580 Lower EL AArch64: SError       */
    BR_VEC_PANIC 12                 /* 0x600 Lower EL AArch32: Synchronous  */
    BR_VEC_PANIC 13                 /* 0x680 Lower EL AArch32: IRQ          */
    BR_VEC_PANIC 14                 /* 0x700 Lower EL AArch32: FIQ          */
    BR_VEC_PANIC 15                 /* 0x780 Lower EL AArch32: SError       */

    /* Pad the table out to 2 KiB before the stub bodies, so 0x000..0x7FF is all vector table. */
    .balign 2048
    .size   br_plat_vectors, . - br_plat_vectors

/* Save/restore stub bodies (one instance per slot, no fallthrough).
 * Save order is correctness: 1) sub sp, sp, #BR_EXC_SIZE (frame 16-byte aligned, AAPCS64)
 * 2) stp x0..x30 (x30 = LR, stored separately) 3) interrupted SP = sp + BR_EXC_SIZE
 * 4) elr/spsr/esr/far/daif/slot index/pad 5) fault path: msr daifset, #0xf
 * 6) mov x0, sp; bl br_fault_enter / br_irq_enter.
 * Restore: system registers (x9 is the only scratch) -> x0..x30 -> raise stack -> eret. No msr daif
 * here (P-IRQ-ASM-2); esr/far are restored as frame semantics. x-offset = 8*n (literals, because
 * BR_EXC_X_OFF contains a C cast usable only on the C side). */
.macro BR_EXC_STUB_BODY label, idx, is_fault
    .balign 16
    .type   \label, %function
\label:
    /* ---- save ---- */
    sub     sp, sp, #BR_EXC_SIZE
    stp     x0,  x1,  [sp, #0x00]      /* BR_EXC_X_OFF(0..1)   */
    stp     x2,  x3,  [sp, #0x10]      /* BR_EXC_X_OFF(2..3)   */
    stp     x4,  x5,  [sp, #0x20]      /* BR_EXC_X_OFF(4..5)   */
    stp     x6,  x7,  [sp, #0x30]      /* BR_EXC_X_OFF(6..7)   */
    stp     x8,  x9,  [sp, #0x40]      /* BR_EXC_X_OFF(8..9)   */
    stp     x10, x11, [sp, #0x50]      /* BR_EXC_X_OFF(10..11) */
    stp     x12, x13, [sp, #0x60]      /* BR_EXC_X_OFF(12..13) */
    stp     x14, x15, [sp, #0x70]      /* BR_EXC_X_OFF(14..15) */
    stp     x16, x17, [sp, #0x80]      /* BR_EXC_X_OFF(16..17) */
    stp     x18, x19, [sp, #0x90]      /* BR_EXC_X_OFF(18..19) */
    stp     x20, x21, [sp, #0xA0]      /* BR_EXC_X_OFF(20..21) */
    stp     x22, x23, [sp, #0xB0]      /* BR_EXC_X_OFF(22..23) */
    stp     x24, x25, [sp, #0xC0]      /* BR_EXC_X_OFF(24..25) */
    stp     x26, x27, [sp, #0xD0]      /* BR_EXC_X_OFF(26..27) */
    stp     x28, x29, [sp, #0xE0]      /* BR_EXC_X_OFF(28..29) */
    str     x30,      [sp, #0xF0]      /* BR_EXC_X_OFF(30)     */

    add     x9, sp, #BR_EXC_SIZE       /* after adding back = interrupted SP */
    str     x9, [sp, #BR_EXC_SP_OFF]

    mrs     x9, elr_el1
    str     x9, [sp, #BR_EXC_ELR_OFF]
    mrs     x9, spsr_el1
    str     x9, [sp, #BR_EXC_SPSR_OFF]
    mrs     x9, esr_el1
    str     x9, [sp, #BR_EXC_ESR_OFF]
    mrs     x9, far_el1
    str     x9, [sp, #BR_EXC_FAR_OFF]
    mrs     x9, daif                    /* DAIF at entry (before the daifset below) */
    str     x9, [sp, #BR_EXC_DAIF_OFF]
    mov     x9, #\idx
    str     x9, [sp, #BR_EXC_VECTOR_OFF]
    str     xzr, [sp, #BR_EXC_PAD_OFF]
    str     xzr, [sp, #BR_EXC_PAD_OFF + 8]

.if \is_fault
    /* A synchronous exception must mask all of DAIF (I does not cover SError/debug; SError re-entry
     * would overwrite the site being saved). The IRQ path omits it: hardware set it on entry and v1
     * does not nest. */
    msr     daifset, #0xf
.endif

    mov     x0, sp
.if \is_fault
    bl      br_fault_enter
.else
    bl      br_irq_enter
.endif

    /* ---- restore ---- */
    ldr     x9, [sp, #BR_EXC_ELR_OFF]
    msr     elr_el1, x9
    ldr     x9, [sp, #BR_EXC_SPSR_OFF]  /* eret restores PSTATE (including DAIF) from here */
    msr     spsr_el1, x9
    ldr     x9, [sp, #BR_EXC_ESR_OFF]
    msr     esr_el1, x9
    ldr     x9, [sp, #BR_EXC_FAR_OFF]
    msr     far_el1, x9

    ldp     x0,  x1,  [sp, #0x00]
    ldp     x2,  x3,  [sp, #0x10]
    ldp     x4,  x5,  [sp, #0x20]
    ldp     x6,  x7,  [sp, #0x30]
    ldp     x8,  x9,  [sp, #0x40]
    ldp     x10, x11, [sp, #0x50]
    ldp     x12, x13, [sp, #0x60]
    ldp     x14, x15, [sp, #0x70]
    ldp     x16, x17, [sp, #0x80]
    ldp     x18, x19, [sp, #0x90]
    ldp     x20, x21, [sp, #0xA0]
    ldp     x22, x23, [sp, #0xB0]
    ldp     x24, x25, [sp, #0xC0]
    ldp     x26, x27, [sp, #0xD0]
    ldp     x28, x29, [sp, #0xE0]
    ldr     x30,      [sp, #0xF0]

    add     sp, sp, #BR_EXC_SIZE
    eret
    .size   \label, . - \label
.endm

/* ---- 0x200 synchronous exception -> fault path ---- */
BR_EXC_STUB_BODY br_exc_body_fault, 4, 1

/* ---- 0x280 IRQ -> interrupt path ---- */
BR_EXC_STUB_BODY br_exc_body_irq,   5, 0
