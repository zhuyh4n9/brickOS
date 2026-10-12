/*
 * brickOS prototype v0.2.0 — syscall surface (unistd.h; provided by the runtime/posix plugin)
 *
 * Coverage = the **TR-A + TR-B** fd/file/directory/process families from
 * `11-02-svc-posix-subset.md`. Explicitly **not** here (subset honesty duty, see TR-C/TR-D there):
 *   `fork`/`exec*`/`wait*` (single address space, `1-01` §2.1), signal family (no signal model),
 *   `gettimeofday/time` (no wall-clock source, P-1), `exit/_exit` (no process-exit semantics, Q-5),
 *   `mmap/mprotect` (vfs is always -ENOTSUP, P-7), `chroot/pipe/mknod` (no counterpart mechanism).
 */
#ifndef BR_POSIX_UNISTD_H
#define BR_POSIX_UNISTD_H

#include <sys/types.h>
#include <sys/uio.h>

/* ---- file I/O ---- */
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

/* ---- name space ---- */
int unlink(const char *path);
int rmdir (const char *path);
int link  (const char *old_path, const char *new_path);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t cap);
int rename(const char *old_path, const char *new_path);

/* ---- working directory (target of relative paths; cwd is a POSIX concept owned by this service,
 * vfs knows only absolute paths)---- */
int   chdir (const char *path);
char *getcwd(char *buf, size_t size);

/* ---- permissions (this generation = existence check + always allow; see ADR-0014)---- */
/* POSIX-required standard descriptor numbers. Here fd 0/1/2 are bound to the console by
 * runtime/posix initialization (see that plugin's start hook). */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4
int access(const char *path, int mode);

/* ---- time (see time.h on "no wall clock")---- */
unsigned int sleep(unsigned int seconds);
int          usleep(useconds_t usec);

/* ---- process identity (single APP: constants)---- */
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

#endif /* BR_POSIX_UNISTD_H */
