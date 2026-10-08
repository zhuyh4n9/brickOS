/*
 * tests/host/host_stubs.c — 宿主侧替身(仅 `-DBR_HOSTTEST=1` 的 mem-test 使用)
 *
 * 为什么需要: 目标镜像是 `-ffreestanding -nostdlib`, core 的 `br_log_write` 最终落到
 * platform 的早期 console(轮询 PL011)。宿主用例不链 platform, 也不该为了打几行
 * 排障日志去链 core/src/log.c 里的格式化器(它依赖 br_console_*), 所以这里给最小替身:
 *   - `br_log_write` 用宿主 vprintf 实现(用例允许 libc);
 *   - `br_console_*` 全空(宿主没有 PL011)。
 * 替身只实现**契约**(br_log.h / br_console.h 的签名), 不改语义面。
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

/* ---- 早期 console 替身(宿主无 PL011; 空实现) ---- */

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
