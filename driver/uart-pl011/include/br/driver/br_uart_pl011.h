/*
 * PL011 UART char device -- public face of driver/uart-pl011. Registers the same PL011 as a cdev so
 * /dev/uart0 is openable by br_open, while platform console_pl011.c keeps the M0 early console
 * and panic channel. v1 is a polling cdev, not an interrupt tty: WORKAROUND(br-wa-io-001).
 * QEMU virt: base 0x0900_0000, refclk 24 MHz, 115200 8N1 => IBRD=13 FBRD=1 (same constants in
 * the console, each side _Static_assert-checked).
 */
#ifndef BR_DRIVER_BR_UART_PL011_H
#define BR_DRIVER_BR_UART_PL011_H

#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

/* /dev/<name> ([a-z][a-z0-9]*) */
#define BR_UART_PL011_DEV_NAME   "uart0"

/* ioctl family magic (one per driver family) */
#define BR_UART_IOC_MAGIC        'u'

/* ioctls supported (cmd = BR_IOR/IOW/IOWR(BR_UART_IOC_MAGIC, nr, type)) */
#define BR_UART_IOC_GET_INFO     0u   /* BR_IOR('u', 0, br_uart_pl011_info_t) */
#define BR_UART_IOC_FLUSH_RX     1u   /* BR_IO ('u', 1): drop residue in the RX FIFO */
#define BR_UART_IOC_SET_BAUD     2u   /* BR_IOW('u', 2, br_u32): set baud rate (Hz) */

/* Reply to `BR_UART_IOC_GET_INFO` (session-level query; read-only). */
typedef struct br_uart_pl011_info {
    br_u32 base;           /* register base (platform data, for diagnostics) */
    br_u32 refclk_hz;      /* reference clock */
    br_u32 baud;           /* current baud rate */
    /* PL011's FR has only RXFE/RXFF, no FIFO depth field => honest lower bound: 0 (RXFE set),
     * 16 (RXFF set, FIFO depth 16), otherwise 1. An exact count needs a FIFO read (side effect);
     * query semantics must be side-effect free. */
    br_u32 rx_available;   /* 0 / 1 / 16 -- see note above */
    /* Cumulative overrun count. Source is UARTRSR[3]; FR bit3 is BUSY, not OE. Clearing goes via
     * UARTICR[10] (OEIC) -- ICR[3] is reserved. */
    br_u32 rx_dropped;
} br_uart_pl011_info_t;

/* Lifecycle hooks (name = symbol_prefix + phase; referenced by generated code).
 * early_init EARLY: no-op (console already built; no threads/heap). init CORE: br_cdev_register()
 * makes the device appear in /dev. start START: logs one device summary line only. */
int uart_pl011_early_init(void);
int uart_pl011_init(void);
int uart_pl011_start(void);

/* Conformance cases (TC-IO-*, registered in br-wa-test-001) live in src/uart_selftest.c, driven
 * by core's br_plugin_manager_selftest() via the descriptor's .selftest hook. They are not part of
 * the golden interface face: adding a case must not be an interface change. */

#endif /* BR_DRIVER_BR_UART_PL011_H */
