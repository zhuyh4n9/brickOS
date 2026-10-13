/*
 * io/uart-pl011 plugin-private contract (src/, not include/: not exported, not a device
 * contract). Holds the platform data truth shared by src/uart_pl011.c and src/uart_selftest.c,
 * so the TC-IO-003 facts (base/refclk/baud) have one source instead of "copy equals copy".
 * No register accessors here: the suite observes the device only through the /dev/uart0 file API.
 */
#ifndef BR_IO_PL011_INTERNAL_H
#define BR_IO_PL011_INTERNAL_H

#include <br/core/br_types.h>

/* Platform data (QEMU virt; same origin as platform/qemu-aarch64/src/console_pl011.c). A value
 * that disagrees with the console side is reported by each side's _Static_assert. */

/* PL011 base on QEMU virt (platform data written directly in v1). */
#define BR_PL011_BASE        0x09000000UL

/*
 * QEMU virt PL011 reference clock, 24 MHz (QEMU hw/arm/virt.c drives the model at 24 MHz).
 * Divisor for 115200 8N1: 24e6 / (16 * 115200) = 13.0208... => IBRD=13, FBRD=round(0.0208*64)=1
 */
#define BR_PL011_REFCLK_HZ   24000000u
#define BR_PL011_BAUD        115200u
#define BR_PL011_IBRD_VALUE  13u
#define BR_PL011_FBRD_VALUE  1u

/*
 * Divisor formula (one source of truth for compile time and run time):
 *   divisor = refclk / (16 * baud); IBRD = integer part; FBRD = round(fraction * 64)
 * The fraction is fixed point in 1/64, so "round to nearest" is (rem*64 + den/2) / den
 * (den = 16*baud), never a floating-point computation (this image is -mgeneral-regs-only).
 */
#define BR_PL011_DIV_DEN(baud)   ((br_u64)16u * (br_u64)(baud))
#define BR_PL011_DIV_IBRD(clk, baud) \
    ((br_u32)((br_u64)(clk) / BR_PL011_DIV_DEN(baud)))
#define BR_PL011_DIV_FBRD(clk, baud) \
    ((br_u32)((((br_u64)(clk) % BR_PL011_DIV_DEN(baud)) * 64u \
               + (BR_PL011_DIV_DEN(baud) / 2u)) / BR_PL011_DIV_DEN(baud)))

/* Divisor constants are tied to the reference clock: changing the clock but forgetting the
 * divisor is caught at compile time. Production and suite each verify once per translation unit,
 * while the truth exists only here. */
_Static_assert(BR_PL011_DIV_IBRD(BR_PL011_REFCLK_HZ, BR_PL011_BAUD) == BR_PL011_IBRD_VALUE,
               "PL011 IBRD inconsistent with reference clock/baud");
_Static_assert(BR_PL011_DIV_FBRD(BR_PL011_REFCLK_HZ, BR_PL011_BAUD) == BR_PL011_FBRD_VALUE,
               "PL011 FBRD inconsistent with the fractional divisor for clock/baud");

#endif /* BR_IO_PL011_INTERNAL_H */
