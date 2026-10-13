/*
 * Core error codes. Values are copied verbatim from the Linux kernel errno-base.h / errno.h
 * and must never be invented; the host gate ft-test cross-checks each one. Freestanding build
 * (`-ffreestanding -nostdlib`), so no host <errno.h>. Names are the POSIX originals with no
 * `BR_` prefix, co-located with their values; BR_ERR(e) negates. Core int returns use negative
 * errno. Section one is the full 1-34 set, section two only what this repo uses.
 */
#ifndef BR_CORE_BR_ERROR_H
#define BR_CORE_BR_ERROR_H

/* success */
#define BR_OK   0

/* ---- section one: full errno-base.h set (1-34, copied verbatim from the kernel) ---- */
#define EPERM        1   /* Operation not permitted */
#define ENOENT       2   /* No such file or directory */
#define ESRCH        3   /* No such process */
#define EINTR        4   /* Interrupted system call */
#define EIO          5   /* I/O error */
#define ENXIO        6   /* No such device or address */
#define E2BIG        7   /* Argument list too long */
#define ENOEXEC      8   /* Exec format error */
#define EBADF        9   /* Bad file number */
#define ECHILD      10   /* No child processes */
#define EAGAIN      11   /* Try again */
#define ENOMEM      12   /* Out of memory */
#define EACCES      13   /* Permission denied */
#define EFAULT      14   /* Bad address (error code for extable fixup) */
#define ENOTBLK     15   /* Block device required */
#define EBUSY       16   /* Device or resource busy */
#define EEXIST      17   /* File exists */
#define EXDEV       18   /* Cross-device link */
#define ENODEV      19   /* No such device */
#define ENOTDIR     20   /* Not a directory */
#define EISDIR      21   /* Is a directory */
#define EINVAL      22   /* Invalid argument */
#define ENFILE      23   /* File table overflow */
#define EMFILE      24   /* Too many open files */
#define ENOTTY      25   /* Not a typewriter */
#define ETXTBSY     26   /* Text file busy */
#define EFBIG       27   /* File too large */
#define ENOSPC      28   /* No space left on device */
#define ESPIPE      29   /* Illegal seek */
#define EROFS       30   /* Read-only file system */
#define EMLINK      31   /* Too many links */
#define EPIPE       32   /* Broken pipe */
#define EDOM        33   /* Math argument out of domain of func */
#define ERANGE      34   /* Math result not representable */

/* ---- section two: generic errno.h entries this repo uses (copied verbatim) ---- */
#define EDEADLK       35   /* Resource deadlock would occur */
#define ENAMETOOLONG  36   /* File name too long */
#define ENOLCK        37   /* No record locks available */
#define ENOSYS        38   /* Invalid system call number */
#define ENOTEMPTY     39   /* Directory not empty */
#define ELOOP         40   /* Too many symbolic links encountered */
#define EOVERFLOW     75   /* Value too large for defined data type */
#define ENOTSUP       95   /* Operation not supported (= EOPNOTSUPP) */
#define ETIMEDOUT    110   /* Connection timed out (unified blocking timeout) */
#define EUCLEAN      117   /* Structure needs cleaning (EFSCORRUPTED for EROFS) */
#define EDQUOT       122   /* Quota exceeded */
#define ECANCELED    125   /* Operation Canceled */
#define EOWNERDEAD   130   /* Owner died */
#define ENOTRECOVERABLE 131 /* State not recoverable */

/* ---- same-value aliases (the kernel has two names; keep both so consumers need not change) ---- */
#define EWOULDBLOCK  EAGAIN      /* 11 */
#define EOPNOTSUPP   ENOTSUP     /* 95 */

/* negative form: core int returns use negative errno */
#define BR_ERR(e)   (-(int)(e))

#endif /* BR_CORE_BR_ERROR_H */
