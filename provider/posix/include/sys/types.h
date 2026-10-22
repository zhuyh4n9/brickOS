/*
 * POSIX base types for provider/posix. Every type is an alias or thin wrapper of br_*; ssize_t
 * matches br_s64, so read() returns br_file_read() directly and errno = -ret is zero-cost.
 * Only what is implemented is declared: no gettimeofday, fork/exec or signals.
 */
#ifndef BR_POSIX_SYS_TYPES_H
#define BR_POSIX_SYS_TYPES_H

/* Compiler builtins: -ffreestanding, no <stddef.h> dependency. */
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

/* In the base type header so there is no include-order dependency; Linux values. */
#ifndef SEEK_SET
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#endif

#ifndef NULL
#define NULL  ((void*)0)
#endif

#endif /* BR_POSIX_SYS_TYPES_H */
