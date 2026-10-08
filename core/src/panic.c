/*
 * brickOS prototype v0.1.0 — panic: 独立通道的最小格式化输出 + 停机
 *
 * 权威设计: `docs/3-os-core/3-02-int.md` §8.3.1(panic 的 ISR 契约)与 §10.6。
 *
 * ★ 两种入口的分工(§8.3.1):
 *   br_panic_bare()  —— **任何上下文**(ISR / fault / double fault); 只轮询 console
 *                       打一行最小信息然后停机。**无锁 / 无堆 / 无调度器 / 不写 trace 环**,
 *                       与 5-01 §2「panic 通道独立」同一原则。
 *   br_panic()       —— thread-only 便利入口; v0.1 与 bare 的差别只有门口那句提示
 *                       (完整路径要等 trace/tamdump 服务落地)。
 *   两者都是**终止性**的: "停机"就是语义, 因此不受"ISR 内不得阻塞"约束, 但**仍不得
 *   在停机前取锁**(会把锁留给已经不存在的世界, 且让 panic 依赖锁状态)。
 *
 * 为什么自带格式化器: -ffreestanding -nostdlib 下没有 printf; 而为了 panic 去链一个
 * 完整 printf 与"panic 不依赖任何插件栈"相冲突(设计 §8.3.1)。这里只实现必需子集:
 *   %% %c %s %d %i %u %x %X %p, 支持 '0' 零填充与最小宽度、l/ll/z 长度修饰。
 * 输出收口 = core 拥有的轮询 console 契约(br_console_putc / br_console_puts)。
 */
#include <br/core/br_console.h>
#include <br/core/br_fault.h>

#include <stdarg.h>

/* 无符号数按 base 输出: buf 逆序生成, 再按 width(零/空格填充)补齐后正序打出。
 * buf 24 字节足够: 64 位十进制最长 20 位, 十六进制 16 位。 */
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

/* 解析 "<flags><width><length><conversion>"; *pp 停在已消费的转换符上。 */
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
        /* 尾随 '%': 原样收回, 不越过字符串结尾 */
        br_console_putc('%');
        *pp = p - 1;
        return;
    default:
        /* 未支持的转换符: 打出来而不是吞掉, 免得"少了一段"这类诡异现象 */
        br_console_putc('%');
        br_console_putc(*p);
        break;
    }

    *pp = p;
}

/* 行格式: "\n" + tag + " " + <message> + "\n"; 全部直写轮询 console。 */
static void panic_vprint(const char *tag, const char *fmt, va_list ap)
{
    /*
     * 停机前把四个异常屏蔽位全部置上(与 §10.2 "fault 桩一律显式全屏蔽"同源):
     * 防止 panic 输出期间被新 IRQ/SError/debug 异常重入而搅乱最后的信息。
     * 只写 PSTATE —— 不取锁、不调插件、不碰堆/时钟/trace。
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

    va_start(ap, fmt);
    panic_vprint("[PANIC]", fmt, ap);
    va_end(ap);

    /* 就地停机: 不调用任何平台/插件代码(§8.3.1)。wfe 而非 wfi —— 事件型等待,
     * 不承诺被"被屏蔽的 pending 中断"唤醒(那正是 WFI 的行为, 见 §11.1 的 idle 讨论)。 */
    for (;;) {
        __asm__ volatile("wfe" ::: "memory");
    }
}

BR_NORETURN void br_panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    panic_vprint("[PANIC](thread)", fmt, ap);
    va_end(ap);

    for (;;) {
        __asm__ volatile("wfe" ::: "memory");
    }
}
