/*
 * struct stat: vfs's br_stat_t is only {valid, type, size, nlink}, so st_mode is synthesized
 * from type (S_IFDIR/S_IFREG/S_IFLNK | 0777); st_nlink/st_size are real; st_ino/st_dev/st_uid/
 * st_gid/st_*time are always 0 (this surface does not exist). Fields are kept for layout
 * stability when vfs adds them later.
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

/* ---- file type bits (Linux values) ---- */
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

/* ---- permission bits (expressible; not enforced) ---- */
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

/* Fixed mode reported for every file (no permission model). */
#define BR_POSIX_MODE_BITS  0777

int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);      /* no-op ⇒ 0 (user decision; see ADR-0014) */
int fchmod(int fd, mode_t mode);               /* no-op ⇒ 0 */
int chown(const char *path, uid_t uid, gid_t gid);  /* no-op ⇒ 0 */
int fchown(int fd, uid_t uid, gid_t gid);           /* no-op ⇒ 0 */
mode_t umask(mode_t mask);                     /* record and return old value (not enforced) */

#endif /* BR_POSIX_SYS_STAT_H */
