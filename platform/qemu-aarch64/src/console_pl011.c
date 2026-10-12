/*
 * brickOS prototype v0.1.0 — PL011 early console (polled)
 *
 * Implements the "early console protocol (polled putc)" owned by core (core/include/br/core/br_console.h),
 * matching the three-layer pattern of design 1-01 §8: the interface is in core, the platform supplies
 * "which UART, what baud rate".
 *
 * Paid off (ADR-0005): this file is part of the Platform Entry (not made a plugin -- that is the **in-design**
 * shape, the shape itself is **not** a workaround -- design 1-01 §8 explicitly has two console forms, and the
 * polled one is M0's early console (M0's `io/uart-pl011` also registers no device, see 1-03 §1).
 *
 * v0.1.0 registers no device, enables no interrupts, uses no FIFO interrupt; devfs/cdev are M2's business.
 */
#include <br/core/br_console.h>
#include <br/platform/br_plat.h>

/* PL011 base address on QEMU virt (on the design side this is part of "platform data", to move into the platform plugin later) */
#define BR_PL011_BASE        0x09000000UL

/* PL011 register offsets (PrimeCell UART, ARM DDI 0183) */
#define BR_PL011_DR          0x000   /* Data Register            */
#define BR_PL011_FR          0x018   /* Flag Register            */
#define BR_PL011_IBRD        0x024   /* Integer Baud Rate Divisor*/
#define BR_PL011_FBRD        0x028   /* Fractional Baud Divisor  */
#define BR_PL011_LCR_H       0x02c   /* Line Control             */
#define BR_PL011_CR          0x030   /* Control                  */
#define BR_PL011_ICR         0x044   /* Interrupt Clear          */

#define BR_PL011_FR_TXFF     (1u << 5)   /* TX FIFO full */

#define BR_PL011_CR_UARTEN   (1u << 0)
#define BR_PL011_CR_TXE      (1u << 8)
#define BR_PL011_CR_RXE      (1u << 9)

#define BR_PL011_LCR_H_8N1_FIFO  0x70u   /* WLEN=11 (8-bit) | FEN=1 */

/*
 * PL011 reference clock on QEMU virt is 24 MHz (a platform parameter, not a guess: QEMU hw/arm/virt.c
 * drives the PL011 model at 24MHz). Divisors for 115200 8N1:
 *     24e6 / (16 * 115200) = 13.0208 -> IBRD=13, FBRD=round(0.0208*64)=1
 */
#define BR_PL011_REFCLK_HZ   24000000u
#define BR_PL011_BAUD        115200u
#define BR_PL011_IBRD_VALUE  13u
#define BR_PL011_FBRD_VALUE  1u

/* Divisor constants are tied to the reference clock: changing the clock without the divisors fails at compile time */
_Static_assert(BR_PL011_REFCLK_HZ / (16u * BR_PL011_BAUD) == BR_PL011_IBRD_VALUE,
               "PL011 IBRD inconsistent with reference clock/baud rate");

static void pl011_write(br_u32 off, br_u32 value)
{
    *(volatile br_u32 *)(BR_PL011_BASE + off) = value;
}

static br_u32 pl011_read(br_u32 off)
{
    return *(volatile br_u32 *)(BR_PL011_BASE + off);
}

void br_console_init(void)
{
    pl011_write(BR_PL011_CR, 0);              /* disable first, then change parameters */
    pl011_write(BR_PL011_ICR, 0x7ffu);        /* clear pending interrupts */
    pl011_write(BR_PL011_IBRD, BR_PL011_IBRD_VALUE);
    pl011_write(BR_PL011_FBRD, BR_PL011_FBRD_VALUE);
    pl011_write(BR_PL011_LCR_H, BR_PL011_LCR_H_8N1_FIFO);
    pl011_write(BR_PL011_CR,
                BR_PL011_CR_UARTEN | BR_PL011_CR_TXE | BR_PL011_CR_RXE);
}

/* Raw putc with no '\n' translation: the translation must happen exactly once at the outermost layer,
 * otherwise '\n' gets two '\r's. */
static void pl011_putc_raw(char c)
{
    while ((pl011_read(BR_PL011_FR) & BR_PL011_FR_TXFF) != 0) {
        /* busy-wait for space in the TX FIFO. No interrupts, so there is nothing else to do. */
    }

    pl011_write(BR_PL011_DR, (br_u32)(br_u8)c);
}

void br_console_putc(char c)
{
    /* Terminal friendly: when emitting '\n', also emit '\r'. A serial line expects CRLF; a missing CR
     * shows up as "staircase" newlines on some terminals. This translation is done only in the polled
     * console layer. */
    if (c == '\n') {
        pl011_putc_raw('\r');
    }

    pl011_putc_raw(c);
}

void br_console_write(const char *buf, br_size_t len)
{
    for (br_size_t i = 0; i < len; i++) {
        br_console_putc(buf[i]);
    }
}

void br_console_puts(const char *s)
{
    while (*s != '\0') {
        br_console_putc(*s);
        s++;
    }
}
