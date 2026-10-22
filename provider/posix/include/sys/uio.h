/* Scatter/gather I/O. */
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
