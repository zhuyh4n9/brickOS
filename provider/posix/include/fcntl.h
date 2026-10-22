/*
 * O_* use Linux's octal values (implementation-defined in POSIX, so copying an authoritative
 * table is safer). The native side uses BR_O_*; posix_to_br_flags() translates explicitly.
 */
#ifndef BR_POSIX_FCNTL_H
#define BR_POSIX_FCNTL_H

#include <sys/types.h>

/* ---- open flags (Linux values) ---- */
#define O_RDONLY     00000000
#define O_WRONLY     00000001
#define O_RDWR       00000002
#define O_ACCMODE    00000003
#define O_CREAT      00000100
#define O_EXCL       00000200
#define O_NOCTTY     00000400
#define O_TRUNC      0001000
#define O_APPEND     0002000
#define O_NONBLOCK   0004000
#define O_DSYNC      0010000
#define O_DIRECT     0040000
/* 0 on 64-bit hosts (off_t already 64-bit); 0100000 is the 32-bit value and disagrees with it. */
#define O_LARGEFILE  0
#define O_DIRECTORY  0200000
#define O_NOFOLLOW   0400000
#define O_NOATIME    01000000
#define O_CLOEXEC    02000000

/* ---- file descriptor flags ---- */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define F_GETLK   5
#define F_SETLK   6
#define F_SETLKW  7

#define FD_CLOEXEC  1

/* ---- record locks ---- */
#define F_RDLCK   0
#define F_WRLCK   1
#define F_UNLCK   2

struct flock {
    short l_type;      /* F_RDLCK / F_WRLCK / F_UNLCK */
    short l_whence;    /* SEEK_SET / SEEK_CUR / SEEK_END */
    off_t l_start;
    off_t l_len;       /* 0 = to end of file */
    pid_t l_pid;       /* answer of F_GETLK */
};

#define AT_FDCWD  (-100)

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif /* BR_POSIX_FCNTL_H */
