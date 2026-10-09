/*
 * brickOS prototype v0.2.0 — 文件控制(fcntl.h; runtime/posix 插件提供)
 *
 * ★ `O_*` **取 Linux 的数值**(八进制沿用内核口径), 理由有二:
 *   ① 与 errno 同一条纪律 —— 三方代码里常有按 Linux 数值写的条件, 对齐能白捡兼容;
 *   ② 它们是"实现定义"的(POSIX 不管数值), 所以抄一份权威表比自己编号更不容易出错。
 *   注意: 本仓的 native 面用 `BR_O_*`(另一套数值, 见 br_vfs.h), 两者靠 runtime/posix 的
 *   `posix_to_br_flags()` **显式翻译** —— 一侧是标准, 一侧是内核, 中间必然有一层。
 */
#ifndef BR_POSIX_FCNTL_H
#define BR_POSIX_FCNTL_H

#include <sys/types.h>

/* ---- 打开标志(Linux 数值)---- */
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
#define O_LARGEFILE  0100000
#define O_DIRECTORY  0200000
#define O_NOFOLLOW   0400000
#define O_NOATIME    01000000
#define O_CLOEXEC    02000000

/* ---- 文件描述符标志 ---- */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define F_GETLK   5
#define F_SETLK   6
#define F_SETLKW  7

#define FD_CLOEXEC  1

/* ---- 记录锁 ---- */
#define F_RDLCK   0
#define F_WRLCK   1
#define F_UNLCK   2

struct flock {
    short l_type;      /* F_RDLCK / F_WRLCK / F_UNLCK */
    short l_whence;    /* SEEK_SET / SEEK_CUR / SEEK_END */
    off_t l_start;
    off_t l_len;       /* 0 = 到文件末尾 */
    pid_t l_pid;       /* F_GETLK 的回答 */
};

#define AT_FDCWD  (-100)

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int creat(const char *path, mode_t mode);
int fcntl(int fd, int cmd, ...);

#endif /* BR_POSIX_FCNTL_H */
