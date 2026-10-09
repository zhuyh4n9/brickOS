/*
 * brickOS prototype v0.1.0 — 编译器支持例程的内部原型(core 私有)
 *
 * 这四个符号**不是 native API**(不进 `3-01` 的 48 函数面, 也不是插件可见面):
 * 它们是 GCC 会自己发射的调用目标, 属"链接能否成立"的地基。放在 core/src 下的
 * 内部头里, 是为了满足 `-Wmissing-prototypes`(定义前必须有原型), 同时**不**把
 * 它们混进 `core/include/br/core/` 的契约头 —— 契约头里出现的名字都算对外面。
 *
 * ★ 符号归一**做不到**(ADR-0014 §4): core 在 EARLY 相就要 `memset`, 而 POSIX 运行时是
 *   LATE 相的服务 ⇒ 那四个符号只能留在 core。本文件因此长期存在, 不是临时件。
 */
#ifndef BR_CORE_STRING_INTERNAL_H
#define BR_CORE_STRING_INTERNAL_H

#include <br/core/br_types.h>

void *memcpy (void *dst, const void *src, br_size_t n);
void *memmove(void *dst, const void *src, br_size_t n);
void *memset (void *dst, int c, br_size_t n);
int   memcmp (const void *a, const void *b, br_size_t n);

#endif /* BR_CORE_STRING_INTERNAL_H */
