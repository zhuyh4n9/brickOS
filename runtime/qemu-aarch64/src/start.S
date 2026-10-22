/*
 * Platform Entry (aarch64): reset and BSS, nothing else.
 * Picks the boot core, masks DAIF, drops EL2 -> EL1h if needed, loads the boot stack, installs the
 * vector table (vectors.S), clears BSS, then bl br_core_main(); the four-phase boot chain is
 * orchestrated entirely on the core side. Does not return.
 */

    .section .text.boot, "ax"
    .balign 16
    .global _start
    .type   _start, %function

_start:
    /* Let only the boot core continue: MPIDR_EL1.Aff0 != 0 parks. */
    mrs     x1, mpidr_el1
    and     x1, x1, #0xff
    cbnz    x1, .Lpark

    /* Mask all exception sources (D/A/I/F). */
    msr     daifset, #0xf

    /* Drop from EL2 to EL1h if needed (QEMU -machine virtualization=on). Bare-metal QEMU virt
     * already gives EL1, so this is defensive: writing SP_EL1 without checking CurrentEL would
     * misbehave at EL2. */
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
    /* Select SP_EL1 and load the boot stack (__stack_top from the linker script). */
    mov     x0, #1
    msr     spsel, #1
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Install the exception vector table (vectors.S: 16 slots x 0x80 B, 2 KiB aligned). Without it
     * a fault/SError before interrupts are enabled would jump to address 0 and hang silently. */
    ldr     x0, =br_plat_vectors
    msr     vbar_el1, x0

    /* Clear BSS (__bss_end - __bss_start is a multiple of 8). */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
.Lbss_loop:
    cmp     x0, x1
    b.hs    .Lbss_done
    str     xzr, [x0], #8
    b       .Lbss_loop
.Lbss_done:

    /* Hand over to core's entry br_core_main (does not return: it ends in br_sched_run, or the last
     * plugin start() occupies the CPU). */
    bl      br_core_main

    /* Unreachable (br_core_main is BR_NORETURN); treat arrival as unrecoverable. */
    b       br_plat_park_forever

    .size   _start, . - _start

/* The vector table itself is in vectors.S; this file only installs it into VBAR_EL1.
 * .Lpark: park spot for the other cores, at the end of .text so it does not crowd .text.boot. */
    .section .text, "ax"
    .balign 4
.Lpark:
    wfe
    b       .Lpark
