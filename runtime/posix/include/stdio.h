/*
 * brickOS prototype v0.2.0 — standard I/O (stdio.h; provided by the runtime/posix plugin)
 *
 * ** This generation has no printf family** (`11-02` §2.5 rates it TR-C): formatted output needs a
 *    `vsnprintf`, and that is part of "libc selection" (an open question in design `11-01` §3 -- picolibc /
 *    newlib / a self-made subset is not decided yet). core does have **three private formatting subsets**
 *    (`core/src/log.c`, `panic.c`, `service/hexdump`), each serving one call site and exporting nothing.
 *    The proper order is to extract a `vsnprintf` first and then build stdio -- that is the next cut.
 *    => this generation provides the **stream layer** (`FILE*` + buffering + positioning), not the **formatting layer**.
 *
 * * Buffering policy (declared honestly so callers need not guess):
 *     - **write** is buffered (default 512 B, `setvbuf` can replace/disable it), flushed when full, and
 *       `fflush`/`fclose`/positioning operations flush first;
 *     - **read** is unbuffered (`fread` passes straight through to `read`). Reason: a read buffer introduces
 *       hidden state ("how much was pre-read") that correctness of mixing `fseek`/`ftell` with `read` depends on
 *       entirely -- not worth the semantic risk for that little performance in v1 (same discipline as SD-3 in
 *       vfs, "no cache favors correctness").
 */
#ifndef BR_POSIX_STDIO_H
#define BR_POSIX_STDIO_H

#include <sys/types.h>

#define BR_POSIX_EOF      (-1)
#define BR_POSIX_BUFSIZ   512

typedef struct br_posix_file FILE;

/* Standard streams. v1 does **not auto-open** 0/1/2 (those three fds every POSIX process starts with belong to
 * the "process model", and this generation has no process model) => these three macros are **symbols**, with
 * value BR_NULL until runtime/posix's init installs them. With no stdio consumer they are just NULL --
 * dereferencing them panics instead of silently corrupting something. */
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

/* Namespace: **declaration** only (implementation lives in posix.c / posix_dir.c) */
int remove(const char *path);
int rename(const char *old_path, const char *new_path);

/* ---- formatting layer: **not provided** in this generation (see the file header) ----
 * `printf`/`fprintf`/`snprintf`/`vprintf`/`scanf` are all absent. Declaring what cannot link is worse than
 * not declaring it (the design's "subset honesty obligation": unimplemented items should surface at **link time**). */

#endif /* BR_POSIX_STDIO_H */
