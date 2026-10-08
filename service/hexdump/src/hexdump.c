/*
 * service/hexdump — 十六进制 + ASCII 呈现原语(实现)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md`
 *   §2 debug bridge 的 `MEMRD` / 主机侧 `brickie dbg`(把内存读回渲染成可读行);
 *   §3 mini ramdump 的输出形态("捕获路径只用静态缓冲"这一约束在这里落地:
 *      本文件只用一行的栈缓冲, 不分配、不缓冲整段)。
 *
 * ★ 格式是契约(用例 TC-DBG-020 逐字节断言), 权威描述在
 *   include/br/debug/br_hexdump.h 的文件头。本实现把它展开成可核算的字段:
 *
 *     [地址 16][2 空格][组区 49][1 空格]['|'][ASCII 16]['|']['\n'] = 87 字节
 *
 *   组区 49 = 16 组 × (2 hex + 1 分隔空格) + 第 8 组后的 1 个加宽空格。
 *   因此最后一个 hex 组与 '|' 之间是 **2 个空格**(组区末的分隔空格 + 显式空格),
 *   与头文件样例 `…00  |Hello, world!...|` 一致; 满行 87 字节(含行尾 '\n')。
 *
 * ★ 本插件**不做边界判定**: "谁读内存谁负责问 region 表"(那是 service/dump 的
 *   职责, 见 br_hexdump.h 抬头)。这里只把调用方给的 base/len 变成行。
 *
 * ISR 安全: 全部 **thread-only**(会写 console)。
 */
#include <br/debug/br_hexdump.h>

#include <br/core/br_console.h>
#include <br/core/br_log.h>

/* =====================================================================
 * 行格式常量(全部可核算; 用 _Static_assert 把契约钉在编译期)
 * ===================================================================== */

#define HD_ADDR_DIGITS   16u   /* 地址固定 16 位小写 hex */
#define HD_GROUP_EXTRA_AT 8u   /* 第 8 组之后加宽 1 个空格 */

/* 满行字节数 = 16 + 2 + 49 + 1 + 1 + 16 + 1 + 1 = 87(含行尾 '\n') */
#define HD_LINE_BYTES    (HD_ADDR_DIGITS + 2u                                   \
                          + (BR_HEXDUMP_WIDTH * 3u + 1u)                        \
                          + 1u + 1u + BR_HEXDUMP_WIDTH + 1u + 1u)
_Static_assert(HD_LINE_BYTES == 87u, "hexdump 行字节数契约必须是 87");

/* 一行缓冲(87 字节 + NUL 余量); br_hexdump 只用这一个, 不进整段缓冲 */
#define HD_LINE_BUF      (HD_LINE_BYTES + 1u)
_Static_assert(HD_LINE_BUF <= 128u, "行缓冲必须留在 128 B 以内");

static const char HD_HEX[] = "0123456789abcdef";

/* =====================================================================
 * 渲染(不借 br_log_info 的格式化, 保证逐字节可控)
 * ===================================================================== */

/*
 * 输出汇: 逻辑位置上永远前进, 但只在"cap 留出结尾 NUL 之后"的位置真正落笔
 * —— 与 snprintf 的截断语义一致(pos 可超过 cap, 返回值照报"需要的字节数")。
 */
typedef struct {
    char      *out;
    br_size_t  cap;
    br_size_t  pos;
} hd_sink_t;

static void hd_putc(hd_sink_t *s, char c)
{
    if ((s->out != BR_NULL) && ((s->pos + 1u) < s->cap)) {
        s->out[s->pos] = c;
    }
    s->pos++;
}

static void hd_hex2(hd_sink_t *s, br_u8 v)
{
    hd_putc(s, HD_HEX[(v >> 4) & 0x0fu]);
    hd_putc(s, HD_HEX[v & 0x0fu]);
}

static void hd_addr16(hd_sink_t *s, br_uintptr_t addr)
{
    for (br_u32 k = HD_ADDR_DIGITS; k > 0u; k--) {
        hd_putc(s, HD_HEX[(addr >> ((k - 1u) * 4u)) & 0x0fu]);
    }
}

/* ASCII 列: 0x20..0x7E 原样, 其余(含 '\n' 与 '\0')一律 '.' */
static char hd_ascii(br_u8 v)
{
    return ((v >= 0x20u) && (v <= 0x7eu)) ? (char)v : '.';
}

/* 渲染一行; n <= BR_HEXDUMP_WIDTH。不足的组用 3 空格占位, ASCII 列缺的用空格。 */
static void hd_render_line(hd_sink_t *s, br_uintptr_t addr,
                           const br_u8 *p, br_size_t n)
{
    hd_addr16(s, addr);
    hd_putc(s, ' ');
    hd_putc(s, ' ');

    for (br_u32 i = 0u; i < BR_HEXDUMP_WIDTH; i++) {
        if ((br_size_t)i < n) {
            hd_hex2(s, p[i]);
        } else {
            hd_putc(s, ' ');
            hd_putc(s, ' ');
        }
        hd_putc(s, ' ');                              /* 组分隔 */
        if (i == (HD_GROUP_EXTRA_AT - 1u)) {
            hd_putc(s, ' ');                          /* 第 8 组后加宽 */
        }
    }

    hd_putc(s, ' ');                                  /* 组区末: 显式 1 空格 */
    hd_putc(s, '|');
    for (br_u32 i = 0u; i < BR_HEXDUMP_WIDTH; i++) {
        hd_putc(s, ((br_size_t)i < n) ? hd_ascii(p[i]) : ' ');
    }
    hd_putc(s, '|');
    hd_putc(s, '\n');
}

/* 需要的字节数(不含结尾 NUL) */
static br_size_t hd_needed(br_size_t len)
{
    return ((len + ((br_size_t)BR_HEXDUMP_WIDTH - 1u)) / (br_size_t)BR_HEXDUMP_WIDTH)
           * (br_size_t)HD_LINE_BYTES;
}

/* =====================================================================
 * 对外面
 * ===================================================================== */

br_size_t br_hexdump_to(char *out, br_size_t cap, const void *base, br_size_t len)
{
    /* base 为空却有长度 ⇒ 没有可渲染的地址(诚实回报 0, 不假装知道地址) */
    if ((base == BR_NULL) && (len > 0u)) {
        return 0u;
    }

    const br_size_t need = hd_needed(len);

    /* 两段式调用: 只回报长度 */
    if ((out == BR_NULL) || (cap == 0u)) {
        return need;
    }

    hd_sink_t s;
    s.out = out;
    s.cap = cap;
    s.pos = 0u;

    const br_u8 *p = (const br_u8 *)base;
    for (br_size_t off = 0u; off < len; off += (br_size_t)BR_HEXDUMP_WIDTH) {
        br_size_t n = len - off;
        if (n > (br_size_t)BR_HEXDUMP_WIDTH) {
            n = (br_size_t)BR_HEXDUMP_WIDTH;
        }
        hd_render_line(&s, (br_uintptr_t)base + off, p + off, n);
    }

    /* 保证 NUL 结尾(cap > 0): 未截断 ⇒ 落在 need 处; 截断 ⇒ 落在 cap-1 */
    out[(s.pos < cap) ? s.pos : (cap - 1u)] = '\0';
    return need;
}

br_size_t br_hexdump(const void *base, br_size_t len)
{
    if ((base == BR_NULL) && (len > 0u)) {
        return 0u;
    }

    char      line[HD_LINE_BUF];   /* 88 B: 逐行渲染, 不缓冲整段 */
    br_size_t total = 0u;

    const br_u8 *p = (const br_u8 *)base;
    for (br_size_t off = 0u; off < len; off += (br_size_t)BR_HEXDUMP_WIDTH) {
        br_size_t n = len - off;
        if (n > (br_size_t)BR_HEXDUMP_WIDTH) {
            n = (br_size_t)BR_HEXDUMP_WIDTH;
        }

        hd_sink_t s;
        s.out = line;
        s.cap = (br_size_t)HD_LINE_BUF;
        s.pos = 0u;
        hd_render_line(&s, (br_uintptr_t)base + off, p + off, n);

        /* 走 br_console_write: '\n' -> "\r\n" 的终端友好转换由 console 层负责 */
        br_console_write(line, s.pos);
        total += s.pos;
    }

    return total;
}

br_size_t br_hexdump_line_bytes(void)
{
    return (br_size_t)HD_LINE_BYTES;
}

int br_hexdump_init(void)
{
    /* 本插件无状态。不跨插件注册 trace 事件名: 声明面里没有
     * service/hexdump → service/trace 的 [[dep]] 边(这一点已写进 br_hexdump.h 的
     * init 注释), 跨插件调用会引入未声明的依赖。 */
    return 0;
}

/* =====================================================================
 * 自检(TC-DBG-02x)
 * ===================================================================== */

/* 空格片段: 用命名常量拼串, 避免"数不清的连续空格"这类抄写错误 */
#define HD_SP5 "     "       /* 5 空格 */
#define HD_SP8 "        "    /* 8 空格 */
_Static_assert(sizeof(HD_SP5) == 6u, "HD_SP5 必须是 5 个空格 + NUL");
_Static_assert(sizeof(HD_SP8) == 9u, "HD_SP8 必须是 8 个空格 + NUL");

/*
 * 头文件样例(基址 0x40088000, "Hello, world!\n\0\0")在**地址字段之后**的 71 字节。
 * 样例基址只是**格式**示例: 用例按负载真实地址独立渲染地址字段, 其余逐字节对齐
 * 这份后缀(声明面已在样例下方写明同一口径, 见 br_hexdump.h)。
 */
static const char HD_SAMPLE_SUFFIX[] =
    "  48 65 6c 6c 6f 2c 20 77"   /* 地址后 2 空格 + 第 1..8 组(第 8 组后留出加宽位) */
    "  6f 72 6c 64 21 0a 00 00"   /* 第 9..16 组 */
    "  |Hello, world!...|\n";     /* 组区末分隔 + 显式空格 + '|' + ASCII + '|' + '\n' */
_Static_assert(sizeof(HD_SAMPLE_SUFFIX) == 72u, "样例后缀必须是 71 字节 + NUL");

/* 3 字节尾行 {'A',0x1f,0x7f} 的 71 字节后缀: 13 个缺组用 3 空格占位, ASCII 列补齐 */
static const char HD_TAIL_SUFFIX3[] =
    "  "                             /* 地址后 2 空格 */
    "41 1f 7f "                      /* 3 组真实 hex */
    HD_SP5 HD_SP5 HD_SP5             /* 第 4..8 组占位: 5 × 3 = 15 空格 */
    " "                              /* 第 8 组后的加宽空格 */
    HD_SP8 HD_SP8 HD_SP8             /* 第 9..16 组占位: 8 × 3 = 24 空格 */
    " |"                             /* 组区末分隔 + 显式空格 + '|' */
    "A.." HD_SP5 HD_SP8              /* ASCII: 'A' + '.' + '.' + 13 空格 */
    "|\n";
_Static_assert(sizeof(HD_TAIL_SUFFIX3) == 72u, "3 字节尾行后缀必须是 71 字节 + NUL");

/* 记一条 DBGCONF 判据; 返回 0/1 便于累加失败数 */
static br_u32 hd_conf(br_bool ok, const char *tag, const char *what)
{
    br_log_info("[DBGCONF] %s %s %s", ok ? "PASS" : "FAIL", tag, what);
    return ok ? 0u : 1u;
}

/* 用例自带的地址渲染(与实现分开写, 两个实现必须一致) */
static void hd_tc_addr16(char *dst, br_uintptr_t v)
{
    for (br_u32 i = 0u; i < HD_ADDR_DIGITS; i++) {
        const br_u32 shift = (HD_ADDR_DIGITS - 1u - i) * 4u;
        dst[i] = HD_HEX[(v >> shift) & 0x0fu];
    }
}

static br_bool hd_tc_eq(const char *a, const char *b, br_size_t n)
{
    for (br_size_t i = 0u; i < n; i++) {
        if (a[i] != b[i]) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

static br_size_t hd_tc_len(const char *s)
{
    br_size_t n = 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int br_hexdump_selftest(void)
{
    static const br_u8 payload[16] = {
        (br_u8)'H', (br_u8)'e', (br_u8)'l', (br_u8)'l', (br_u8)'o', (br_u8)',',
        (br_u8)' ', (br_u8)'w', (br_u8)'o', (br_u8)'r', (br_u8)'l', (br_u8)'d',
        (br_u8)'!', (br_u8)'\n', 0u, 0u
    };
    static const br_u8 tail3[3] = { (br_u8)'A', 0x1fu, 0x7fu };

    static char got[HD_LINE_BUF];
    static char expect[HD_LINE_BUF];
    br_u32 fails = 0u;

    /* ---- TC-DBG-020: 16 字节行 + 3 字节尾行, 逐字节比对 ----
     * 头文件样例的基址(0x40088000 一类)只是**格式**示例: 目标镜像的 .text.boot
     * 入口在 0x40080000(link.ld), 那里不可写、内容也不能假定, 所以"在样例基址上
     * 读 16 字节再比样例"不成立。本用例改为: 负载放静态缓冲, 用**真实地址**渲染
     * 期望串的地址字段(用独立写的 hd_tc_addr16), 其余 71 字节与头文件样例的后缀
     * **逐字节相同** —— 声明面样例下方已写明这条口径。 */
    hd_tc_addr16(expect, (br_uintptr_t)(const void *)payload);
    for (br_size_t i = 0u; i < (sizeof(HD_SAMPLE_SUFFIX) - 1u); i++) {
        expect[HD_ADDR_DIGITS + i] = HD_SAMPLE_SUFFIX[i];
    }

    const br_size_t n16 = br_hexdump_to(got, sizeof got, payload, 16u);
    br_bool ok20 = (n16 == (br_size_t)HD_LINE_BYTES)
                   && hd_tc_eq(got, expect, (br_size_t)HD_LINE_BYTES);

    hd_tc_addr16(expect, (br_uintptr_t)(const void *)tail3);
    for (br_size_t i = 0u; i < (sizeof(HD_TAIL_SUFFIX3) - 1u); i++) {
        expect[HD_ADDR_DIGITS + i] = HD_TAIL_SUFFIX3[i];
    }

    const br_size_t n3 = br_hexdump_to(got, sizeof got, tail3, 3u);
    ok20 = ok20 && (n3 == (br_size_t)HD_LINE_BYTES)
           && hd_tc_eq(got, expect, (br_size_t)HD_LINE_BYTES);

    fails += hd_conf(ok20, "TC-DBG-020",
                     "16B 样例行 + 3B 尾行逐字节匹配(padding 与 ASCII 列)");

    /* ---- TC-DBG-021: snprintf 式截断语义 + 行字节数契约 ---- */
    static char small[10];
    const br_size_t need       = br_hexdump_to(small, sizeof small, payload, 16u);
    const br_size_t need_null  = br_hexdump_to(BR_NULL, 0u, payload, 16u);
    const br_size_t need_cons  = br_hexdump(payload, 16u);   /* 顺带留下 console 路径证据 */

    const br_bool ok21 = (need == (br_size_t)HD_LINE_BYTES)
                         && (small[9] == '\0') && (hd_tc_len(small) == 9u)
                         && (need_null == (br_size_t)HD_LINE_BYTES)
                         && (need_cons == (br_size_t)HD_LINE_BYTES)
                         && (br_hexdump_line_bytes() == (br_size_t)HD_LINE_BYTES)
                         && (br_hexdump_to(small, 0u, payload, 16u) == (br_size_t)HD_LINE_BYTES)
                         && (br_hexdump_to(small, sizeof small, BR_NULL, 16u) == 0u)
                         && (br_hexdump_to(small, sizeof small, payload, 0u) == 0u);

    fails += hd_conf(ok21, "TC-DBG-021",
                     "cap 截断 + NUL 结尾 + 只回报长度 + line_bytes=87");

    return (int)fails;
}

/* =====================================================================
 * 插件生命周期钩子(名字 = symbol_prefix(short) + 相; 由生成物
 * `build/gen/service/hexdump/plugin_desc.c` 引用; 设计 1-01 §9 / 3-05 §2)
 * 自带原型满足 -Wmissing-prototypes(钩子名由生成器推导, 不进对外头与 [[export]])。
 * ===================================================================== */
int hexdump_early_init(void);
int hexdump_init(void);
int hexdump_start(void);

/* EARLY 相: 无动作(本插件无状态、无注册)。 */
int hexdump_early_init(void)
{
    return 0;
}

/* LATE 相(Service 类别 ⇒ ② 完成点): 现有 init 的转调。 */
int hexdump_init(void)
{
    return br_hexdump_init();
}

/* START 相: 本服务没有需要"中断可用/可建线程"之后才做的事。 */
int hexdump_start(void)
{
    return 0;
}

