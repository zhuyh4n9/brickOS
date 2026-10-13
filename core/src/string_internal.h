/*
 * Core-private prototypes for the compiler support routines: not public surface (GCC emits the
 * calls itself). Kept in an internal header to satisfy -Wmissing-prototypes without adding names
 * to core/include/br/core/.
 * They must stay in core: it needs memset in the EARLY phase, while the POSIX runtime that owns
 * the POSIX symbol surface is a LATE-phase service, so the sets cannot be unified.
 */
#ifndef BR_CORE_STRING_INTERNAL_H
#define BR_CORE_STRING_INTERNAL_H

#include <br/core/br_types.h>

void *memcpy (void *dst, const void *src, br_size_t n);
void *memmove(void *dst, const void *src, br_size_t n);
void *memset (void *dst, int c, br_size_t n);
int   memcmp (const void *a, const void *b, br_size_t n);

#endif /* BR_CORE_STRING_INTERNAL_H */
