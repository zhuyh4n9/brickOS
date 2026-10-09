/*
 * brickOS prototype v0.2.0 — 文件状态(sys/stat.h; runtime/posix 插件提供)
 *
 * ★ `struct stat` 的字段: 本原型**没有** uid/gid/时间戳/设备号的存储面(vfs 的
 *   `br_stat_t` 只有 `{valid, type, size, nlink}`, 见 br_vfs.h)。于是:
 *     - `st_mode` 由 `br_stat_t.type` **合成**(S_IFDIR/S_IFREG/S_IFLNK | 0777);
 *     - `st_nlink` / `st_size` 是真的(来自 br_stat_t);
 *     - `st_ino` / `st_dev` / `st_uid` / `st_gid` / `st_atime`… **恒为 0**(不是"读失败",
 *       而是如实回答"这个面不存在")。这一点写在 ADR-0014 的欠账里 —— 依赖 inode 号做
 *       去重的代码在本代拿不到它。
 *   `struct stat` 保留这些字段是为了**布局稳定**: 将来 vfs 补上它们时, 消费者不必改结构。
 */
#ifndef BR_POSIX_SYS_STAT_H
#define BR_POSIX_SYS_STAT_H

#include <sys/types.h>

struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
    time_t    st_atime;
    time_t    st_mtime;
    time_t    st_ctime;
};

/* ---- 文件类型位(Linux 数值)---- */
#define S_IFMT    0170000
#define S_IFSOCK  0140000
#define S_IFLNK   0120000
#define S_IFREG   0100000
#define S_IFBLK   0060000
#define S_IFDIR   0040000
#define S_IFCHR   0020000
#define S_IFIFO   0010000

#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* ---- 权限位(不执法, 但要能表达; 见 ADR-0014 的"权限模型"欠账)---- */
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000
#define S_IRWXU  0000700
#define S_IRUSR  0000400
#define S_IWUSR  0000200
#define S_IXUSR  0000100
#define S_IRWXG  0000070
#define S_IRGRP  0000040
#define S_IWGRP  0000020
#define S_IXGRP  0000010
#define S_IRWXO  0000007
#define S_IROTH  0000004
#define S_IWOTH  0000002
#define S_IXOTH  0000001

/* 本代报出的固定权限(没有权限模型 ⇒ 如实报"全开", 而不是编一个 0644 出来骗人) */
#define BR_POSIX_MODE_BITS  0777

int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);      /* 空操作 ⇒ 0(用户裁定; 见 ADR-0014) */
int fchmod(int fd, mode_t mode);               /* 空操作 ⇒ 0 */
int chown(const char *path, uid_t uid, gid_t gid);  /* 空操作 ⇒ 0 */
int fchown(int fd, uid_t uid, gid_t gid);           /* 空操作 ⇒ 0 */
mode_t umask(mode_t mask);                     /* 记录并返回旧值(不执法) */

#endif /* BR_POSIX_SYS_STAT_H */
