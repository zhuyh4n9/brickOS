/*
 * brickOS prototype v0.2.0 — 标准 I/O(stdio.h; runtime/posix 插件提供)
 *
 * ★★ **本代没有 printf 家族**(`11-02` §2.5 把它判为 TR-C): 格式化输出需要一个
 *    `vsnprintf`, 而那是"libc 选型"的一部分(设计 `11-01` §3 的开放问题 —— picolibc /
 *    newlib / 自研子集尚未拍板)。core 里倒是有**三份私有的格式化子集**
 *    (`core/src/log.c`、`panic.c`、`service/hexdump`), 它们各自服务一个调用点、都不导出。
 *    规范做法是先提炼一个 `vsnprintf` 再做 stdio —— 那是下一刀。
 *    ⇒ 本代提供的是**流层**(`FILE*` + 缓冲 + 定位), 不是**格式化层**。
 *
 * ★ 缓冲策略(如实声明, 免得调用方猜):
 *     - **写**有缓冲(默认 512 B, `setvbuf` 可换/可关), 满则落, `fflush`/`fclose`/定位
 *       操作都会先落盘;
 *     - **读**不缓冲(`fread` 直通 `read`)。理由: 读缓冲会引入"预读了多少"的隐藏状态,
 *       而 `fseek`/`ftell` 与 `read` 混用时的正确性全靠它 —— v1 不值得为这点性能冒
 *       语义风险(与 vfs 的 SD-3"无 cache 优先正确"同一条纪律)。
 */
#ifndef BR_POSIX_STDIO_H
#define BR_POSIX_STDIO_H

#include <sys/types.h>

#define BR_POSIX_EOF      (-1)
#define BR_POSIX_BUFSIZ   512

typedef struct br_posix_file FILE;

/* 标准流。v1 **不自动打开** 0/1/2(POSIX 进程启动就有的那三个 fd 属"进程模型",
 * 而本代没有进程模型)⇒ 这三个宏是**符号**, 值为 BR_NULL 直到 runtime/posix 的 init 装上。
 * 没有 stdio 消费者时它们就是 NULL —— 解引用即 panic, 不会静默写坏东西。 */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF  BR_POSIX_EOF

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int   fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int   fseek(FILE *f, long off, int whence);
long  ftell(FILE *f);
void  rewind(FILE *f);
int   fflush(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);
int   fileno(FILE *f);
int   setvbuf(FILE *f, char *buf, int mode, size_t size);

char *fgets(char *buf, int n, FILE *f);
int   fputs(const char *s, FILE *f);
int   fputc(int c, FILE *f);
int   fgetc(FILE *f);
int   puts(const char *s);
int   putchar(int c);

#define _IOFBF  0
#define _IOLBF  1
#define _IONBF  2

/* 名字空间:**声明**而已(实现落在 posix.c / posix_dir.c) */
int remove(const char *path);
int rename(const char *old_path, const char *new_path);

/* ---- 格式化层: 本代**不提供**(见文件头)----
 * `printf`/`fprintf`/`snprintf`/`vprintf`/`scanf` 都不在此列。写了却链不上比不写更糟
 * (设计的"子集诚实义务": 未实现项应在**链接期**暴露)。 */

#endif /* BR_POSIX_STDIO_H */
