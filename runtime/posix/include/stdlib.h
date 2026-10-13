/*
 * malloc/calloc/realloc/free alias core's br_malloc family; abs/labs/atoi/atol are hand-written.
 * The strtol family (endptr/base/overflow) and qsort/bsearch are out of this generation.
 * No exit()/atexit(): single address space, so "process exit" is undefined. abort() maps to
 * br_panic and does not return.
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

/* Panic; does not return. */
void  abort(void) __attribute__((noreturn));

/* No environment-variable model: always returns NULL. */
char *getenv(const char *name);

#endif /* BR_POSIX_STDLIB_H */
