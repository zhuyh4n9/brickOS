/*
 * PL011 early console (polled), implementing core's early console protocol
 * (core/include/br/core/br_console.h). v0.1 registers no device and enables no interrupts.
 */
#include <br/core/br_console.h>
#include <br/platform/br_plat.h>

/* PL011 base address on QEMU virt (platform data). */
#define BR_PL011_BASE        0x09000000UL

/* PL011 register offsets (PrimeCell UART, ARM DDI 0183) */
#define BR_PL011_DR          0x000
#define BR_PL011_FR          0x018
#define BR_PL011_IBRD        0x024
#define BR_PL011_FBRD        0x028
#define BR_PL011_LCR_H       0x02c
#define BR_PL011_CR          0x030
#define BR_PL011_ICR         0x044

#define BR_PL011_FR_TXFF     (1u << 5)

#define BR_PL011_CR_UARTEN   (1u << 0)
#define BR_PL011_CR_TXE      (1u << 8)
#define BR_PL011_CR_RXE      (1u << 9)

#define BR_PL011_LCR_H_8N1_FIFO  0x70u

/* PL011 reference clock on QEMU virt is 24 MHz (QEMU hw/arm/virt.c drives the model at 24 MHz).
 * Divisors for 115200 8N1: 24e6 / (16 * 115200) = 13.0208 -> IBRD=13, FBRD=round(0.0208*64)=1 */
#define BR_PL011_REFCLK_HZ   24000000u
#define BR_PL011_BAUD        115200u
#define BR_PL011_IBRD_VALUE  13u
#define BR_PL011_FBRD_VALUE  1u

/* Divisors are tied to the reference clock; a clock change without them fails at compile time */
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

/* Raw putc, no '\n' translation: that must happen exactly once at the outermost layer. */
static void pl011_putc_raw(char c)
{
    while ((pl011_read(BR_PL011_FR) & BR_PL011_FR_TXFF) != 0) {
        /* busy-wait for space in the TX FIFO; no interrupts, so nothing else to do. */
    }

    pl011_write(BR_PL011_DR, (br_u32)(br_u8)c);
}

void br_console_putc(char c)
{
    /* Emit CR before LF: a serial line expects CRLF, and a missing CR shows as "staircase"
     * newlines. Only this polled console layer does the translation. */
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
