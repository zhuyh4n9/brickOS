/*
 * Early console contract (polled putc). Core owns the protocol; the platform plugin provides
 * the implementation. v0.1.0 is polled only -- registering no device is by design.
 */
#ifndef BR_CORE_BR_CONSOLE_H
#define BR_CORE_BR_CONSOLE_H

#include <br/core/br_types.h>

/* Must be called once before any br_console_* output. */
void br_console_init(void);

/* Emit one character; blocks until the transmit register is writable. '\n' is sent as "\r\n". */
void br_console_putc(char c);

/* Emit a fixed-length buffer; may contain '\0' (no string interpretation). */
void br_console_write(const char *buf, br_size_t len);

/* Emit a NUL-terminated string. */
void br_console_puts(const char *s);

#endif /* BR_CORE_BR_CONSOLE_H */
