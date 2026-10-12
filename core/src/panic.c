/*
 * brickOS prototype v0.1.0 — panic: minimal formatted output on a standalone channel + halt
 *
 * Authoritative design: `docs/3-os-core/3-02-int.md` §8.3.1 (panic ISR contract) and §10.6.
 *
 * ★ Division of the two entries (§8.3.1):
 *   br_panic_bare()  —— **any context** (ISR / fault / double fault); only polls the console
 *                       for one minimal line then halts. **No lock / no heap / no scheduler /
 *                       no trace ring writes**, same principle as 5-01 §2 "panic channel is
 *                       standalone".
 *   br_panic()       —— thread-only convenience entry; in v0.1 it differs from bare only in
 *                       that one banner line (the full path waits for trace/tamdump services).
 *   Both are **terminal**: "halt" is the semantics, so they are not bound by "no blocking in
 *   an ISR", but they **still must not take locks before halting** (that would leave locks to
 *   a world that no longer exists, and make panic depend on lock state).
 *
 * Why a private formatter: no printf under -ffreestanding -nostdlib; and linking a full printf
 * for panic conflicts with "panic depends on no plugin stack" (design §8.3.1). Only the
 * required subset is implemented here:
 *   %% %c %s %d %i %u %x %X %p, with '0' zero padding, minimum width, l/ll/z length modifiers.
 * Output sink = the polling console contract owned by core (br_console_putc / br_console_puts).
 */
#include <br/core/br_console.h>
#include <br/core/br_fault.h>

#include "panic_internal.h"

#include <stdarg.h>

/*
 * State of the panic re-entrancy guard (§10.6 and ADR-0003 §5).
 * v0.1 is single-core ⇒ a file-static is enough (v2 per-CPU comes with TPIDR_EL1 per-CPU);
 * it is **volatile**: a fault may re-enter where the compiler cannot see it.
 */
static volatile br_u32 s_panic_depth;

br_bool br_panic_in_progress(void)
{
    return (s_panic_depth != 0u) ? BR_TRUE : BR_FALSE;
}

BR_NORETURN void br_panic_halt(void)
{
    /* No printing, no locking, no touching anything possibly corrupted — just stop.
     * wfe not wfi: an event-type wait, not promising to wake on "masked pending interrupts" (§11.1). */
    for (;;) {
        __asm__ volatile("wfe" ::: "memory");
    }
}

/* Emit an unsigned value in base: buf is built in reverse, padded to width (zero/space), then
 * printed forward. buf of 24 bytes suffices: 64-bit decimal is at most 20 digits, hex 16. */
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
        /* unsupported conversion: print it rather than swallow it, avoiding odd "a chunk is
         * missing" symptoms */
        br_console_putc('%');
        br_console_putc(*p);
        break;
    }

    *pp = p;
}

/* Line format: "\n" + tag + " " + <message> + "\n"; all written straight to the polling console. */
static void panic_vprint(const char *tag, const char *fmt, va_list ap)
{
    /*
     * Before halting, set all four exception mask bits (same origin as §10.2 "fault stubs
     * always mask everything explicitly"): prevent re-entry by a new IRQ/SError/debug
     * exception during panic output from garbling the final message.
     * PSTATE only — no locks, no plugin calls, no heap/clock/trace.
     */
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

    /* ★ set the re-entrancy flag before printing: if the print path faults again, the fault
     *   path sees it and halts silently instead of recursing (see the background note in
     *   panic_internal.h). */
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
