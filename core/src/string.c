/*
 * brickOS prototype v0.1.0 — 编译器支持例程(memcpy/memmove/memset/memcmp)
 *
 * ★ 这不是"实现 libc", 而是**freestanding 代码生成的硬性义务**:
 *   即使开了 `-ffreestanding -fno-builtin`, GCC 的**中端/后端**仍会把
 *   "整块搬移"(结构体赋值、结构体清零、大数组初始化)降低成对
 *   `memcpy/memmove/memset` 的调用 —— `-fno-builtin` 关掉的是**内建识别**,
 *   不是 lowering。实测踩到过一次: `br_mem_layout()` 里的一句结构体赋值被降成
 *   `bl memcpy`, 于是 `-nostdlib` 链接直接红(`undefined reference to memcpy`)。
 *   所以本文件是**链接能否成立**的前提, 不是可有可无的加速件。
 *
 * 归属说明(为什么在 core 而不是 runtime/posix):
 *   设计侧 POSIX 符号面归 POSIX 运行时(D18; 本树里是 `runtime/posix` 插件, ADR-0015)。
 *   ★ **本文件不会因为那个插件到位而消失** —— 原因是**相位**, 不是排期:
 *   core 在 EARLY(还开着 MMU 建堆)就要 `memset` 控制块, 而 `runtime/posix` 是 LATE 相的
 *   服务。符号归一(把 mem* 也搬进那个插件)在"core 早于服务"这条相位事实面前做不到 ——
 *   这一条写在 ADR-0014 §4 的欠账里, 不是遗漏。
 *   本文件因此刻意**最小**: 只提供编译器会发射的那四个, 签名严格照 AAPCS64/标准 C,
 *   不导出任何别的 libc 名字(其余 `str*` 归 `runtime/posix`, 两边不重叠)。
 *
 * 安全纪律(与 `-mstrict-align` 的关系): 本文件在**开 MMU 之前**也会被调用
 *   (`br_mem_init` 建控制块时要清零), 那时全部访存按 Device-nGnRnE 处理 ⇒
 *   **非对齐的宽访问会取 Alignment fault**。所以宽访问只在**指针对齐到 8 字节**时
 *   使用, 其余一律逐字节 —— 这条不是优化取舍, 是"两种内存模型下都正确"的要求。
 */
#include "string_internal.h"

#if defined(__GNUC__)
/*
 * 防"优化把本函数自己变成对 memcpy 的调用"(loop-distribute-patterns 在 -O2 会做这件事,
 * 那是无限递归 + 链接期自引用)。这是 GCC 特有属性, 故包在 __GNUC__ 里。
 */
#  define BR_NO_LOOP_PATTERN __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#  define BR_NO_LOOP_PATTERN
#endif

static br_bool is_word_aligned(const void *p)
{
    return (((br_uintptr_t)p & 7u) == 0u) ? BR_TRUE : BR_FALSE;
}

/*
 * ★ **8 字节搬运绝不能写成 `__builtin_memcpy(..., 8)`**(实测踩过, ADR-0003 §5):
 *   在本文件的编译组合下(`-ffreestanding -fno-builtin` + 函数上的
 *   `optimize("no-tree-loop-distribute-patterns")`), GCC 会把那次定长 8 的
 *   `__builtin_memcpy` **降成对 `memcpy` 的 libcall** —— 于是 memcpy 内部
 *   `bl memcpy`, 只要 n ≥ 8 且指针对齐就**无限递归**, 栈无界增长、压穿启动栈落进
 *   .bss 的页表, 表现为"开机即翻译 fault"。
 *   这里改用**显式宽访问**: `aligned(1)` 让编译器不假设对齐(地址对齐已由
 *   `is_word_aligned` 保证), `may_alias` 避开严格别名 UB; 两者都不产生调用。
 *   验收判据 = `objdump -d <本 .o> | grep -A40 '<memcpy>:' | grep -c 'bl.*memcpy'`
 *   必须为 0(见核验记录)。
 */
typedef br_u64 br_word_u __attribute__((aligned(1), may_alias));

BR_NO_LOOP_PATTERN void *memcpy(void *dst, const void *src, br_size_t n)
{
    br_u8       *d = (br_u8 *)dst;
    const br_u8 *s = (const br_u8 *)src;
    br_size_t    i = 0u;

    if (is_word_aligned(dst) && is_word_aligned(src)) {
        while ((i + 8u) <= n) {
            const br_word_u w = *(const br_word_u *)(const void *)(s + i);
            *(br_word_u *)(void *)(d + i) = w;
            i += 8u;
        }
    }
    while (i < n) {
        d[i] = s[i];
        i++;
    }
    return dst;
}

BR_NO_LOOP_PATTERN void *memmove(void *dst, const void *src, br_size_t n)
{
    br_u8       *d = (br_u8 *)dst;
    const br_u8 *s = (const br_u8 *)src;

    /* 重叠且目标在高处 ⇒ 从尾向头逐字节(d 与 s 都是字节视图, 比较良定义) */
    if ((d > s) && (d < (s + n))) {
        br_size_t i = n;
        while (i > 0u) {
            i--;
            d[i] = s[i];
        }
        return dst;
    }
    return memcpy(dst, src, n);
}

BR_NO_LOOP_PATTERN void *memset(void *dst, int c, br_size_t n)
{
    br_u8    *d = (br_u8 *)dst;
    br_size_t i = 0u;
    const br_u8 b = (br_u8)c;

    if (is_word_aligned(dst)) {
        br_u64 w = 0u;
        for (br_u32 k = 0u; k < 8u; k++) {
            w |= ((br_u64)b) << (8u * k);
        }
        while ((i + 8u) <= n) {
            *(br_word_u *)(void *)(d + i) = w;   /* 显式宽访问(不用 __builtin_memcpy, 见上) */
            i += 8u;
        }
    }
    while (i < n) {
        d[i] = b;
        i++;
    }
    return dst;
}

int memcmp(const void *a, const void *b, br_size_t n)
{
    const br_u8 *x = (const br_u8 *)a;
    const br_u8 *y = (const br_u8 *)b;

    for (br_size_t i = 0u; i < n; i++) {
        if (x[i] != y[i]) {
            return (x[i] < y[i]) ? -1 : 1;
        }
    }
    return 0;
}
