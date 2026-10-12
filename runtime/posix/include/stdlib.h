/*
 * brickOS prototype v0.2.0 — general utilities (stdlib.h; provided by the runtime/posix
 * plugin)
 *
 * Coverage (matching `11-02` §2.5):
 *   - `malloc/calloc/realloc/free` = **TR-B**: they land directly on core's `br_malloc`
 *     family (the comment in `br_mem.h` already says "needed by the runtime/posix libc
 *     stub"). Aliases, zero overhead.
 *   - `abs/labs/atoi/atol` = small **hand-written** pieces of this generation (about ten
 *     lines each). Strictly, the `strtol` family is TR-C in `11-02` ("either bring in
 *     libc or write it yourself"); these three are the most worthwhile entry points on
 *     the hand-written route — without them, any "read one line of numbers" code would
 *     have to write its own parser. The full `strtol` (with endptr/base/overflow) and
 *     `qsort`/`bsearch` remain **out of this generation**.
 *
 * ★ **No `exit()`/`atexit()`**: in a single address space "process exit" is undefined
 *   (the gap in design `1-01` §2.1, Q-5 of `11-02`). `abort()` exists and lands on core's
 *   `br_panic` — it is not a "normal exit path" but "an unrecoverable error happened
 *   here", with clear semantics.
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

/* Terminate in panic form (crash presentation of design `5-01`; does not return). */
void  abort(void) __attribute__((noreturn));

/* No environment-variable model this generation ⇒ always returns NULL (answer "none"
 * honestly instead of faking an empty environment). */
char *getenv(const char *name);

#endif /* BR_POSIX_STDLIB_H */
