/*
 * Host-side stand-ins for mem-test (`-DBR_HOSTTEST=1`): br_log_write via host vprintf, and empty
 * br_console_*. Implements only the br_log.h / br_console.h contracts; the host tests link neither
 * platform code nor core's log.c formatter.
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
