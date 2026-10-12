/*
 * brickOS prototype v0.2.0 — PL011 UART char device (public face of the io/uart-pl011 plugin)
 *
 * Design basis:
 *   - `1-03-roadmap.md` §1 plugin list: `io/uart-pl011` = "PL011: early polling console
 *     (M0: platform early console, **no device registration**) → **interrupt tty (M2: register cdev)**"
 *   - `8-01-device.md` §1.1 (layering: device → device class → dev-core →(devfs)→ VFS),
 *     §6 (driver author contract: implement ops table + three disciplines + declare resources)
 *   - `8-01-device.md` §5 (ioctl uses Linux-compatible encoding; this device's family magic = `'u'`)
 * Case-by-case rulings (especially "why v1 is a polling cdev, not an interrupt tty") see
 * `docs/decisions/0009-vfs-storage-stack.md` §3 ruling 7, registered as `WORKAROUND(br-wa-io-001)`.
 *
 * ## Relation to the platform early console
 *   platform's `console_pl011.c` is the **M0 early console** (polling putc, C standalone shape:
 *   no device registration) — an in-design shape (`1-01` §8's two console shapes). This plugin is its
 *   **M2 shape A** side: register the same PL011 as a `cdev`, so `/dev/uart0` can be opened by `br_open`.
 *   Both share one hardware instance: console keeps kernel logging (incl. the panic channel), `/dev/uart0` is for
 *   consumers doing device-semantics reads/writes (design `8-01` §1.2 division of labor, not a duplicate implementation).
 *
 * ## Register and clock facts (QEMU virt)
 *   Base 0x0900_0000; reference clock 24 MHz; 115200 8N1 ⇒ IBRD=13 FBRD=1 (same-source constants as console —
 *   written in both places because they belong to different plugins; a mismatch is reported by each _Static_assert).
 */
#ifndef BR_IO_BR_UART_PL011_H
#define BR_IO_BR_UART_PL011_H

#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

/* /dev/<name> (design `8-01` §2 naming rule: [a-z][a-z0-9]*) */
#define BR_UART_PL011_DEV_NAME   "uart0"

/* ioctl family magic (design `8-01` §5: one per driver family) */
#define BR_UART_IOC_MAGIC        'u'

/* ioctls supported by this device (cmd = BR_IOR/IOW/IOWR(BR_UART_IOC_MAGIC, nr, type)) */
#define BR_UART_IOC_GET_INFO     0u   /* BR_IOR('u', 0, br_uart_pl011_info_t) */
#define BR_UART_IOC_FLUSH_RX     1u   /* BR_IO ('u', 1): drop residue in the RX FIFO */
#define BR_UART_IOC_SET_BAUD     2u   /* BR_IOW('u', 2, br_u32): set baud rate (Hz) */

/* Reply to `BR_UART_IOC_GET_INFO` (session-level query; read-only). */
typedef struct br_uart_pl011_info {
    br_u32 base;           /* register base (platform data, for diagnostics) */
    br_u32 refclk_hz;      /* reference clock */
    br_u32 baud;           /* current baud rate */
    /* ★ PL011's FR has only the RXFE/RXFF status bits, **no** FIFO depth field ⇒ this reports an
     * honest lower bound: 0 (RXFE set = empty) / 16 (RXFF set = full, FIFO depth 16) / otherwise 1.
     * Reporting an "exact readable byte count" needs a FIFO read (side effects); query semantics must be side-effect free. */
    br_u32 rx_available;   /* 0 / 1 / 16 — see note above */
    /* Cumulative overrun count. ★ Source is **UARTRSR[3]** (ARM DDI 0183): PL011's FR
     * bit3 is BUSY, not OE; clearing overrun goes via UARTICR[10] (OEIC) — ICR[3] is reserved.
     * Using the wrong bit reads BUSY as "lost bytes" and writes a reserved bit. */
    br_u32 rx_dropped;
} br_uart_pl011_info_t;

/*
 * Lifecycle hooks (name = `symbol_prefix` + phase; referenced by generated code).
 *   `early_init` EARLY: no-op (console already built by platform; no threads, no heap yet)
 *   `init`       CORE: `br_cdev_register("uart0", …)` — registration makes it appear in /dev
 *   `start`      START: logs only one device summary line (the conformance case does not run here, see note below)
 */
int uart_pl011_early_init(void);
int uart_pl011_init(void);
int uart_pl011_start(void);

/* ==================================================================== conformance cases */

/* This device's conformance cases (self-numbered `TC-IO-*`; registered in `br-wa-test-001`) have moved to
 * `src/uart_selftest.c` (implementing `uart_pl011_selftest()`), driven uniformly by core's
 * `br_plugin_manager_selftest()` through the descriptor's `.selftest` hook (ADR-0010).
 * Why it was dropped from the public header: the test entry is **not** the device's golden interface face — it is this device's self-proof mechanism,
 * consumers must not depend on it, and it must not participate in interface hash/version governance (adding a case must not be an interface change). */

#endif /* BR_IO_BR_UART_PL011_H */
