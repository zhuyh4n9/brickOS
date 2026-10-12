/*
 * brickOS prototype v0.1.0 - internal prototypes for compiler support routines (core-private)
 *
 * These four symbols are **not native API** (not part of the 48-function surface in `3-01`,
 * and not visible to plugins): GCC emits calls to them itself, so they are part of the
 * foundation of whether linking succeeds. They live in an internal header under core/src to
 * satisfy `-Wmissing-prototypes` (a prototype must precede the definition) while **not**
 * mixing them into the contract headers in `core/include/br/core/` - any name in a contract
 * header counts as public surface.
 *
 * Symbol unification is **not achievable** (ADR-0014 §4): core needs `memset` in the EARLY
 * phase while the POSIX runtime is a LATE-phase service, so those four symbols must stay in
 * core. This file is therefore permanent, not temporary.
 */
#ifndef BR_CORE_STRING_INTERNAL_H
#define BR_CORE_STRING_INTERNAL_H

#include <br/core/br_types.h>

void *memcpy (void *dst, const void *src, br_size_t n);
void *memmove(void *dst, const void *src, br_size_t n);
void *memset (void *dst, int c, br_size_t n);
int   memcmp (const void *a, const void *b, br_size_t n);

#endif /* BR_CORE_STRING_INTERNAL_H */
