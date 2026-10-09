/*
 * brickOS prototype v0.2.0 — 通用工具(stdlib.h; runtime/posix 插件提供)
 *
 * 覆盖范围(与 `11-02` §2.5 对表):
 *   - `malloc/calloc/realloc/free` = **TR-B**: 直接落到 core 的 `br_malloc` 族
 *     (`br_mem.h` 的注释本来就写着"runtime/posix 的 libc stub 需要")。别名, 零开销。
 *   - `abs/labs/atoi/atol` = 本代**自写的小件**(各十来行)。
 *     严格说 `strtol` 族在 `11-02` 里属 TR-C("要么引 libc, 要么自写"), 这三个是
 *     自写路线里最划算的入口 —— 没有它们, 任何"读一行数字"的代码都得自己写解析。
 *     完整 `strtol`(带 endptr/基数/溢出)与 `qsort`/`bsearch` 仍**不在本代**。
 *
 * ★ **没有 `exit()`/`atexit()`**: 单地址空间里"进程退出"没有定义(设计 `1-01` §2.1
 *   的空位, `11-02` 的 Q-5)。`abort()` 有, 落到 core 的 `br_panic` —— 它不是"正常
 *   退出路径", 而是"这里出了不可恢复的错", 语义清晰。
 */
#ifndef BR_POSIX_STDLIB_H
#define BR_POSIX_STDLIB_H

#include <sys/types.h>

void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);

int   abs(int v);
long  labs(long v);
int   atoi(const char *s);
long  atol(const char *s);

/* 以 panic 形态终止(设计 `5-01` 的崩溃呈现; 不返回)。 */
void  abort(void) __attribute__((noreturn));

/* 本代没有环境变量模型 ⇒ 恒返回 NULL(如实回答"没有", 不做假的空环境)。 */
char *getenv(const char *name);

#endif /* BR_POSIX_STDLIB_H */
