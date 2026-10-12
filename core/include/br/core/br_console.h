/*
 * brickOS prototype v0.1.0 — early console contract (polled putc)
 *
 * Why this header lives in core, not platform:
 *   Design doc 1-01 §8 "three-layer platform capability model": **interfaces in core /
 *   ISA shared library / platform supplies data**. The authoritative definition of the
 *   early console protocol (polled putc) is on the core side (3-01 §10 table); the
 *   Platform plugin fills the implementation (here platform/src/aarch64/console_pl011.c).
 *
 * Shape note (not a workaround):
 *   Design doc 1-01 §8 defines two console shapes — the **platform early console
 *   (polled, init-chain printing)** then the I/O plugin full tty (interrupt-driven,
 *   cdev registered at M2). v0.1.0 uses the former, so polling here and registering no
 *   device is **by design**, not a defect.
 */
#ifndef BR_CORE_BR_CONSOLE_H
#define BR_CORE_BR_CONSOLE_H

#include <br/core/br_types.h>

/* Initialize the early console. Must be called once before any br_console_* output.
 * Owner: platform (inside br_plat_early_init). */
void br_console_init(void);

/* Emit one character (blocks until the transmit register is writable). The
 * implementation handles the terminal-friendly '\n' -> "\r\n" conversion. */
void br_console_putc(char c);

/* Emit a fixed-length buffer (may contain '\0'; no string interpretation). */
void br_console_write(const char *buf, br_size_t len);

/* Emit a NUL-terminated string. */
void br_console_puts(const char *s);

#endif /* BR_CORE_BR_CONSOLE_H */
