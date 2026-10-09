/*
 * brickOS prototype v0.2.0 — 字符串(string.h; runtime/posix 插件提供)
 *
 * ★ `memcpy/memmove/memset/memcmp` **不是本插件的定义** —— 它们是 core 的
 *   `core/src/string.c` 提供的符号(编译器 lowering 的硬性义务: 即使 -ffreestanding,
 *   GCC 也会把整块搬移降成对 memcpy 的调用, 见那个文件的注释)。本头只是**声明**它们,
 *   于是源码可以 `#include <string.h>` 而符号只有一份。
 *   ⇒ 设计里"POSIX 运行时到位后删掉 core/src/string.c 以归一符号"这句话在本代**做不到**:
 *     core 在 EARLY 相就要用 memcpy, 而 runtime/posix 是 LATE 相的服务。这一条登记在
 *     ADR-0014 的欠账里(不是遗漏, 是相位事实)。
 *
 * ★ `str*` 族是**本件定义**的(TR-C 里"自写"的那条路): 规模小, 而缺了它 FILE 层与
 *   任何字符串处理都写不了。**不在本代**: `strtok/strstr/strspn` 等"高级"函数与
 *   所有 locale 相关面。
 */
#ifndef BR_POSIX_STRING_H
#define BR_POSIX_STRING_H

#include <sys/types.h>

/* ---- core 提供(本件只声明) ---- */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* ---- 本件提供 ---- */
size_t strlen (const char *s);
char  *strcpy (char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat (char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp (const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr (const char *s, int c);
char  *strrchr(const char *s, int c);
void  *memchr (const void *s, int c, size_t n);
char  *strerror(int err);

#endif /* BR_POSIX_STRING_H */
