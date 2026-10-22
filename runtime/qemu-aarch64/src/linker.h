/*
 * Linker-script-provided symbols of this platform (single source of truth:
 * runtime/qemu-aarch64/src/link.ld).
 *
 * A `.c` must not declare these itself (ADR-0038 / R1: a declaration in a translation unit needs
 * its definition in the same unit; linker labels have none). One header, so "which labels does the
 * linker script promise" has exactly one answer and moving a label means editing one place.
 */
#ifndef BR_PLATFORM_QEMU_AARCH64_LINKER_H
#define BR_PLATFORM_QEMU_AARCH64_LINKER_H

/* Image bounds: `__image_start` = first byte of .text, `__stack_top` = top of the boot stack
 * (stacks grow down; `__stack_bottom` is not needed by C, the MMU table is in mmu.c). */
extern char __image_start[];
extern char __stack_top[];

#endif /* BR_PLATFORM_QEMU_AARCH64_LINKER_H */
