/*
 * tests/host/host_stubs.c - host-side stand-ins (used only by mem-test with `-DBR_HOSTTEST=1`)
 *
 * Why: the target image is `-ffreestanding -nostdlib`, and core's `br_log_write` ends up at the
 * platform early console (polling PL011). Host tests do not link platform, and should not link
 * the formatter in core/src/log.c (it depends on br_console_*) just to print a few debug lines,
 * so this provides minimal stand-ins:
 *   - `br_log_write` implemented with the host vprintf (tests may use libc);
 *   - `br_console_*` all empty (no PL011 on the host).
 * The stand-ins implement only the **contract** (the signatures in br_log.h / br_console.h) and
 * do not change the semantic surface.
 */
#include <br/core/br_console.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>

#include <stdarg.h>
#include <stdio.h>

static br_log_level_t s_host_level = BR_LOG_INFO;

void br_log_write(br_log_level_t level, const char *fmt, ...)
{
    if (fmt == BR_NULL) {
        return;
    }
    if ((unsigned)level < (unsigned)s_host_level) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    (void)fputs("[hosttest] ", stdout);
    (void)vfprintf(stdout, fmt, ap);
    (void)fputc('\n', stdout);
    va_end(ap);
}

void br_log_set_level(br_log_level_t level)
{
    if ((unsigned)level < (unsigned)BR_LOG_LEVEL_COUNT) {
        s_host_level = level;
    }
}

br_log_level_t br_log_get_level(void)
{
    return s_host_level;
}

/* ---- early console stand-in (no PL011 on the host; empty implementation) ---- */

void br_console_init(void)
{
}

void br_console_putc(char c)
{
    (void)c;
}

void br_console_write(const char *buf, br_size_t len)
{
    (void)buf;
    (void)len;
}

void br_console_puts(const char *s)
{
    (void)s;
}
