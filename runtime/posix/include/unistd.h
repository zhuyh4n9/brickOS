/*
 * brickOS prototype v0.2.0 — 系统调用面(unistd.h; runtime/posix 插件提供)
 *
 * 覆盖范围 = `11-02-svc-posix-subset.md` 里 **TR-A + TR-B** 的 fd/文件/目录/进程族。
 * 明确**不在**本头里的(子集诚实义务, 见该文的 TR-C/TR-D):
 *   `fork`/`exec*`/`wait*`(单地址空间, `1-01` §2.1)、信号族(无信号模型)、
 *   `gettimeofday/time`(无墙钟源, P-1)、`exit/_exit`(无进程退出语义, Q-5)、
 *   `mmap/mprotect`(vfs 恒 -ENOTSUP, P-7)、`chroot/pipe/mknod`(无对应机制)。
 */
#ifndef BR_POSIX_UNISTD_H
#define BR_POSIX_UNISTD_H

#include <sys/types.h>
#include <sys/uio.h>

/* ---- 文件 I/O ---- */
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

/* ---- 名字空间 ---- */
int unlink(const char *path);
int rmdir (const char *path);
int link  (const char *old_path, const char *new_path);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t cap);
int rename(const char *old_path, const char *new_path);

/* ---- 工作目录(相对路径的落点; cwd 是 POSIX 概念, 归本服务, vfs 只认绝对路径)---- */
int   chdir (const char *path);
char *getcwd(char *buf, size_t size);

/* ---- 权限(本代 = 存在性检查 + 一律放行; 见 ADR-0014)---- */
#define F_OK  0
#define X_OK  1
#define W_OK  2
#define R_OK  4
int access(const char *path, int mode);

/* ---- 时间(见 time.h 关于"没有墙钟"的说明)---- */
unsigned int sleep(unsigned int seconds);
int          usleep(useconds_t usec);

/* ---- 进程身份(单 APP: 常量)---- */
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);

#endif /* BR_POSIX_UNISTD_H */
