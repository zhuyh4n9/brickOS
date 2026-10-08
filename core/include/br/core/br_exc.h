/*
 * brickOS prototype v0.1.0 — 异常帧布局与入口桩契约(aarch64)
 *
 * 设计依据: `docs/3-os-core/3-02-int.md` §5.2(IRQ 处理流程: asm 入口桩保存
 * caller-saved → 调 `br_irq_enter(frame)`)、§10.2(向量表分槽与屏蔽位处理)、
 * §14.2(异常帧归属)。
 *
 * ⚠ 设计侧本头的归属是 **ISA 共享库**(异常向量入口汇编的配套头; "core 不进它的
 *   字段")。本原型还没有独立的 ISA 共享库层(WORKAROUND(br-wa-isa-001)), 而
 *   **core 的 fault 路径必须能改写现场**(extable fixup: 改 ELR + 写通用寄存器,
 *   3-02 §10.4/§10.5), 所以帧布局必须由 core 与 asm 桩**共享同一处真值** ——
 *   放在 core 是"单层原型下消除两份易错布局"的选择; 抽出 ISA 库时本头随之搬家。
 *
 * 帧里放什么、为什么只放这么多(3-02 §5.2 的注释):
 *   v1 的 ISR 一律返回被打断的现场(不切上下文) ⇒ 帧里只需 caller-saved(x0–x30)
 *   与异常现场寄存器; callee-saved 由被调用的 C 函数按 AAPCS64 自行保存。
 *   **v2 若在 IRQ 出口切栈, 这个前提就变了**(§11.2) —— 那时要么帧里也放
 *   callee-saved, 要么明确"切栈点只发生在 C 出口之后、帧已无用"。
 */
#ifndef BR_CORE_BR_EXC_H
#define BR_CORE_BR_EXC_H

/* br_types.h 是 C 专属(typedef/静态断言)⇒ 汇编侧不拉它(否则 as 会报
 * "unknown mnemonic typedef")。下面的偏移宏是纯字面量, 汇编侧不需要类型。 */
#ifndef __ASSEMBLER__
#include <br/core/br_types.h>
#endif

/* ---- 帧内偏移(asm 桩与 C 结构**同一处真值**) ----
 * ★ 本头被 `vectors.S` 直接 `#include`(Makefile 的 ASFLAGS 带 `-Icore/include`,
 *   且 GCC 对 `.S` 自动定义 `__ASSEMBLER__`)⇒ **下面这些偏移宏必须保持
 *   汇编器可用的纯字面量**: 不要写成含 C 强制类型转换的表达式。
 *   C 专属部分(typedef/静态断言/x 偏移的表达式形式)在 `#ifndef __ASSEMBLER__` 里。 */
#define BR_EXC_SP_OFF       0x0F8u               /* 异常发生时的 SP(栈回溯必需) */
#define BR_EXC_ELR_OFF      0x100u
#define BR_EXC_SPSR_OFF     0x108u
#define BR_EXC_ESR_OFF      0x110u
#define BR_EXC_FAR_OFF      0x118u
#define BR_EXC_DAIF_OFF     0x120u               /* 进入时的 DAIF(供 §6.4 的 DAIF 恢复策略) */
#define BR_EXC_VECTOR_OFF   0x128u               /* 向量槽下标 0..15(诊断归因) */
#define BR_EXC_PAD_OFF      0x130u
#define BR_EXC_SIZE         0x140u               /* 320 B = 16 × 20: **16 字节对齐是 AAPCS64 硬要求** */

#define BR_EXC_XCOUNT       31u

/* x0..x30 的偏移: `8*n`(汇编侧用字面量, C 侧用这个表达式宏) */
#define BR_EXC_X_OFF(n)     (8u * (br_u32)(n))

/* aarch64 DAIF 位(PSTATE 里的位置; 与 br_irq_state_t 的位布局**不同**, 见 br_irq.h) */
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
    br_u64 pad[2];             /*             [0x130] 16 字节对齐填充 + append-only 预留 */
} br_exc_frame_t;

_Static_assert(sizeof(br_exc_frame_t) == BR_EXC_SIZE, "异常帧布局漂移");
_Static_assert((BR_EXC_SIZE % 16u) == 0u, "异常帧必须 16 字节对齐(AAPCS64)");
_Static_assert(BR_EXC_X_OFF(BR_EXC_XCOUNT) == BR_EXC_SP_OFF, "x[] 之后必须紧跟 sp");

#endif /* !__ASSEMBLER__ */

#endif /* BR_CORE_BR_EXC_H */
