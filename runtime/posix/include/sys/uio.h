/*
 * brickOS prototype v0.2.0 — 分散/聚集 I/O(sys/uio.h; runtime/posix 插件提供)
 */
#ifndef BR_POSIX_SYS_UIO_H
#define BR_POSIX_SYS_UIO_H

#include <sys/types.h>

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#endif /* BR_POSIX_SYS_UIO_H */
