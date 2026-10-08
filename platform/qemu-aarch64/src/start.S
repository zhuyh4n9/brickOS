/*
 * brickOS prototype v0.1.0 — Platform Entry (aarch64)
 *
 * ======================= WORKAROUND(br-wa-entry-001) =======================
 * 设计上这段汇编属于 **Platform 插件**(设计 1-01 §6.3: Platform 类 = reset 汇编、
 * 时钟/引脚/RAM、中断控制器实现、console、cache、timer、region 表; 每 SoC 一个)。
 * v0.1.0 没有 `br` 组合器与插件管理器, 所以它被 Makefile 直接编进镜像 —— 见
 * platform/include/br/platform/br_plat.h 顶部与 WORKAROUNDS.md 的退出条件。
 * ==========================================================================
 *
 * 与设计启动序列(1-01 §9)的对应:
 *   reset -> platform 向量表(汇编) + BSS 清零(汇编)   [本文件]
 *         -> platform.early_init                        [br_plat_early_init]
 *         -> core.init ...                              [br_core_main, 见 br-wa-boot-001]
 *
 * 本文件只做 5 件事, 不碰内存管理与中断控制器本体:
 *   1. 选 CPU(boot 核继续, 其余核 park)
 *   2. 屏蔽异常源(DAIF)+ 必要时从 EL2 降到 EL1h
 *   3. 装载启动栈 + 设向量表(表本体在 vectors.S)
 *   4. 清零 BSS
 *   5. 初始化 core 的 per-CPU 中断状态, 再调平台早期初始化, 最后交 core
 */

    .section .text.boot, "ax"
    .balign 16
    .global _start
    .type   _start, %function

_start:
    /* ---- 1. 只让 boot 核继续: MPIDR_EL1.Aff0 != 0 的核停在这里 ---- */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, .Lpark

    /* ---- 2a. 屏蔽全部异常源(D/A/I/F) ---- */
    msr     daifset, #0xf

    /* ---- 2b. 若从 EL2 进入(QEMU -machine virtualization=on), 降到 EL1h ----
     * 裸机 QEMU virt 缺省已给 EL1, 所以这段是"防御性"的:
     * 不判 CurrentEL 就直接写 SP_EL1 会在 EL2 上静默出错。 */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    .Lel_ready

    mov     x0, #(1 << 31)          /* HCR_EL2.RW = 1: EL1 按 AArch64 执行 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5              /* SPSR_EL2: DAIF 全屏蔽 + EL1h(M[3:0]=0101) */
    msr     spsr_el2, x0
    adr     x0, .Lel_ready
    msr     elr_el2, x0
    eret

.Lel_ready:
    /* ---- 3a. 选 SP_EL1 并装载启动栈(链接脚本提供 __stack_top) ---- */
    mov     x0, #1
    msr     spsel, #1
    ldr     x0, =__stack_top
    mov     sp, x0

    /* ---- 3b. 设异常向量表 ----
     * 表本体在 vectors.S(16 槽 × 0x80 B, 2 KiB 对齐; 0x200 = fault, 0x280 = IRQ)。
     * v0.1 在走进 core 之前不开中断, 但 fault/SError 仍可能来; 没有向量表就会
     * 跳到地址 0 静默挂死 —— 那是最难查的一种 bug。 */
    ldr     x0, =br_plat_vectors
    msr     vbar_el1, x0

    /* ---- 4. 清零 BSS(链接脚本保证 __bss_end - __bss_start 为 8 的倍数) ---- */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
.Lbss_loop:
    cmp     x0, x1
    b.hs    .Lbss_done
    str     xzr, [x0], #8
    b       .Lbss_loop
.Lbss_done:

    /* ---- 4b. core 的 per-CPU 中断状态初始化 ----
     * 设计侧这一步属 core.init; v0.1.0 没有 core.init(WORKAROUND br-wa-boot-001),
     * 所以由 Platform Entry 在 BSS 清零之后、br_plat_early_init() 之前调用。
     * 它把 TPIDR_EL1 指向本核的 br_irq_cpu_t —— 此后任何 IRQ 入口都要用(3-02 §14.4)。
     * 顺序硬约束(§14.3): 必须在 BSS 清零后(否则 CPU-local 结构被抹掉)。 */
    bl      br_irq_cpu_init

    /* ---- 5a. 平台早期初始化: console + PIC 注册 + 绑定表(失败即停机) ---- */
    bl      br_plat_early_init

    /* ---- 5b. 交 core: MainLoop(延时 + 日志), 不返回 ---- */
    bl      br_core_main

    /* 理论上到不了这里; 到了也按不可恢复处理 */
    b       br_plat_park_forever

    .size   _start, . - _start

/*
 * 异常向量表**不在这里** —— 实体在 vectors.S(16 槽 × 0x80 B, 2 KiB 对齐,
 * 0x200 = fault 桩, 0x280 = IRQ 桩, 其余 14 槽 = 真实可用的 panic 桩;
 * 设计 3-02 §10.2)。本文件只负责把它装进 VBAR_EL1(见 3b)。
 *
 * 其余核的停机位。放在 .text 尾(不进 .text.boot, 免得挤占入口布局)。
 */
    .section .text, "ax"
    .balign 4
.Lpark:
    wfe
    b       .Lpark
