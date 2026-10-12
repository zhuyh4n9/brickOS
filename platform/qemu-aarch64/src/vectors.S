/*
 * brickOS prototype v0.1.0 — aarch64 exception vector table + entry save/restore stubs
 *
 * Design basis (authoritative): `docs/3-os-core/3-02-int.md`
 *   §5.2  interrupt handling flow: the asm entry stub saves caller-saved only → calls br_irq_enter(frame)
 *   §10.2 vector table split into 16 slots by (exception type × source); **the four EL1t slots must also be
 *         real usable stubs** (the first exception after reset may land in EL1t, and an empty slot becomes an
 *         exception loop); **fault stubs must explicitly mask all of DAIF** (`msr daifset, #0xf`) —— PSTATE.I
 *         does not mask SError(A) or debug(D), and whether a synchronous exception sets it automatically is
 *         "to be verified" on hardware, so neither claim is relied upon.
 *   §5.3/§10.2 fault and IRQ share **the same save/restore assembly** (the only difference is the extra
 *         `msr daifset, #0xf` on the fault path), to avoid two error-prone copies.
 *
 * The **single source of truth** for the frame layout: `core/include/br/core/br_exc.h` (design §14.2).
 * This file `#include`s it directly — the header's offset macros are pure literals (usable by the assembler),
 * and the C-only parts (typedefs/static asserts) are inside `#ifndef __ASSEMBLER__`; the Makefile ASFLAGS carry
 * `-Icore/include`, and GCC defines `__ASSEMBLER__` automatically for `.S`.
 *
 * ★ Why each slot holds only a "trampoline" rather than the full stub (implementation ruling P-IRQ-ASM-1):
 *   Each slot is hard-limited to 0x80 B (fixed by the architecture; the table must be contiguous), while saving/
 *   restoring 31 GPRs + 6 system registers + eret is about 60+ instructions (> 240 B), physically too large for
 *   one slot. ⇒ each slot holds `b <stub body for this slot>`, and the body is expanded **once per slot** by the
 *   same `.macro` (no fallthrough), placed in the same `.text.vectors` section after the vector table — the
 *   "one instance per slot" discipline holds. The trampoline is only 4 B, keeping all 16 slots on the 0x80 grid.
 *
 * ★ Why DAIF is not written back on restore (implementation ruling P-IRQ-ASM-2):
 *   AArch64 can only **write** PSTATE.DAIF with `daifset/daifclr`; `MSR DAIF, Xt` does not exist
 *   (`MRS Xt, DAIF` can read). DAIF is restored **atomically** by `eret` from SPSR_EL1 ——
 *   and this path never unmasks anything, so there is no "restore window". The daif field is still saved in the
 *   frame (br_exc.h layout + design §6.4 restore policy need it), it is just not written back from here.
 *
 * ★ Why only caller-saved (x0–x30) is saved and not callee-saved (§5.2 original note):
 *   v1 ISRs always **return to the interrupted site** (no context switch), and called C functions save the
 *   callee-saved registers they use per AAPCS64 ⇒ the frame needs only caller-saved and exception-state registers.
 *   **⚠ v2 duty of care**: if v2 switches stacks on IRQ exit (design §11.2's empty hook), this premise changes —
 *   then either put callee-saved in the frame too, or state explicitly "the stack switch happens only after the C
 *   return, when the frame is already dead". Read that §5.2 comment before changing this assembly.
 */
#include <br/core/br_exc.h>

/* WORKAROUND(br-wa-isa-001): in the design this file belongs to the **ISA shared library** (exception vector entry
 * assembly); the prototype has no such layer yet, so it sits with the platform plugin — see WORKAROUNDS.md. */
    .section .text.vectors, "ax"

/* =====================================================================
 * Slot macros
 * ===================================================================== */

/*
 * Non-handling slots (14): real usable stubs — put the slot index in x0 and hand off to the platform bare landing point.
 * ★ Deliberately no dependency on anything that may be corrupted (design §8.3.1): only `mov` + `b`.
 * ★ The four EL1t slots go here too: the first exception after reset may land in EL1t (SPSel not yet 1, §10.2);
 *   exception entry selects SP_ELx (x = target EL), but if SP_EL1 is not yet loaded the C path is still unusable —
 *   this is the inherent limit of "extremely early exception in the reset window", recorded in the implementation ruling.
 */
.macro BR_VEC_PANIC idx
    .balign 0x80
    mov     x0, #\idx
    b       br_plat_unexpected_exception
.endm

/*
 * Handling-slot trampoline: jump to this slot's own stub body (see P-IRQ-ASM-1 at the top of the file).
 */
.macro BR_VEC_ENTER body
    .balign 0x80
    b       \body
.endm

/* =====================================================================
 * Vector table: 16 slots × 0x80 B = 2 KiB, VBAR_EL1 requires 2 KiB alignment
 * Slot order is architecture-fixed: high 2 bits = source, low 2 bits = type (Sync/IRQ/FIQ/SError)
 * ===================================================================== */
    .balign 2048
    .global br_plat_vectors
    .type   br_plat_vectors, %object
br_plat_vectors:
    BR_VEC_PANIC 0                  /* 0x000 Current EL/SP_EL0: Synchronous */
    BR_VEC_PANIC 1                  /* 0x080 Current EL/SP_EL0: IRQ         */
    BR_VEC_PANIC 2                  /* 0x100 Current EL/SP_EL0: FIQ         */
    BR_VEC_PANIC 3                  /* 0x180 Current EL/SP_EL0: SError      */
    BR_VEC_ENTER br_exc_body_fault  /* 0x200 Current EL/SP_ELx: Synchronous → fault (§10.3) */
    BR_VEC_ENTER br_exc_body_irq    /* 0x280 Current EL/SP_ELx: IRQ         → interrupt (§5.2) */
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

    /* Pad the whole table out to 2 KiB before the stub bodies: ensures 0x000..0x7FF all belongs to the vector table
     * (otherwise slot 15's 0x80 range would be occupied by the stub bytes right after it —— harmless functionally,
     *  but the architectural constraint "table = 2 KiB" would no longer hold). */
    .balign 2048
    .size   br_plat_vectors, . - br_plat_vectors

/* =====================================================================
 * Save/restore stub bodies (one instance per slot, not shared, no fallthrough)
 *
 * Inputs: \label this slot's own label; \idx slot index; \is_fault 1 = fault path / 0 = IRQ path
 *
 * Save (order is correctness):
 *   1) sub sp, sp, #BR_EXC_SIZE —— frame is 16-byte aligned (hard AAPCS64 requirement)
 *   2) stp x0..x30 —— caller-saved; x30 = LR, stored separately with str
 *   3) interrupted SP = sp **after adding back** (sp then points at the frame bottom)
 *   4) elr/spsr/esr/far/daif/slot index/pad
 *   5) fault path: `msr daifset, #0xf` (**required**, see §10.2 at the top of the file)
 *   6) mov x0, sp; bl br_fault_enter / br_irq_enter
 *
 * Restore: system registers (x9 as the only scratch) → x0..x30 → raise stack → eret
 *   ★ There is **no** `msr daif, x9` here: see P-IRQ-ASM-2 at the top of the file.
 *   ★ Restoring esr/far is part of "fully restoring the site" (they do not take part in eret, but are frame semantics).
 *
 * The offset of x0..x30 = 8*n (i.e. BR_EXC_X_OFF(n)); literals are written here because
 * BR_EXC_X_OFF contains a C cast and can only be used on the C side (see the note in br_exc.h).
 * ===================================================================== */
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
    /* ★ A synchronous exception does **not depend on whether hardware sets it**: mask all of DAIF (§10.2).
     *   I does not cover SError(A) or debug(D); SError re-entry would overwrite the site being saved.
     *   The IRQ path omits this: hardware already set it on exception entry, and v1 does not nest (§7.2). */
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

/* ---- 0x200 synchronous exception → fault path (§10.3) ---- */
BR_EXC_STUB_BODY br_exc_body_fault, 4, 1

/* ---- 0x280 IRQ → interrupt path (§5.2) ---- */
BR_EXC_STUB_BODY br_exc_body_irq,   5, 0
