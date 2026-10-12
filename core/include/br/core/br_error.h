/*
 * brickOS prototype v0.2.0 — core error codes (errno; value-for-value aligned with the
 * Linux kernel)
 *
 * Design basis (3-01 §11 "error codes"): core int returns always use a negative errno.
 * The usable subset of each domain is documented separately (SD-10: core domain in
 * 3-01 §11; device/storage domain in 8-01 §4); this header gives the full set of
 * values, not "every domain can return all of these".
 *
 * ## Source of truth (read this before editing the file)
 *   Values are taken literally from the Linux kernel
 *   (`include/uapi/asm-generic/errno-base.h` and `include/uapi/asm-generic/errno.h`)
 *   —— both are the same on aarch64 and x86-64, so the host `<errno.h>` is an
 *   executable cross-check for this table: the host gate `ft-test`
 *   (`tests/host/ft_test.c`) asserts code by code that "this table's value == the
 *   host's value", so any copy error shows up red there.
 *   (★ That case must capture the host values FIRST, then `#undef` the host macros of
 *     the same names, and only then include this header —— because this table uses the
 *     same names as POSIX (see below); reversed order would be "comparing it with
 *     itself".)
 *   ⇒ New values may only be copied from the kernel table, never invented; after
 *     copying, add the cross-check assertion to that gate.
 *
 * ## Why ship a copy instead of <errno.h>
 *   Same reasoning as br_types.h —— this prototype is `-ffreestanding -nostdlib` and
 *   does not drag the host libc errno table into the target image.
 *
 * ## Inclusion policy (two sections, matching the kernel header split)
 *   - Section one = the full `errno-base.h` set (1–34): included whole. The reason is
 *     not "we use them" but "domain subsets are cut by semantics, the value table is
 *     copied from the kernel" —— copying only what is used would turn "next time we
 *     need a code" into a copy plus a reconciliation; including all is done once and
 *     for all, and half a table cannot be mis-copied.
 *   - Section two = the generic part of `errno.h`, only what this repo uses (from 35):
 *     that part has 100+ values, most of them network/kernel-module domain (the v2
 *     socket surface will add them as needed).
 *
 * ## Known differences from the design docs (still standing)
 *   8-01 §4's SD-10 device/storage domain subset is `-EIO/-ENODEV/-ENOSPC/-EINVAL/`
 *   `-ENOTSUP/-EBUSY/-EROFS`; but D23's lookup chain and "namespace change" also need
 *   `-ENOENT` (a miss is not a bad argument), `-ENOTDIR`/`-EISDIR`, `-ENOTEMPTY`
 *   (forcing -EBUSY would collide with "mount point not removable").
 *   These are not in SD-10's enumeration, but squashing them into -EINVAL would distort
 *   both diagnosis and errno mapping, so they are added with kernel numbers
 *   (`docs/decisions/0009-…` §3 ruling 4).
 *   `runtime/posix`'s `errno = -ret` is a zero-conversion negation (11-01 §2, ADR-0014)
 *   ⇒ this table's numbering IS what user space sees —— the final reason for
 *   "aligning with the kernel".
 *
 * ## The names are the POSIX originals, co-located with the values (ADR-0027)
 *   This header gives BOTH the name and the value, and the name is the POSIX original
 *   —— `#define EIO 5`, with NO `BR_` prefix. Why no prefix (the crux of this change):
 *     - third-party code is, per ADR-0026, compiled from upstream as-is, and upstream is
 *       full of bare `-EIO`/`-EFSCORRUPTED` ⇒ only same-name same-value lets that code
 *       compile without a single edit; with a prefix the only way out would be stuffing
 *       a `#define EIO BR_EIO` mapping table into upstream files —— which is both a
 *       second source of truth and a modification of upstream code;
 *     - names and values in one place ⇒ no more "name table + value table" pair (they
 *       were separate at ADR-0023 time).
 *   `BR_ERR(e)` (`-(int)(e)`) is kept: it is the negation macro, not an errno name;
 *   likewise `BR_OK`. `br/core/br_errno.h` has degenerated into a forwarding header
 *   (it only includes this one), just to avoid changing existing include sites.
 *   ★ The `BR_` prefix is removed from errno names, but this repo's own identifiers
 *     still use it (`BR_EC_*` exception classes, `BR_EMMC_*`, `BR_EROFS_*`, `BR_ERR`,
 *     etc.) —— "the numbers are theirs, so the names should be theirs too; `BR_` is
 *     left for things this repo invents".
 */
#ifndef BR_CORE_BR_ERROR_H
#define BR_CORE_BR_ERROR_H

/* success */
#define BR_OK   0

/* ==================================================================== section one
 * full errno-base.h set (1–34; copied verbatim from the kernel). */
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
#define EFAULT      14   /* Bad address (the agreed error code for extable fixup, 3-02 §10.4) */
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

/* ==================================================================== section two
 * generic part of errno.h (only what this repo uses; copied verbatim from the kernel). */
#define EDEADLK       35   /* Resource deadlock would occur */
#define ENAMETOOLONG  36   /* File name too long */
#define ENOLCK        37   /* No record locks available */
#define ENOSYS        38   /* Invalid system call number */
#define ENOTEMPTY     39   /* Directory not empty */
#define ELOOP         40   /* Too many symbolic links encountered */
#define EOVERFLOW     75   /* Value too large for defined data type */
#define ENOTSUP       95   /* Operation not supported (= EOPNOTSUPP) */
#define ETIMEDOUT    110   /* Connection timed out (unified blocking timeout, INV-1/CA-4) */
#define EUCLEAN      117   /* Structure needs cleaning —— used for EROFS's EFSCORRUPTED:
                               * ADR-0023 D-4 "add missing POSIX codes from the kernel table",
                               * rather than mapping onto some other code */
#define EDQUOT       122   /* Quota exceeded */
#define ECANCELED    125   /* Operation Canceled */
#define EOWNERDEAD   130   /* Owner died */
#define ENOTRECOVERABLE 131 /* State not recoverable */

/* ---- same-value aliases (the kernel has two names; keep both so consumers need not change) ---- */
#define EWOULDBLOCK  EAGAIN      /* 11 */
#define EOPNOTSUPP   ENOTSUP     /* 95 */

/* negative form: design 3-01 §11 —— core int returns use negative errno */
#define BR_ERR(e)   (-(int)(e))

#endif /* BR_CORE_BR_ERROR_H */
