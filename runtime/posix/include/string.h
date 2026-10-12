/*
 * brickOS prototype v0.2.0 — strings (string.h; provided by the runtime/posix plugin)
 *
 * ★ `memcpy/memmove/memset/memcmp` are **not defined by this plugin** — they are symbols
 *   provided by core's `core/src/string.c` (a hard duty of compiler lowering: even with
 *   -ffreestanding, GCC lowers a block move into a call to memcpy; see that file's
 *   comments). This header only **declares** them so sources can `#include <string.h>`
 *   with a single copy of the symbol.
 *   ⇒ The design statement "delete core/src/string.c once the POSIX runtime lands, to
 *     unify the symbol" is **not achievable** this generation: core needs memcpy already
 *     in the EARLY phase, while runtime/posix is a LATE-phase service. This is logged as
 *     debt in ADR-0014 (not an omission, a phase fact).
 *
 * ★ The `str*` family **is defined here** (the "write it yourself" path of TR-C): small in
 *   size, and without it neither the FILE layer nor any string handling can be written.
 *   **Not in this generation**: "advanced" functions such as `strtok/strstr/strspn` and
 *   everything locale-related.
 */
#ifndef BR_POSIX_STRING_H
#define BR_POSIX_STRING_H

#include <sys/types.h>

/* ---- provided by core (declared only here) ---- */
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset (void *dst, int c, size_t n);
int   memcmp (const void *a, const void *b, size_t n);

/* ---- provided here ---- */
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
