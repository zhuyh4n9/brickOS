/*
 * brickOS prototype v0.2.0 — service/hexdump 的**自检套件**
 * (service/hexdump/src/hexdump_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与生产代码分离)。
 * 本文件是从 `hexdump.c` **原样搬来**的用例(TC-DBG-020/021): case id、断言与日志串
 * 一字未改 —— 门禁 `tests/gates.toml` 按 `[DBGCONF] PASS/FAIL` 与 require_tags 判红绿。
 *
 * ## 边界
 *   只经 `br/debug/br_hexdump.h` 的**公开 API** 驱动(hexdump_to / hexdump /
 *   hexdump_line_bytes);
 *   **例外只有一处**: 格式常量(行缓冲容量 / 满行字节数 / 地址位宽)经
 *   `src/hexdump_internal.h` 取用 —— 它们是生产实现私有的宏, 搬进公开头会让格式
 *   常量进 golden 接口面, 理由见该头文件。
 *
 * ## 用例刻意"与实现分开写"
 *   地址字段由本文件的 `hd_tc_addr16` 独立渲染, digit 表也自带一份(不共用实现的
 *   `HD_HEX`)—— 两个实现必须一致才有判据意义, 共用一张表会让"实现把表写错"时
 *   用例跟着一起错。样例后缀(HD_SAMPLE_SUFFIX / HD_TAIL_SUFFIX3)同样是从
 *   `br_hexdump.h` 的格式契约抄下来的独立期望串。
 *
 * ## 与插件管理器的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[DBGCONF] PASS/FAIL ...`)。
 *   **失败不停机** —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[DBGCONF] FAIL`)。
 *   入口名 `hexdump_selftest` 由生成物按 `symbol_prefix` 推导(见 plugin_desc.c 的
 *   `.selftest`), 不是本插件的对外 API。
 */
#include <br/core/br_types.h>
#include <br/core/br_log.h>
#include <br/debug/br_hexdump.h>

#include "hexdump_internal.h"

/* 入口原型(声明面集中在生成物里; 这里重述只为满足全局函数的原型纪律)。 */
int hexdump_selftest(void);

/* 用例自带的 digit 表(与实现的 HD_HEX 分开写, 见文件头) */
static const char HD_TC_HEX[] = "0123456789abcdef";

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
        dst[i] = HD_TC_HEX[(v >> shift) & 0x0fu];
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

int hexdump_selftest(void)
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
