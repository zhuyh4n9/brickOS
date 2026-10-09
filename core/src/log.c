/*
 * brickOS prototype v0.1.0 — 日志实现(格式化 + 等级过滤)
 *
 * 不拉 libc 的 printf: -ffreestanding -nostdlib 下没有它, 而为了打几行日志
 * 去链一个 printf 实现, 与设计侧"v1 不引入 libc"的姿态不一致(设计 7/11 章:
 * POSIX 由 runtime/posix 显式提供, 不是默认背景)。
 *
 * 输出的收口只有一个: br_console_putc(平台早期 console)。
 * 等 service/trace(设计 5-01)与 service/log(设计 11-01)落地, 本文件
 * 的 br_log_write 应改为"格式化 -> 交给 service", 而不是直接写 console。
 *
 * WORKAROUND(br-wa-boot-001): 直接写 console, 未经服务注册表。
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

/*
 * 无符号数输出: buf 逆序生成, 再按 width(空间/零填充)补齐后正序打出。
 * buf 24 字节足够: 64 位十进制最长 20 位, 十六进制 16 位。
 */
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

/* 解析 "<flags><width><length><conversion>", 从 *pp 处的转换符之后开始,
 * 结束时把 *pp 停在已消费的转换符上。 */
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
        /* 尾随 '%': 原样收回, 不外读字符串结尾之后 */
        br_console_putc('%');
        *pp = p - 1;
        return;
    default:
        /* 未支持的转换符: 打出来而不是吞掉, 免得"日志少了一段"这种诡异现象 */
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

    /* 行首: [<秒>.<微秒 6 位>] <等级> <正文>\n */
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
