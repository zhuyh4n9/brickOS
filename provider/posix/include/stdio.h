/*
 * Stream layer only (FILE* + buffering + positioning); no printf family this generation, since
 * formatted output needs a vsnprintf and libc selection is undecided.
 * Write is buffered (default 512 B; setvbuf may replace/disable it), flushed when full and on
 * fflush/fclose/positioning. Read is unbuffered (fread passes through to read) -- no hidden
 * pre-read state for fseek/ftell to account for.
 */
#ifndef BR_POSIX_STDIO_H
#define BR_POSIX_STDIO_H

#include <sys/types.h>

#define BR_POSIX_EOF      (-1)
#define BR_POSIX_BUFSIZ   512

typedef struct br_posix_file FILE;

/*
 * Standard streams are **accessor calls**, not exported objects (ADR-0038 / R2: a plugin's surface
 * carries functions, types and macros, never variables). The macro keeps the POSIX spelling that
 * user code expects while the object stays private to provider/posix:
 *
 *     fputs("x\n", stdout);   =>   fputs("x\n", br_posix_stdout());
 *
 * No process model: nothing opens fd 0/1/2, so all three are BR_NULL unless a caller installs a
 * stream with br_posix_set_std*(FILE *).
 */
FILE *br_posix_stdin(void);
FILE *br_posix_stdout(void);
FILE *br_posix_stderr(void);
void  br_posix_set_stdin(FILE *f);
void  br_posix_set_stdout(FILE *f);
void  br_posix_set_stderr(FILE *f);

#define stdin  br_posix_stdin()
#define stdout br_posix_stdout()
#define stderr br_posix_stderr()

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

/* Declarations only (implementation in posix.c / posix_dir.c). */
int remove(const char *path);
int rename(const char *old_path, const char *new_path);

/* Formatting layer absent: printf/fprintf/snprintf/vprintf/scanf are not declared, so
 * unimplemented items surface at link time. */

#endif /* BR_POSIX_STDIO_H */
