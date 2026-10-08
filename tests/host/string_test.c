/*
 * brickOS prototype v0.1.0 — 编译器支持例程(compiler support routines)的**宿主**用例
 *
 * 为什么这四个函数值得单独一条宿主用例:
 *   它们是 `core/src/string.c` 的产物, 属"编译器会自己发射调用目标"的地基 ——
 *   一旦写错, **症状出现在完全无关的地方**(实测: memcpy 内部 `bl memcpy` 自递归 ⇒
 *   无限递归压穿启动栈 ⇒ 异常帧落进页表 ⇒ 表面症状是"开 MMU 后开机即翻译 fault",
 *   排查绕了三层, 见 ADR-0003 §5.2)。
 *   宿主侧能直接验证它们: 语义可以逐字节对拍参考实现, 越界写可以用哨兵字节抓,
 *   而**自递归在宿主上会立刻爆栈**(所以这条用例本身就是那个 bug 的回归判据)。
 *
 * 与 `check-string` 门禁的分工: 门禁看**反汇编**(有没有对自身的 bl), 本用例看**语义**
 * (搬对了没有、越界了没有) —— 一个抓"结构性自递归", 一个抓"行为不正确"。
 */
#include <br/core/br_types.h>

#include "string_internal.h"

#include <stdio.h>
#include <string.h>

static int s_pass;
static int s_fail;

static void rep(int cond, const char *tag, const char *desc)
{
    if (cond) {
        s_pass++;
        printf("[STRINGTEST] PASS %s %s\n", tag, desc);
    } else {
        s_fail++;
        printf("[STRINGTEST] FAIL %s %s\n", tag, desc);
    }
}

/*
 * 缓冲区布局: [前哨兵 PAD][载荷 BUF][后哨兵 PAD]。
 * 前端 + 后端都留哨兵, 于是"越界 1 字节"也抓得到(与本项目对红区的要求同源)。
 */
#define PAD 16u
#define BUF 2048u
#define TOTAL (PAD + BUF + PAD)

static br_u8 s_src[TOTAL];
static br_u8 s_dst[TOTAL];
static br_u8 s_ref[TOTAL];

static void fill(br_u8 *b, br_size_t n, br_u32 seed)
{
    br_u32 x = seed;
    for (br_size_t i = 0; i < n; i++) {
        x = (x * 1103515245u) + 12345u;
        b[i] = (br_u8)(x >> 16);
    }
}

/* 哨兵: 前 PAD 个字节置 0xC3, 从 `end` 起的后 PAD 个字节置 0x3C。
 * `end` = 载荷结束位置(相对缓冲区头), 由调用方按"真正会被写到哪"给出。 */
static void pad_set(br_u8 *b, br_size_t end)
{
    for (br_size_t i = 0; i < PAD; i++) {
        b[i] = 0xC3;
        b[end + i] = 0x3C;
    }
}

static int pad_ok(const br_u8 *b, br_size_t end)
{
    for (br_size_t i = 0; i < PAD; i++) {
        if (b[i] != 0xC3 || b[end + i] != 0x3C) {
            return 0;
        }
    }
    return 1;
}

/* 覆盖 0/1/7/8/17/63/64/257/1024: 前三个走字节路径, 8 的倍数走宽访问路径 */
static const br_size_t k_sizes[] = { 0u, 1u, 7u, 8u, 9u, 15u, 16u, 17u, 63u, 64u, 257u, 1024u };
#define NSIZES (sizeof k_sizes / sizeof k_sizes[0])

static void case_memcpy(void)
{
    br_u8 *src = s_src + PAD;
    br_u8 *dst = s_dst + PAD;
    int ok = 1;

    fill(s_src, sizeof s_src, 0x1234u);

    for (br_size_t k = 0; k < NSIZES; k++) {
        const br_size_t n = k_sizes[k];

        /* ① 双对齐(走宽访问路径): 内容 + 返回值 + 两侧哨兵 */
        memset(s_dst, 0, sizeof s_dst);
        pad_set(s_dst, PAD + n);
        void *r = memcpy(dst, src, n);
        ok = ok && (r == (void *)dst);
        ok = ok && (memcmp(dst, src, n) == 0);
        ok = ok && pad_ok(s_dst, PAD + n);

        /* ② 目标非 8 对齐(强制字节路径); 哨兵要按"真正写到哪"设 */
        for (br_size_t off = 1u; off <= 5u; off += 2u) {
            memset(s_dst, 0, sizeof s_dst);
            pad_set(s_dst, PAD + off + n);
            (void)memcpy(dst + off, src, n);
            ok = ok && (memcmp(dst + off, src, n) == 0);
            ok = ok && pad_ok(s_dst, PAD + off + n);
        }

        /* ③ 源非 8 对齐(目标仍对齐) */
        memset(s_dst, 0, sizeof s_dst);
        pad_set(s_dst, PAD + n);
        (void)memcpy(dst, src + 3u, n);
        ok = ok && (memcmp(dst, src + 3u, n) == 0);
        ok = ok && pad_ok(s_dst, PAD + n);

        /* ④ 与参考实现对拍(独立缓冲 + 逐字节参考拷贝): 重新做一次干净的对齐拷贝 */
        memset(s_ref, 0, sizeof s_ref);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[PAD + i] = src[i];
        }
        memset(s_dst, 0, sizeof s_dst);
        (void)memcpy(dst, src, n);
        ok = ok && (memcmp(dst, s_ref + PAD, n) == 0);
        ok = ok && (memcmp(s_dst, s_ref, PAD + n) == 0);
    }
    rep(ok, "STR-001", "memcpy 12 种长度 × 对齐/非对齐: 内容/返回值/两侧哨兵 全对");

    /* 大块(整段载荷)搬运: 只许写 [PAD, PAD+BUF) */
    memset(s_dst, 0, sizeof s_dst);
    pad_set(s_dst, PAD + BUF);
    (void)memcpy(dst, src, BUF);
    rep(pad_ok(s_dst, PAD + BUF) && (memcmp(dst, src, BUF) == 0), "STR-002",
        "memcpy 2 KiB 整段搬运: 内容一致且未越界");
}

static void case_memmove(void)
{
    br_u8 *base = s_dst + PAD;
    int ok = 1;

    for (br_size_t k = 0; k < NSIZES; k++) {
        const br_size_t n = k_sizes[k];
        if (n == 0u) {
            continue;
        }

        /* ① 目标低于源(前向重叠) */
        fill(s_dst, sizeof s_dst, 0xABCDu);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[i] = base[64 + i];
        }
        (void)memmove(base, base + 64, n);
        ok = ok && (memcmp(base, s_ref, n) == 0);

        /* ② 目标高于源(后向重叠: 必须从尾向头搬, 否则被自覆盖) */
        fill(s_dst, sizeof s_dst, 0x5678u);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[i] = base[i];
        }
        (void)memmove(base + 64, base, n);
        ok = ok && (memcmp(base + 64, s_ref, n) == 0);

        /* ③ 部分重叠: 1 字节错位 */
        fill(s_dst, sizeof s_dst, 0x9999u);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[i] = base[i];
        }
        (void)memmove(base + 1, base, n);
        ok = ok && (memcmp(base + 1, s_ref, n) == 0);

        /* ④ 无重叠: 等价于拷贝(源用独立缓冲) */
        fill(s_dst, sizeof s_dst, 0x2468u);
        (void)memmove(base, s_src + PAD, n);
        ok = ok && (memcmp(base, s_src + PAD, n) == 0);

        /* ⑤ n == 0 不许碰任何字节 */
        if (n != 0u) {
            continue;
        }
    }

    /* ⑥ 重叠边界的"贴边"情形: 目标 = 源 + 1, 且反向; 与参考对拍 */
    for (br_size_t k = 0; k < NSIZES; k++) {
        const br_size_t n = k_sizes[k];
        if (n == 0u) {
            continue;
        }
        fill(s_dst, sizeof s_dst, 0x1357u);
        for (br_size_t i = 0; i < n; i++) {
            s_ref[i] = base[i];
        }
        (void)memmove(base + 1, base, n);      /* 目标高: 必须反向 */
        ok = ok && (memcmp(base + 1, s_ref, n) == 0);
    }
    rep(ok, "STR-003", "memmove 三种重叠方向 + 错位 + 无重叠: 与参考逐字节一致");

    /* 零长度: 缓冲区一个字节都不许变 */
    fill(s_dst, sizeof s_dst, 0x7777u);
    memset(s_ref, 0, sizeof s_ref);
    memcpy(s_ref, s_dst, sizeof s_dst);
    (void)memmove(base, base + 32, 0u);
    rep(memcmp(s_dst, s_ref, sizeof s_dst) == 0, "STR-004",
        "memmove(n=0) 不写任何字节");
}

static void case_memset(void)
{
    br_u8 *dst = s_dst + PAD;
    const br_u8 pats[] = { 0x00u, 0x5Au, 0xA5u, 0xFFu, 0x01u };
    int ok = 1;

    for (br_size_t p = 0; p < sizeof pats / sizeof pats[0]; p++) {
        for (br_size_t k = 0; k < NSIZES; k++) {
            const br_size_t n = k_sizes[k];

            /* ① 对齐目标: 哨兵 + 逐字节比对参考 */
            memset(s_dst, 0, sizeof s_dst);
            pad_set(s_dst, PAD + n);
            void *r = memset(dst, pats[p], n);
            ok = ok && (r == (void *)dst);
            ok = ok && pad_ok(s_dst, PAD + n);
            for (br_size_t i = 0; i < n; i++) {
                ok = ok && (dst[i] == pats[p]);
            }

            /* ② 非 8 对齐目标: 同样是字节精确 + 不越界 */
            memset(s_dst, 0, sizeof s_dst);
            pad_set(s_dst, PAD + 3u + n);
            (void)memset(dst + 3, pats[p], n);
            ok = ok && pad_ok(s_dst, PAD + 3u + n);
            for (br_size_t i = 0; i < n; i++) {
                ok = ok && (dst[3 + i] == pats[p]);
            }
        }
    }
    rep(ok, "STR-005", "memset 5 种填充值 × 12 种长度 × 对齐/非对齐: 字节精确且未越界");
}

static void case_memcmp(void)
{
    static br_u8 a[64];
    static br_u8 b[64];
    int ok = 1;

    fill(a, sizeof a, 0x1111u);
    memcpy(b, a, sizeof a);
    ok = ok && (memcmp(a, b, sizeof a) == 0);
    ok = ok && (memcmp(a, b, 0u) == 0);                 /* n == 0 ⇒ 恒等 */
    ok = ok && (memcmp(a, a, sizeof a) == 0);

    b[0] ^= 0x01u;
    ok = ok && (memcmp(a, b, sizeof a) != 0);
    ok = ok && (memcmp(b, a, sizeof a) != 0);
    ok = ok && ((memcmp(a, b, sizeof a) < 0) != (memcmp(b, a, sizeof a) < 0));  /* 反对称 */

    memcpy(b, a, sizeof a);
    b[63] ^= 0x80u;                                     /* 只在最后一字节不同 */
    ok = ok && (memcmp(a, b, 64u) != 0);
    ok = ok && (memcmp(a, b, 63u) == 0);                /* 前缀相同 ⇒ 只看 n */

    rep(ok, "STR-006", "memcmp 相等/不等/前缀/反对称/n=0 语义正确");
}

int main(void)
{
    printf("[STRINGTEST] compiler support routines (core/src/string.c) on host\n");

    case_memcpy();
    case_memmove();
    case_memset();
    case_memcmp();

    printf("[STRINGTEST] SUMMARY pass=%d fail=%d total=%d\n", s_pass, s_fail, s_pass + s_fail);
    return (s_fail == 0) ? 0 : 1;
}
