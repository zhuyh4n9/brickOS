/*
 * brickOS prototype v0.2.0 — POSIX base types (provided by the runtime/posix plugin)
 *
 * ============================== what this is ==============================
 * `runtime/posix` is the **POSIX runtime service** from design D18 (sole owner of the fd table + POSIX symbol
 * surface + libc stubs); this directory is the **POSIX header surface** it exposes to consumers.
 *
 * * Relation to `br_*`: every type here is **an alias or thin wrapper of `br_*`** -- there is no second set of
 *   semantics. For example `typedef long ssize_t` has the same width as core's `br_s64`, so the return value of
 *   `read()` is directly the return value of `br_file_read()`; `errno = -ret` is a **zero-cost conversion**
 *   (design `11-01` §2). The only reason this layer exists is "the standard names", not another abstraction layer.
 *
 * * Subset honesty obligation (cost 2 of `1-01` §7.6): **this directory declares only what is implemented**.
 *   No `gettimeofday`, because the prototype has no wall-clock source (see P-1 in
 *   `docs/11-service/11-02-svc-posix-subset.md`); no `fork`/`exec`/signals, because the design premise is a
 *   single address space (`1-01` §2.1). Declaring something that cannot link is worse than not declaring it.
 */
#ifndef BR_POSIX_SYS_TYPES_H
#define BR_POSIX_SYS_TYPES_H

/* size_t / ptrdiff_t come from compiler builtins: this prototype is -ffreestanding and has no <stddef.h>
 * dependency (GCC and Clang both provide these macros), so "is our size_t the same as the compiler's size_t"
 * never becomes a question. */
#ifndef _BR_POSIX_SIZE_T_DEFINED
#define _BR_POSIX_SIZE_T_DEFINED
typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#endif

#ifndef _BR_POSIX_SSIZE_T_DEFINED
#define _BR_POSIX_SSIZE_T_DEFINED
typedef long ssize_t;      /* same width as core's br_s64 (64-bit) */
typedef long off_t;        /* same width as br_s64 */
#endif

typedef unsigned int  mode_t;
typedef int           pid_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned long dev_t;      /* POSIX/LP64 = 8 B (was unsigned int = 4 B, inconsistent with the host) */
typedef unsigned long ino_t;
typedef unsigned long nlink_t;
typedef long          blksize_t;  /* POSIX/LP64 = 8 B (was unsigned int = 4 B, inconsistent with the host) */
typedef long          blkcnt_t;
typedef long          time_t;          /* seconds (see the "no wall clock" note in time.h) */
typedef long          suseconds_t;
typedef unsigned int  useconds_t;
typedef unsigned long id_t;

/* `SEEK_*`: POSIX declares these in <stdio.h> and <unistd.h> respectively; placing them in the **base type
 * header** is deliberate -- this prototype is freestanding, so one less implicit "some header happens to include
 * another" dependency means one less class of "fails to compile under a different include order". Values follow
 * Linux (0/1/2). */
#ifndef SEEK_SET
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#endif

#ifndef NULL
#define NULL  ((void*)0)
#endif

#endif /* BR_POSIX_SYS_TYPES_H */
