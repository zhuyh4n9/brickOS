/*
 * Logging (formatting + level filtering). No libc printf under -ffreestanding -nostdlib; all output
 * funnels through br_console_putc (platform early console).
 * WORKAROUND(br-wa-boot-001): write the console directly, bypassing the service registry.
 */
#include <br/core/br_log.h>
#include <br/core/br_console.h>
#include <br/core/br_time.h>

#include <stdarg.h>

static br_log_level_t s_level = BR_LOG_INFO;
static br_time_t      s_t0    = 0;

static const char *const s_level_tag[BR_LOG_LEVEL_COUNT] = {
    "DEBUG",
    "INFO ",
    "WARN ",
    "ERROR",
};

void br_log_init(void)
{
    s_t0 = br_clock_now();
}

void br_log_set_level(br_log_level_t level)
{
    if ((unsigned)level < (unsigned)BR_LOG_LEVEL_COUNT) {
        s_level = level;
    }
}

br_log_level_t br_log_get_level(void)
{
    return s_level;
}

/* Reverse-generate into buf, then pad to width (space/zero fill) and emit forward.
 * buf[24] suffices: 64-bit decimal is at most 20 digits, hex 16. */
static void emit_uint(br_u64 value, br_u32 base, br_u32 width, br_bool zero_pad)
{
    char     buf[24];
    br_u32   n = 0;

    if (value == 0) {
        buf[n++] = '0';
    }
    while (value != 0) {
        const br_u32 d = (br_u32)(value % base);
        buf[n++] = (char)((d < 10u) ? ('0' + d) : ('a' + d - 10u));
        value /= base;
    }

    if (width > n) {
        const char pad = zero_pad ? '0' : ' ';
        for (br_u32 k = n; k < width; k++) {
            br_console_putc(pad);
        }
    }
    while (n > 0) {
        br_console_putc(buf[--n]);
    }
}

static void emit_str(const char *s)
{
    br_console_puts((s != BR_NULL) ? s : "(null)");
}

/* Parse "<flags><width><length><conversion>" at *pp; leaves *pp on the conversion char. */
static void emit_field(const char **pp, va_list *ap)
{
    const char *p = *pp;
    br_bool zero_pad = BR_FALSE;
    br_u32  width    = 0;
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
        emit_uint(is64 ? (br_u64)va_arg(*ap, unsigned long)
                       : (br_u64)va_arg(*ap, unsigned int),
                 10u, width, zero_pad);
        break;
    case 'd':
    case 'i': {
        const br_s64 v = is64 ? (br_s64)va_arg(*ap, long)
                              : (br_s64)va_arg(*ap, int);
        if (v < 0) {
            br_console_putc('-');
            emit_uint((br_u64)(-v), 10u, width, zero_pad);
        } else {
            emit_uint((br_u64)v, 10u, width, zero_pad);
        }
        break;
    }
    case 'x':
    case 'X':
        emit_uint(is64 ? (br_u64)va_arg(*ap, unsigned long)
                       : (br_u64)va_arg(*ap, unsigned int),
                 16u, width, zero_pad);
        break;
    case 'p':
        br_console_puts("0x");
        emit_uint((br_u64)(br_uintptr_t)va_arg(*ap, void *), 16u, 16u, BR_TRUE);
        break;
    case 'c':
        br_console_putc((char)va_arg(*ap, int));
        break;
    case 's':
        emit_str(va_arg(*ap, const char *));
        break;
    case '%':
        br_console_putc('%');
        break;
    case '\0':
        /* Trailing '%': take it back as-is, do not read past the end of the string */
        br_console_putc('%');
        *pp = p - 1;
        return;
    default:
        /* Unsupported conversion: print it rather than swallow it */
        br_console_putc('%');
        br_console_putc(*p);
        break;
    }

    *pp = p;
}

void br_log_write(br_log_level_t level, const char *fmt, ...)
{
    if ((unsigned)level >= (unsigned)BR_LOG_LEVEL_COUNT) {
        return;
    }
    if ((unsigned)level < (unsigned)s_level) {
        return;
    }
    if (fmt == BR_NULL) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);

    const br_time_t elapsed_us = br_clock_now() - s_t0;

    br_console_putc('[');
    emit_uint((br_u64)(elapsed_us / BR_US_PER_SEC), 10u, 5u, BR_FALSE);
    br_console_putc('.');
    emit_uint((br_u64)(elapsed_us % BR_US_PER_SEC), 10u, 6u, BR_TRUE);
    br_console_puts("] ");
    br_console_puts(s_level_tag[level]);
    br_console_putc(' ');

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            br_console_putc(*p);
            continue;
        }
        p++;
        emit_field(&p, &ap);
    }

    br_console_putc('\n');

    va_end(ap);
}
