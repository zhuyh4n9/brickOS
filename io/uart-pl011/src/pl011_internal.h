/*
 * brickOS prototype v0.2.0 — io/uart-pl011 **plugin-private** internal contract (not exported, not in include/)
 *
 * Audience: this plugin's own `src/uart_selftest.c` (the production implementation
 * `src/uart_pl011.c` includes it too).
 * Living in `src/` (not `include/`) is deliberate — everything under `include/` is the
 * **plugin's public surface** (governed by `brickie check` interface domains), while this is
 * **platform data truth** and does not form a device contract.
 *
 * ## Why the self-test reads internals
 *   TC-IO-003 asserts the platform facts GET_INFO answers: `info.base == 0x0900_0000`,
 *   `info.refclk_hz == 24 MHz`, `info.baud == 115200`. These three numbers **are** the object
 *   under test — if the suite copied them, the criterion would become "copy equals copy", and
 *   it would stay green even after the production file changed the values.
 *   ⇒ the truth is written in one place (this header), **included by both production and
 *   suite**: one source, two consumers.
 *
 * ## Why there are **no** register accessors here (difference from cdev-core's `cdev_internal.h`)
 *   The self-test does not touch MMIO: all its observation of the device goes through the
 *   public file surface of `/dev/uart0` (ioctl answers / read-write round trips), so register
 *   offsets, FR/RSR/ICR bit definitions and `pl011_reg_read/write` all stay static inside
 *   `uart_pl011.c` — this component has no criterion whose object under test is the internal
 *   mechanism itself.
 */
#ifndef BR_IO_PL011_INTERNAL_H
#define BR_IO_PL011_INTERNAL_H

#include <br/core/br_types.h>

/* =====================================================================
 * Platform data (QEMU virt; same origin as platform/qemu-aarch64/src/console_pl011.c)
 *
 * If a value disagrees with the console side, the respective _Static_assert reports it at
 * compile time — duplicated but self-proving, safer than "extracting a shared constant anyone
 * can change" (the two belong to different plugins; core provides no ambiguous shared header
 * like a "PL011 register table").
 * ===================================================================== */

/* PL011 base on QEMU virt (belongs to "platform data" in the design; v1 writes the constant
 * directly, making platform data a plugin comes later) */
#define BR_PL011_BASE        0x09000000UL

/*
 * QEMU virt PL011 reference clock, 24 MHz (QEMU hw/arm/virt.c drives the PL011 model at 24MHz).
 * Divisor for 115200 8N1: 24e6 / (16 × 115200) = 13.0208… ⇒ IBRD=13, FBRD=round(0.0208×64)=1
 */
#define BR_PL011_REFCLK_HZ   24000000u
#define BR_PL011_BAUD        115200u
#define BR_PL011_IBRD_VALUE  13u
#define BR_PL011_FBRD_VALUE  1u

/*
 * Divisor formula (one source of truth, shared by compile time and run time):
 *   divisor = refclk / (16 × baud); IBRD = integer part; FBRD = round(fraction × 64)
 * The fraction is fixed point in 1/64 — so FBRD's "round to nearest" must be written as
 * `(rem×64 + den/2) / den` (den = 16×baud), not computed in floating point (this image is
 * -mgeneral-regs-only, no FP).
 */
#define BR_PL011_DIV_DEN(baud)   ((br_u64)16u * (br_u64)(baud))
#define BR_PL011_DIV_IBRD(clk, baud) \
    ((br_u32)((br_u64)(clk) / BR_PL011_DIV_DEN(baud)))
#define BR_PL011_DIV_FBRD(clk, baud) \
    ((br_u32)((((br_u64)(clk) % BR_PL011_DIV_DEN(baud)) * 64u \
               + (BR_PL011_DIV_DEN(baud) / 2u)) / BR_PL011_DIV_DEN(baud)))

/* Divisor constants are tied to the reference clock: changing the clock but forgetting the
 * divisor is reported at compile time (same method as console_pl011.c).
 * Assertions live in this header = production and suite each verify once per translation unit,
 * while the truth exists only here. */
_Static_assert(BR_PL011_DIV_IBRD(BR_PL011_REFCLK_HZ, BR_PL011_BAUD) == BR_PL011_IBRD_VALUE,
               "PL011 IBRD inconsistent with reference clock/baud");
_Static_assert(BR_PL011_DIV_FBRD(BR_PL011_REFCLK_HZ, BR_PL011_BAUD) == BR_PL011_FBRD_VALUE,
               "PL011 FBRD inconsistent with the fractional divisor for clock/baud");

#endif /* BR_IO_PL011_INTERNAL_H */
