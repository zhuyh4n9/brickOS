/*
 * brickOS prototype v0.2.0 — service/hexdump **插件私有**内部契约(不导出, 不在 include/)
 *
 * 给谁看: 本插件自己的 `src/hexdump_selftest.c`。放在 `src/`(而不是 `include/`)是刻意的
 * —— `include/` 下的一切都是**插件对外面**(会被 `brickie check` 的接口域治理, 也进
 * `descriptor_include` 的选用范围); 而这里的三个**只有自检与生产实现**需要。
 *
 * ## 为什么自检要读内部
 *   TC-DBG-020/021 的判据是**逐字节比对整行**, 于是用例要自己的行缓冲与"期望长度":
 *     - `HD_LINE_BUF` 是行缓冲容量(用例的 got/expect 按它开静态数组);
 *     - `HD_LINE_BYTES` 是"满行 87 字节"这条**格式契约**的机器可读形态, 用例用它做
 *       长度断言(库里的 `br_hexdump_line_bytes()` 是运行期取值, 开不了静态数组);
 *     - `HD_ADDR_DIGITS` 是用例**自己**渲染地址字段时的位宽(它刻意与实现分开写,
 *       两个实现必须一致, 见 hexdump_selftest.c 的 hd_tc_addr16)。
 *   把这三个搬进公开头会更糟: 格式常量进 golden 接口面, 以后调行宽就是接口变更。
 *
 * ## 边界纪律(与 framework/vfs-core 的 `src/vfs_internal.h` 同源)
 *   只暴露自检真用到的三个**常量**(搬自生产实现的私有宏, 值一字未改), 不暴露任何
 *   可变状态; 行渲染逻辑仍全部 `static`。用例不共用实现里的 digit 表 —— 用例自带一份,
 *   否则"实现把表写错"时用例会跟着一起错。
 */
#ifndef BR_DEBUG_BR_HEXDUMP_INTERNAL_H
#define BR_DEBUG_BR_HEXDUMP_INTERNAL_H

#include <br/core/br_types.h>
#include <br/debug/br_hexdump.h>

/* 地址字段固定 16 位小写 hex(生产实现里的 `HD_ADDR_DIGITS`) */
#define HD_ADDR_DIGITS   16u

/* 满行字节数 = 16 + 2 + 49 + 1 + 1 + 16 + 1 + 1 = 87(含行尾 '\n'); 见 br_hexdump.h 的
 * 格式契约。生产实现在本头之外还有一条 `_Static_assert(HD_LINE_BYTES == 87u, ...)`,
 * 把这个数字钉在编译期。 */
#define HD_LINE_BYTES    (HD_ADDR_DIGITS + 2u                                   \
                          + (BR_HEXDUMP_WIDTH * 3u + 1u)                        \
                          + 1u + 1u + BR_HEXDUMP_WIDTH + 1u + 1u)

/* 一行渲染缓冲的容量(87 + NUL 余量); 用例的静态缓冲按它开 */
#define HD_LINE_BUF      (HD_LINE_BYTES + 1u)

#endif /* BR_DEBUG_BR_HEXDUMP_INTERNAL_H */
