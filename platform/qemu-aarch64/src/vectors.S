/*
 * brickOS prototype v0.1.0 — aarch64 异常向量表 + 入口保存/恢复桩
 *
 * 设计依据(权威): `docs/3-os-core/3-02-int.md`
 *   §5.2  中断处理流程: asm 入口桩只保存 caller-saved → 调 br_irq_enter(frame)
 *   §10.2 向量表按 (异常类型 × 来源) 分 16 槽; **EL1t 四槽也必须是真实可用的桩**
 *         (复位后首个异常可能落在 EL1t, 空槽会变成异常循环); **fault 桩必须显式
 *         全屏蔽 DAIF**(`msr daifset, #0xf`)—— PSTATE.I 不屏蔽 SError(A)与
 *         debug(D), 而同步异常是否自动置位在硬件上"待核实", 故不依赖任一说法。
 *   §5.3/§10.2 fault 与 IRQ 两条路径**共用同一份保存/恢复汇编**(唯一差异 =
 *         fault 路径多一条 `msr daifset, #0xf`), 减少两份易错汇编。
 *
 * 帧布局的**唯一真值**: `core/include/br/core/br_exc.h`(设计 §14.2)。
 * 本文件直接 `#include` 它 —— 该头的偏移宏是纯字面量(汇编器可用), C 专属部分
 * (typedef/静态断言)在 `#ifndef __ASSEMBLER__` 内; Makefile 的 ASFLAGS 带
 * `-Icore/include`, 且 GCC 对 `.S` 自动定义 `__ASSEMBLER__`。
 *
 * ★ 槽内为什么只放"跳板"而不是完整桩(实现裁定 P-IRQ-ASM-1):
 *   每槽硬性 0x80 B(架构固定, 表必须连续), 而 31 个 GPR + 6 个系统寄存器 + eret
 *   的保存/恢复约 60+ 条指令(> 240 B), 物理上放不进一槽。⇒ 槽内放
 *   `b <本槽专属的桩体>`, 桩体由同一个 `.macro` **每槽各展开一份**(互不 fallthrough),
 *   放在向量表之后同一 `.text.vectors` 段内 —— "一槽一份实例"的纪律不破。
 *   跳板只有 4 B, 保证 16 槽严格落在 0x80 网格上。
 *
 * ★ 为什么恢复时不写回 DAIF(实现裁定 P-IRQ-ASM-2):
 *   AArch64 只有 `daifset/daifclr` 能**写** PSTATE.DAIF, 不存在 `MSR DAIF, Xt`
 *   (`MRS Xt, DAIF` 可读)。DAIF 的恢复由 `eret` 从 SPSR_EL1 **原子**完成 ——
 *   且本路径上从不解除屏蔽, 所以没有"恢复窗口"。帧里的 daif 字段照存
 *   (br_exc.h 布局 + 设计 §6.4 的恢复策略要用), 只是不从这里写回。
 *
 * ★ 为什么只存 caller-saved(x0–x30)而不存 callee-saved(§5.2 原注):
 *   v1 的 ISR 一律**返回被打断的现场**(不切上下文), 被调用的 C 函数按 AAPCS64
 *   自行保存它用到的 callee-saved 寄存器 ⇒ 帧里只需 caller-saved 与异常现场寄存器。
 *   **⚠ v2 的注意义务**: 若 v2 在 IRQ 出口切栈(设计 §11.2 的空钩子), 这个前提就变了 ——
 *   那时要么帧里也放 callee-saved, 要么明确"切栈点只发生在 C 出口之后、帧已无用"。
 *   改这段汇编前请先读 §5.2 的那段注释。
 */
#include <br/core/br_exc.h>

/* WORKAROUND(br-wa-isa-001): 本文件属设计里的 **ISA 共享库**(异常向量入口汇编);
 * 原型还没有该独立层, 故与 platform 插件同目录 —— 见 WORKAROUNDS.md。 */
    .section .text.vectors, "ax"

/* =====================================================================
 * 槽级宏
 * ===================================================================== */

/*
 * 非处理槽(14 个): 真实可用的桩 —— 把槽下标放进 x0 后交给平台 bare 落点。
 * ★ 刻意不依赖任何可能已损坏的设施(设计 §8.3.1): 只 `mov` + `b`。
 * ★ EL1t 四槽也走这里: 复位后首个异常可能落在 EL1t(SPSel 还没切到 1, §10.2);
 *   异常入口会选 SP_ELx(x = 目标 EL), 但若 SP_EL1 尚未装载则 C 路径仍不可用 ——
 *   这是"复位窗口内极早异常"的固有极限, 记录在实现裁定里。
 */
.macro BR_VEC_PANIC idx
    .balign 0x80
    mov     x0, #\idx
    b       br_plat_unexpected_exception
.endm

/*
 * 处理槽跳板: 转入本槽专属的桩体(见文件头 P-IRQ-ASM-1 的说明)。
 */
.macro BR_VEC_ENTER body
    .balign 0x80
    b       \body
.endm

/* =====================================================================
 * 向量表: 16 槽 × 0x80 B = 2 KiB, VBAR_EL1 要求 2 KiB 对齐
 * 槽序由架构固定: 高 2 位 = 来源, 低 2 位 = 类型(Sync/IRQ/FIQ/SError)
 * ===================================================================== */
    .balign 2048
    .global br_plat_vectors
    .type   br_plat_vectors, %object
br_plat_vectors:
    BR_VEC_PANIC 0                  /* 0x000 Current EL/SP_EL0: Synchronous */
    BR_VEC_PANIC 1                  /* 0x080 Current EL/SP_EL0: IRQ         */
    BR_VEC_PANIC 2                  /* 0x100 Current EL/SP_EL0: FIQ         */
    BR_VEC_PANIC 3                  /* 0x180 Current EL/SP_EL0: SError      */
    BR_VEC_ENTER br_exc_body_fault  /* 0x200 Current EL/SP_ELx: Synchronous → fault(§10.3) */
    BR_VEC_ENTER br_exc_body_irq    /* 0x280 Current EL/SP_ELx: IRQ         → 中断(§5.2)  */
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

    /* 把整张表补满到 2 KiB 再放桩体: 保证 0x000..0x7FF 全部属于向量表
     * (否则槽 15 的 0x80 区间会被紧随其后的桩体字节占据 —— 功能上无害,
     *  但"表 = 2 KiB"这条架构约束就名不副实了)。 */
    .balign 2048
    .size   br_plat_vectors, . - br_plat_vectors

/* =====================================================================
 * 保存/恢复桩体(每槽一份实例, 不共享、不 fallthrough)
 *
 * 入参: \label 本槽专属标签; \idx 槽下标; \is_fault 1 = fault 路径 / 0 = IRQ 路径
 *
 * 保存(顺序即正确性):
 *   1) sub sp, sp, #BR_EXC_SIZE —— 帧 16 字节对齐(AAPCS64 硬要求)
 *   2) stp x0..x30 —— caller-saved; x30 = LR, 用 str 单独存
 *   3) 被中断的 SP = **加回去之后**的 sp(此时 sp 指向帧底)
 *   4) elr/spsr/esr/far/daif/槽号/pad
 *   5) fault 路径: `msr daifset, #0xf`(**必须**, 见文件头 §10.2)
 *   6) mov x0, sp; bl br_fault_enter / br_irq_enter
 *
 * 恢复: 系统寄存器(用 x9 作唯一 scratch)→ x0..x30 → 抬栈 → eret
 *   ★ 这里**没有** `msr daif, x9`: 见文件头 P-IRQ-ASM-2。
 *   ★ 恢复 esr/far 是"完整还原现场"的一部分(它们不参与 eret, 但属于帧的语义)。
 *
 * x0..x30 的偏移 = 8*n(即 BR_EXC_X_OFF(n)); 这里写字面量是因为
 * BR_EXC_X_OFF 含 C 强制类型转换, 只能给 C 侧用(见 br_exc.h 的说明)。
 * ===================================================================== */
.macro BR_EXC_STUB_BODY label, idx, is_fault
    .balign 16
    .type   \label, %function
\label:
    /* ---- 保存 ---- */
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

    add     x9, sp, #BR_EXC_SIZE       /* 抬回后 = 被中断的 SP */
    str     x9, [sp, #BR_EXC_SP_OFF]

    mrs     x9, elr_el1
    str     x9, [sp, #BR_EXC_ELR_OFF]
    mrs     x9, spsr_el1
    str     x9, [sp, #BR_EXC_SPSR_OFF]
    mrs     x9, esr_el1
    str     x9, [sp, #BR_EXC_ESR_OFF]
    mrs     x9, far_el1
    str     x9, [sp, #BR_EXC_FAR_OFF]
    mrs     x9, daif                    /* 进入时的 DAIF(在下面 daifset 之前) */
    str     x9, [sp, #BR_EXC_DAIF_OFF]
    mov     x9, #\idx
    str     x9, [sp, #BR_EXC_VECTOR_OFF]
    str     xzr, [sp, #BR_EXC_PAD_OFF]
    str     xzr, [sp, #BR_EXC_PAD_OFF + 8]

.if \is_fault
    /* ★ 同步异常**不依赖硬件是否置位**: 全屏蔽 DAIF(§10.2)。
     *   I 不覆盖 SError(A)与 debug(D); SError 重入会改写正要保存的现场。
     *   IRQ 路径不加这条: 硬件进入异常时已置位, 且 v1 不嵌套(§7.2)。 */
    msr     daifset, #0xf
.endif

    mov     x0, sp
.if \is_fault
    bl      br_fault_enter
.else
    bl      br_irq_enter
.endif

    /* ---- 恢复 ---- */
    ldr     x9, [sp, #BR_EXC_ELR_OFF]
    msr     elr_el1, x9
    ldr     x9, [sp, #BR_EXC_SPSR_OFF]  /* eret 从这里恢复 PSTATE(含 DAIF) */
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

/* ---- 0x200 同步异常 → fault 路径(§10.3) ---- */
BR_EXC_STUB_BODY br_exc_body_fault, 4, 1

/* ---- 0x280 IRQ → 中断路径(§5.2) ---- */
BR_EXC_STUB_BODY br_exc_body_irq,   5, 0
