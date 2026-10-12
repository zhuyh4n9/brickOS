/*
 * brickOS prototype v0.2.0 — Platform Entry (aarch64)
 *
 * ======================= Paid off (ADR-0005/0008) · boot chain =======================
 * By design this assembly belongs to the **Platform plugin** (design 1-01 §6.3: Platform class = reset
 * assembly, clock/pins/RAM, interrupt controller implementation, console, cache, timer, region table;
 * one per SoC). Pluginization is done (manifest + descriptor + `[build].target` supply the target facts),
 * and this file stays inside the platform plugin directory per design.
 *
 * ★ **This file no longer calls any subsystem directly, nor the plugin manager directly**: after
 *   reset/BSS it only `bl`s core's entry `br_core_main()` (core/src/main.c). The four-phase boot chain
 *   (platform-independent init → platform plugin init → heap and address mapping → plugin phase drive)
 *   is orchestrated entirely on the core side, see `core/include/br/core/br_main.h` and
 *   `docs/decisions/0008-core-main-boot-chain.md`.
 * ==========================================================================
 *
 * Correspondence with the design boot sequence (1-01 §9):
 *   reset -> vector table (asm) + BSS clear (asm)              [this file]
 *         -> core's main (br_core_main, core/src/main.c)      [this file just jumps there]
 *              ① core platform-independent init (clock/log/interrupt framework/plugin manager section scan)
 *              ② platform plugin init (console / PIC / region table / page table ops)
 *              ③ core modules depending on the platform implementation (heap + identity mapping setup)
 *              ④ plugin_manager phase drive (EARLY/CORE/LATE -> enable interrupts -> START)
 *                 -> br_sched_run()
 *
 * This file does only 5 things, touching neither memory management nor the interrupt controller itself:
 *   1. Pick the CPU (boot core continues, the rest park)
 *   2. Mask exception sources (DAIF) + drop from EL2 to EL1h if needed
 *   3. Load the boot stack + install the vector table (the table itself is in vectors.S)
 *   4. Clear BSS
 *   5. Hand the CPU to core's main (does not return)
 */

    .section .text.boot, "ax"
    .balign 16
    .global _start
    .type   _start, %function

_start:
    /* ---- 1. Let only the boot core continue: cores with MPIDR_EL1.Aff0 != 0 stop here ---- */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, .Lpark

    /* ---- 2a. Mask all exception sources (D/A/I/F) ---- */
    msr     daifset, #0xf

    /* ---- 2b. If entered from EL2 (QEMU -machine virtualization=on), drop to EL1h ----
     * Bare-metal QEMU virt already gives EL1 by default, so this is "defensive":
     * writing SP_EL1 without checking CurrentEL would silently misbehave at EL2. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    .Lel_ready

    mov     x0, #(1 << 31)          /* HCR_EL2.RW = 1: EL1 executes as AArch64 */
    msr     hcr_el2, x0
    mov     x0, #0x3c5              /* SPSR_EL2: DAIF all masked + EL1h (M[3:0]=0101) */
    msr     spsr_el2, x0
    adr     x0, .Lel_ready
    msr     elr_el2, x0
    eret

.Lel_ready:
    /* ---- 3a. Select SP_EL1 and load the boot stack (linker script provides __stack_top) ---- */
    mov     x0, #1
    msr     spsel, #1
    ldr     x0, =__stack_top
    mov     sp, x0

    /* ---- 3b. Install the exception vector table ----
     * The table itself is in vectors.S (16 slots × 0x80 B, 2 KiB aligned; 0x200 = fault, 0x280 = IRQ).
     * v0.1 does not enable interrupts before entering core, but fault/SError can still arrive; without a
     * vector table it would jump to address 0 and hang silently — the hardest kind of bug to chase. */
    ldr     x0, =br_plat_vectors
    msr     vbar_el1, x0

    /* ---- 4. Clear BSS (linker script guarantees __bss_end - __bss_start is a multiple of 8) ---- */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
.Lbss_loop:
    cmp     x0, x1
    b.hs    .Lbss_done
    str     xzr, [x0], #8
    b       .Lbss_loop
.Lbss_done:

    /* ---- 5. Hand over to core's entry br_core_main (core/src/main.c) ----
     * ★ Here we **no longer** call br_irq_cpu_init / br_plugin_manager_run directly: the interrupt
     *   framework's per-CPU state, clock/log, section scan, platform early_init, heap and identity
     *   mapping setup, plugin phase drive — all happen in order inside core's four-phase boot chain
     *   (see br_main.h). Assembly only handles reset/BSS; orchestration has exactly one owner: core's
     *   main (design 1-01 §9).
     * Does not return: the manager hands over to the scheduler (br_sched_run) at the end of the START
     * phase, or the last start() occupies the CPU (transitional bridge, see
     * core/include/br/core/br_plugin.h). */
    bl      br_core_main

    /* Theoretically unreachable (br_core_main is BR_NORETURN); treat arrival as unrecoverable */
    b       br_plat_park_forever

    .size   _start, . - _start

/*
 * The exception vector table is **not here** — the entity is in vectors.S (16 slots × 0x80 B, 2 KiB aligned,
 * 0x200 = fault stub, 0x280 = IRQ stub, the other 14 slots = real usable panic stubs;
 * design 3-02 §10.2). This file only installs it into VBAR_EL1 (see 3b).
 *
 * Park spot for the other cores. Placed at the end of .text (not in .text.boot, so as not to crowd the entry layout).
 */
    .section .text, "ax"
    .balign 4
.Lpark:
    wfe
    b       .Lpark
