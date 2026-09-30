/*
 * brickOS prototype v0.1.0 — 早期 console 契约(轮询 putc)
 *
 * 为什么这个头文件在 core 而不是 platform:
 *   设计文档 1-01 §8「平台能力三层模式」规定 —— **接口在 core / ISA 共享库 / 平台提供数据**。
 *   「早期 console 协议(轮询 putc)」的权威定义在 core 侧(3-01 §10 表),
 *   实现由 Platform 插件填(本例 = platform/src/aarch64/console_pl011.c)。
 *
 * 形态说明(不是 workaround):
 *   设计文档 1-01 §8 明确 console 双形态 —— **platform 早期 console(轮询, init 链打印)**
 *   → I/O 插件完整 tty(中断驱动, M2 注册 cdev)。v0.1.0 用的正是前者的形态,
 *   所以这里轮询、不注册设备是**设计内行为**, 不是缺陷。
 */
#ifndef BR_CORE_BR_CONSOLE_H
#define BR_CORE_BR_CONSOLE_H

#include <br/core/br_types.h>

/* 初始化早期 console。必须在任何 br_console_* 输出之前调用一次。
 * 归属: platform(在 br_plat_early_init 内)。 */
void br_console_init(void);

/* 输出一个字符(阻塞至发送寄存器可写)。实现负责 '\n' -> "\r\n" 的终端友好转换。 */
void br_console_putc(char c);

/* 输出定长缓冲区(可含 '\0', 不做字符串解释)。 */
void br_console_write(const char *buf, br_size_t len);

/* 输出 NUL 结尾字符串。 */
void br_console_puts(const char *s);

#endif /* BR_CORE_BR_CONSOLE_H */
