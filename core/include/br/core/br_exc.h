/*
 * brickOS prototype v0.1.0 — exception frame layout and entry-stub contract (aarch64)
 *
 * Design basis: `docs/3-os-core/3-02-int.md` §5.2 (IRQ flow: the asm entry stub saves
 * caller-saved registers → calls `br_irq_enter(frame)`), §10.2 (vector slotting and mask-bit
 * handling), §14.2 (exception frame ownership).
 *
 * ⚠ In the design this header belongs to the **shared ISA library** (companion header for the
 *   exception vector entry assembly; "core does not enter its fields"). The prototype has no
 *   separate ISA library layer yet (WORKAROUND(br-wa-isa-001)), and **core's fault path must be
 *   able to rewrite the interrupted context** (extable fixup: change ELR + write general
 *   registers, 3-02 §10.4/§10.5), so frame layout must have **one single source of truth**
 *   shared by core and the asm stub — putting it in core removes two error-prone layouts in a
 *   single-layer prototype; this header moves when the ISA library is extracted.
 *
 * What the frame holds and why only this much (note in 3-02 §5.2):
 *   v1 ISRs always return to the interrupted context (no context switch) ⇒ the frame only needs
 *   caller-saved (x0–x30) and the exception status registers; callee-saved registers are saved
 *   by the called C function per AAPCS64.
 *   **If v2 switches stacks on IRQ exit this premise changes** (§11.2) — then either the frame
 *   must also hold callee-saved registers, or it must be stated that the switch happens only
 *   after the C return, when the frame is dead.
 */
#ifndef BR_CORE_BR_EXC_H
#define BR_CORE_BR_EXC_H

/* br_types.h is C-only (typedef/static asserts) ⇒ assembly must not pull it in (as would
 * report "unknown mnemonic typedef"). The offset macros below are pure literals; assembly
 * needs no types. */
#ifndef __ASSEMBLER__
#include <br/core/br_types.h>
#endif

/* ---- In-frame offsets (single source of truth for the asm stub and the C struct) ----
 * ★ This header is `#include`d directly by `vectors.S` (the Makefile's ASFLAGS carry
 *   `-Icore/include`, and GCC defines `__ASSEMBLER__` for `.S`) ⇒ **these offset macros must
 *   stay pure literals usable by the assembler**: do not write expressions with C casts.
 *   The C-only parts (typedefs/static asserts/expression forms of x offsets) are inside
 *   `#ifndef __ASSEMBLER__`. */
#define BR_EXC_SP_OFF       0x0F8u               /* SP at exception time (backtrace needs it) */
#define BR_EXC_ELR_OFF      0x100u
#define BR_EXC_SPSR_OFF     0x108u
#define BR_EXC_ESR_OFF      0x110u
#define BR_EXC_FAR_OFF      0x118u
#define BR_EXC_DAIF_OFF     0x120u               /* DAIF on entry (for §6.4 DAIF restore policy) */
#define BR_EXC_VECTOR_OFF   0x128u               /* vector slot index 0..15 (diagnosis) */
#define BR_EXC_PAD_OFF      0x130u
#define BR_EXC_SIZE         0x140u               /* 320 B = 16 × 20: **16-byte alignment is an AAPCS64 hard requirement** */

#define BR_EXC_XCOUNT       31u

/* Offset of x0..x30: `8*n` (literals in assembly, this expression macro in C) */
#define BR_EXC_X_OFF(n)     (8u * (br_u32)(n))

/* aarch64 DAIF bits (position in PSTATE; **different** from br_irq_state_t's layout, see br_irq.h) */
#define BR_DAIF_D           0x200u   /* [9] debug */
#define BR_DAIF_A           0x100u   /* [8] SError */
#define BR_DAIF_I           0x080u   /* [7] IRQ */
#define BR_DAIF_F           0x040u   /* [6] FIQ */

#ifndef __ASSEMBLER__

typedef struct br_exc_frame {
    br_u64 x[BR_EXC_XCOUNT];   /* x0 .. x30   [0x000] */
    br_u64 sp;                 /*             [0x0F8] */
    br_u64 elr;                /*             [0x100] */
    br_u64 spsr;               /*             [0x108] */
    br_u64 esr;                /*             [0x110] */
    br_u64 far;                /*             [0x118] */
    br_u64 daif;               /*             [0x120] */
    br_u64 vector;             /*             [0x128] */
    br_u64 pad[2];             /*             [0x130] 16-byte alignment padding + append-only reserve */
} br_exc_frame_t;

_Static_assert(sizeof(br_exc_frame_t) == BR_EXC_SIZE, "exception frame layout drift");
_Static_assert((BR_EXC_SIZE % 16u) == 0u, "exception frame must be 16-byte aligned (AAPCS64)");
_Static_assert(BR_EXC_X_OFF(BR_EXC_XCOUNT) == BR_EXC_SP_OFF, "sp must follow x[] immediately");

#endif /* !__ASSEMBLER__ */

#endif /* BR_CORE_BR_EXC_H */
