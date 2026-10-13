/*
 * POSIX errno surface. Names and values come from core (br/core/br_errno.h re-export); only
 * the per-thread errno macro and BR_POSIX_STRERROR_MAX are unique to this plugin.
 * errno is the slot address returned by br_posix_errno(); never declare `extern int errno`.
 * POSIX: a failing function returns -1 and sets errno; success need not clear it.
 */
#ifndef BR_POSIX_ERRNO_H
#define BR_POSIX_ERRNO_H

#include <br/core/br_errno.h>

/* Per-thread errno slot address; unregistered threads lazily get a record. */
int *br_posix_errno(void);

/* Modifiable lvalue per POSIX; macro expands to a dereference. */
#define errno   (*br_posix_errno())

/* Success code (not an E*); keeps the exported symbol name. */
#define BR_POSIX_OK   0

/* Max strerror buffer incl. trailing NUL; larger caller buffers are never truncated. */
#define BR_POSIX_STRERROR_MAX  40

#endif /* BR_POSIX_ERRNO_H */
