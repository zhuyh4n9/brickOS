/*
 * fd/file/directory/process surfaces. Not provided: fork/exec/wait (single address space),
 * signals, wall clock (gettimeofday/time), exit/_exit, mmap/mprotect (always -ENOTSUP),
 * chroot/pipe/mknod.
 */
#ifndef BR_POSIX_UNISTD_H
#define BR_POSIX_UNISTD_H

#include <sys/types.h>
#include <sys/uio.h>

ssize_t read (int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
ssize_t pread (int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     dup (int oldfd);
int     dup2(int oldfd, int newfd);
int     fsync(int fd);
int     fdatasync(int fd);
int     ftruncate(int fd, off_t len);
int     truncate(const char *path, off_t len);
int     isatty(int fd);

int unlink(const char *path);
int rmdir (const char *path);
int link  (const char *old_path, const char *new_path);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t cap);
int rename(const char *old_path, const char *new_path);

/* cwd is a POSIX concept owned by this service; vfs knows only absolute paths. */
int   chdir (const char *path);
char *getcwd(char *buf, size_t size);

/* fd 0/1/2 are bound to the console by runtime/posix initialization. */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4
/* Existence check only; always allows this generation. */
int access(const char *path, int mode);

/* No wall clock; see time.h. */
unsigned int sleep(unsigned int seconds);
int          usleep(useconds_t usec);

/* Single APP: constants. */
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

#endif /* BR_POSIX_UNISTD_H */
