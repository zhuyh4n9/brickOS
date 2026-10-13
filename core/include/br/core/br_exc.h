/*
 * aarch64 exception frame layout and entry-stub contract. No separate ISA library layer yet
 * (WORKAROUND(br-wa-isa-001)), so this is the single source of truth shared by the asm stub and
 * core, which rewrites the interrupted context on extable fixup. v1 ISRs always
 * return to the interrupted context, so the frame holds only caller-saved x0-x30 and the status
 * registers; callee-saved registers are saved by the C callee per AAPCS64.
 */
#ifndef BR_CORE_BR_EXC_H
#define BR_CORE_BR_EXC_H

/* br_types.h is C-only (typedefs/static asserts); assembly must not pull it in. */
#ifndef __ASSEMBLER__
#include <br/core/br_types.h>
#endif

/* ---- In-frame offsets (single source of truth for the asm stub and the C struct) ----
 * vectors.S includes this header directly, so these macros must stay pure literals usable by
 * the assembler: no C-cast expressions. C-only parts live under #ifndef __ASSEMBLER__. */
#define BR_EXC_SP_OFF       0x0F8u               /* SP at exception time (backtrace needs it) */
#define BR_EXC_ELR_OFF      0x100u
#define BR_EXC_SPSR_OFF     0x108u
#define BR_EXC_ESR_OFF      0x110u
#define BR_EXC_FAR_OFF      0x118u
#define BR_EXC_DAIF_OFF     0x120u               /* DAIF on entry (DAIF restore policy) */
#define BR_EXC_VECTOR_OFF   0x128u               /* vector slot index 0..15 (diagnosis) */
#define BR_EXC_PAD_OFF      0x130u
#define BR_EXC_SIZE         0x140u               /* 320 B = 16 x 20; AAPCS64 16-byte alignment */

#define BR_EXC_XCOUNT       31u

/* Offset of x0..x30: `8*n` (literal in assembly, expression macro in C) */
#define BR_EXC_X_OFF(n)     (8u * (br_u32)(n))

/* aarch64 DAIF bits (position in PSTATE; layout differs from br_irq_state_t, see br_irq.h) */
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
