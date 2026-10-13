/*
 * Panic: minimal formatted output on a standalone channel, then halt (terminal, never returns).
 * br_panic_bare() is callable from any context (ISR/fault/double fault): no lock, heap, scheduler
 * or trace-ring writes; br_panic() is the thread-only entry (v0.1: only a different banner).
 * Neither may take a lock before halting.
 * No printf under -ffreestanding -nostdlib and panic must not depend on the plugin stack, so the
 * private formatter covers %% %c %s %d %i %u %x %X %p; the sink is the polling console.
 */
#include <br/core/br_console.h>
#include <br/core/br_fault.h>

#include "panic_internal.h"

#include <stdarg.h>

/* Panic re-entrancy depth; volatile since a fault may re-enter where the compiler cannot see it. */
static volatile br_u32 s_panic_depth;

br_bool br_panic_in_progress(void)
{
    return (s_panic_depth != 0u) ? BR_TRUE : BR_FALSE;
}

BR_NORETURN void br_panic_halt(void)
{
    /* No printing, no locking, nothing possibly corrupted -- just stop. wfe, not wfi: an
     * event-type wait, not promising to wake on masked pending interrupts. */
    for (;;) {
        __asm__ volatile("wfe" ::: "memory");
    }
}

/* Reverse-generate into buf, pad to width (zero/space), print forward. buf[24] suffices: 64-bit
 * decimal is at most 20 digits, hex 16. */
static void panic_emit_uint(br_u64 value, br_u32 base, br_u32 width,
                            br_bool zero_pad, br_bool upper)
{
    const char *digits = (upper == BR_TRUE) ? "0123456789ABCDEF" : "0123456789abcdef";
    char   buf[24];
    br_u32 n = 0u;

    if (value == 0u) {
        buf[n++] = '0';
    }
    while (value != 0u) {
        buf[n++] = digits[value % (br_u64)base];
        value /= (br_u64)base;
    }

    if (width > n) {
        const char pad = (zero_pad == BR_TRUE) ? '0' : ' ';
        for (br_u32 k = n; k < width; k++) {
            br_console_putc(pad);
        }
    }
    while (n > 0u) {
        br_console_putc(buf[--n]);
    }
}

/* Parse "<flags><width><length><conversion>"; *pp stops on the consumed conversion char. */
static void panic_emit_field(const char **pp, va_list *ap)
{
    const char *p = *pp;
    br_bool zero_pad = BR_FALSE;
    br_u32  width    = 0u;
    br_bool is64     = BR_FALSE;

    while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
        if (*p == '0') {
            zero_pad = BR_TRUE;
        }
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        width = (width * 10u) + (br_u32)(*p - '0');
        p++;
    }
    while (*p == 'l' || *p == 'z' || *p == 'h') {
        if (*p != 'h') {
            is64 = BR_TRUE;
        }
        p++;
    }

    switch (*p) {
    case 'u':
        panic_emit_uint(is64 == BR_TRUE ? (br_u64)va_arg(*ap, unsigned long)
                                        : (br_u64)va_arg(*ap, unsigned int),
                        10u, width, zero_pad, BR_FALSE);
        break;
    case 'd':
    case 'i': {
        const br_s64 v = (is64 == BR_TRUE) ? (br_s64)va_arg(*ap, long)
                                           : (br_s64)va_arg(*ap, int);
        if (v < 0) {
            br_console_putc('-');
            panic_emit_uint((br_u64)(-v), 10u, width, zero_pad, BR_FALSE);
        } else {
            panic_emit_uint((br_u64)v, 10u, width, zero_pad, BR_FALSE);
        }
        break;
    }
    case 'x':
        panic_emit_uint(is64 == BR_TRUE ? (br_u64)va_arg(*ap, unsigned long)
                                        : (br_u64)va_arg(*ap, unsigned int),
                        16u, width, zero_pad, BR_FALSE);
        break;
    case 'X':
        panic_emit_uint(is64 == BR_TRUE ? (br_u64)va_arg(*ap, unsigned long)
                                        : (br_u64)va_arg(*ap, unsigned int),
                        16u, width, zero_pad, BR_TRUE);
        break;
    case 'p':
        br_console_puts("0x");
        panic_emit_uint((br_u64)(br_uintptr_t)va_arg(*ap, void *),
                        16u, 16u, BR_TRUE, BR_FALSE);
        break;
    case 'c':
        br_console_putc((char)va_arg(*ap, int));
        break;
    case 's': {
        const char *s = va_arg(*ap, const char *);
        br_console_puts((s != BR_NULL) ? s : "(null)");
        break;
    }
    case '%':
        br_console_putc('%');
        break;
    case '\0':
        /* trailing '%': take it back verbatim, do not pass the end of the string */
        br_console_putc('%');
        *pp = p - 1;
        return;
    default:
        /* unsupported conversion: print it rather than swallow it */
        br_console_putc('%');
        br_console_putc(*p);
        break;
    }

    *pp = p;
}

/* Line format: "\n" + tag + " " + <message> + "\n"; all written straight to the polling console. */
static void panic_vprint(const char *tag, const char *fmt, va_list ap)
{
    /* Mask all four exception mask bits before halting (PSTATE only -- no locks, plugin calls,
     * heap/clock/trace): a re-entrant IRQ/SError/debug exception would garble the final message. */
    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    br_console_putc('\n');
    br_console_puts(tag);
    br_console_putc(' ');

    if (fmt != BR_NULL) {
        for (const char *p = fmt; *p != '\0'; p++) {
            if (*p != '%') {
                br_console_putc(*p);
                continue;
            }
            p++;
            panic_emit_field(&p, &ap);
        }
    }

    br_console_putc('\n');
}

BR_NORETURN void br_panic_bare(const char *fmt, ...)
{
    va_list ap;

    /* Set the re-entrancy flag before printing: a fault in the print path then halts silently
     * instead of recursing (see panic_internal.h). */
    s_panic_depth++;

    va_start(ap, fmt);
    panic_vprint("[PANIC]", fmt, ap);
    va_end(ap);

    br_panic_halt();
}

BR_NORETURN void br_panic(const char *fmt, ...)
{
    va_list ap;

    s_panic_depth++;

    va_start(ap, fmt);
    panic_vprint("[PANIC](thread)", fmt, ap);
    va_end(ap);

    br_panic_halt();
}
