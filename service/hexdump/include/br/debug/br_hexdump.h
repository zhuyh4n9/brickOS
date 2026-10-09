/*
 * service/hexdump — 十六进制 + ASCII 呈现原语(对外声明面)
 *
 * 设计出处: `docs/5-debug/5-01-debug.md` §2(debug bridge 的 `MEMRD`, 主机侧
 * `brickie dbg`)与 §3(ramdump 输出)。本插件是**最底层的呈现原语**: 把一段内存
 * 变成行, 给 dump / bridge / panic 报告共用; 它**不做边界判定**(那是 service/dump
 * 的职责 —— 谁读内存谁负责问 region 表)。
 *
 * ★ 格式是**契约**(用例 TC-DBG-020 逐字节断言), 所以在这里写死:
 *
 *   每 16 字节一行, 行 = <addr:16 位小写 hex> + 两个空格
 *                      + 16 组 `<2 位 hex>`(组间 1 空格; 第 8 组后**多一个空格**)
 *                      + 两个空格 + '|' + 16 个 ASCII 字符 + '|' + '\n'
 *   不足 16 字节的尾行: 缺的组用**3 个空格**占位(保持 '|' 列对齐);
 *   ASCII 列: 0x20..0x7E 原样, 其余('.') —— 含 '\n'/'\0'。
 *
 * 完整样例(16 字节输入 "Hello, world!\n\0\0" 落在基址 0x40088000 的静态负载上):
 *
 *   0000000040088000  48 65 6c 6c 6f 2c 20 77  6f 72 6c 64 21 0a 00 00  |Hello, world!...|
 *
 * ⚠ 样例里的基址只是**格式**示例(地址字段恒为 16 位小写 hex); 用例 `TC-DBG-020`
 *   会用被测负载的**真实地址**渲染地址字段, 并逐字节比对剩余部分 —— 因为目标上
 *   不可能"在 .text.boot 入口 0x40080000 读 16 字节"(那里不可写且内容未知)。
 *
 * 返回值语义照 snprintf: `br_hexdump_to` 返回"**需要**写入的字节数(不含结尾 NUL)",
 * 写盘/截断不影响返回值 —— 这样调用方能用 `ret >= cap` 判断截断(用例 TC-DBG-021)。
 */
#ifndef BR_DEBUG_BR_HEXDUMP_H
#define BR_DEBUG_BR_HEXDUMP_H

#include <br/core/br_types.h>

#define BR_HEXDUMP_WIDTH   16u   /* 每行字节数(契约; 格式见文件头) */

/* LATE 相 init: 本插件无状态, 只把内部状态复位并返回 0。
 * (不跨插件注册 trace 事件名: 声明面没有 backtrace/hexdump → service/trace 的 `[[dep]]`
 *  边, 见本插件 README 的"声明面欠账"。) */
int br_hexdump_init(void);

/* 自检入口(TC-DBG-02x)已移到 `src/hexdump_selftest.c`(ADR-0010): 生成物按 symbol_prefix
 * 发 `.selftest = hexdump_selftest`, 由 core 在全部 start 之后统一驱动。
 * ★ 测试入口**不进**本插件的 golden 接口面 —— 否则改一条用例就成了对外接口变更
 *   (接口 hash 覆盖的正是声明面), 与"自检与生产分离"的裁定冲突。 */

/*
 * 渲染到缓冲: 写最多 `cap` 字节(含结尾 NUL)。返回**需要**的字节数(不含 NUL)。
 *   cap == 0 或 out == NULL ⇒ 只回报所需长度(可用于两段式调用)。
 */
br_size_t br_hexdump_to(char *out, br_size_t cap, const void *base, br_size_t len);

/* 渲染到 console(逐行 putc; 不分配、不缓冲整段)。返回渲染的字节数。 */
br_size_t br_hexdump(const void *base, br_size_t len);

/* 一行的字节数(= 一次 `br_hexdump_to` 在 len<=16 时的返回值, 不含 NUL);
 * 供 dump 的调用方预算缓冲。 */
br_size_t br_hexdump_line_bytes(void);

#endif /* BR_DEBUG_BR_HEXDUMP_H */
