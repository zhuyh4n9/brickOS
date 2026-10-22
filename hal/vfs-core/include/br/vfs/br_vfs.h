/*
 * VFS public contract (hal/vfs-core plugin). br_open resolves through the mount table only;
 * pure VFS -- no device knowledge, devices attach via fs/devfs as /dev nodes. FS plugins fill
 * br_fs_ops/br_inode_ops/br_file_ops and free their transient inodes via free_inode. fpriv travels
 * with the inode, is handed to br_file_t at open, may be overwritten by fops->open() and is freed
 * by fops->close() (Linux file->private_data shape).
 */
#ifndef BR_VFS_BR_VFS_H
#define BR_VFS_BR_VFS_H

#include <br/core/br_types.h>

/* br_open flags: append-only, values deliberately not shared with POSIX O_*.
 * ACCMODE = access mode mask; CREAT create-if-absent (files only, dirs use br_mkdir);
 * EXCL with CREAT => -EEXIST; TRUNC truncate to 0 (needs write permission); APPEND seeks to end
 * before each write; NONBLOCK no data => -EAGAIN; NOFOLLOW final symlink => -ELOOP (middle
 * components still expanded). */
#define BR_O_RDONLY    0x0001u
#define BR_O_WRONLY    0x0002u
#define BR_O_RDWR      0x0003u
#define BR_O_ACCMODE   0x0003u
#define BR_O_CREAT     0x0010u
#define BR_O_EXCL      0x0020u
#define BR_O_TRUNC     0x0040u
#define BR_O_APPEND    0x0080u
#define BR_O_NONBLOCK  0x0100u
#define BR_O_NOFOLLOW  0x0200u

#define BR_O_WRITABLE(f)  (((f) & BR_O_ACCMODE) != BR_O_RDONLY)

/* ---- lseek ---- */
#define BR_SEEK_SET    0
#define BR_SEEK_CUR    1
#define BR_SEEK_END    2

/* ---- inode type (SYMLINK appended, old values unchanged) ---- */
#define BR_INODE_NONE    0u
#define BR_INODE_DIR     1u
#define BR_INODE_FILE    2u
#define BR_INODE_SYMLINK 3u

/* ---- poll event bits ---- */
#define BR_POLLIN      0x01u
#define BR_POLLOUT     0x02u
#define BR_POLLERR     0x04u

/* Directory entry / device name bound. devfs node names are [a-z][a-z0-9]*; for tmpfs file names
 * longer => -ENOSPC (the -ENAMETOOLONG substitute). */
#define BR_NAME_MAX    32u

/* Symlink target string bound (single target, not the resolved total); longer => -ENAMETOOLONG.
 * 96 clears BR_NAME_MAX with room for a path yet stays below the path buffer, so a relative
 * target always joins. */
#define BR_SYMLINK_MAX 96u

/* ---- forward declarations ---- */
typedef struct br_file       br_file_t;       /* opaque open handle */
typedef struct br_inode      br_inode_t;      /* transient walk product: public head + FS private tail */
typedef struct br_dir        br_dir_t;        /* directory iteration handle */
typedef struct br_file_ops   br_file_ops_t;
typedef struct br_inode_ops  br_inode_ops_t;
typedef struct br_dentry_ops br_dentry_ops_t; /* all NULL in v1 */
typedef struct br_fs_ops     br_fs_ops_t;

/* ---- stat / dirent ---- */

/* Attributes: v1 implements size only (truncation); other fields and valid bits are reserved
 * append-only, so a setattr can state which fields it wants to change. For a symlink, size =
 * target string length. */
#define BR_STAT_SIZE   0x0001u
#define BR_STAT_NLINK  0x0002u

typedef struct br_stat {
    br_u32 valid;   /* BR_STAT_* bits: fields valid in this call/answer */
    br_u32 type;    /* BR_INODE_DIR / BR_INODE_FILE / BR_INODE_SYMLINK */
    br_u64 size;    /* bytes (directory = 0; symlink = target string length) */
    br_u32 nlink;   /* hard link count (directory = 1; FS without links may leave 0 and not set the valid bit) */
} br_stat_t;

/* Directory entry: fixed-size record; read() fills one and returns sizeof(br_dirent_t), or 0 when
 * exhausted. */
typedef struct br_dirent {
    br_u32 type;                /* BR_INODE_DIR / BR_INODE_FILE */
    br_u32 name_len;            /* excluding the trailing '\0' */
    char   name[BR_NAME_MAX];
} br_dirent_t;

/* ---- layer 3: file ---- */

/* File-face ops. Unsupported slot = BR_NULL => -ENOTSUP. open() establishes the session; close()
 * pairs with it and frees what open() stored via br_file_set_fpriv(). A directory has a
 * br_file_ops too (read produces br_dirent_t), so br_opendir/br_readdir are fully generic. */
struct br_file_ops {
    int     (*open) (br_file_t *f, br_u32 flags);
    br_s64  (*read) (br_file_t *f, void *buf, br_size_t n);
    br_s64  (*write)(br_file_t *f, const void *buf, br_size_t n);
    br_s64  (*lseek)(br_file_t *f, br_s64 off, int whence);
    int     (*ioctl)(br_file_t *f, br_u32 cmd, void *arg);
    int     (*fsync)(br_file_t *f);
    int     (*poll) (br_file_t *f, br_u32 *events);
    int     (*poll_attach)(br_file_t *f, void *wait);   /* v2 reserved (wait-queue); always BR_NULL in v1 */
    int     (*close)(br_file_t *f);
};

/* Accessors for the opaque open handle, for fops implementers: inode / private data / offset /
 * flags. Plugins must not touch br_file_t fields. */
br_inode_t *br_file_inode(br_file_t *f);
void       *br_file_fpriv(br_file_t *f);
void        br_file_set_fpriv(br_file_t *f, void *p);
br_u32      br_file_flags(br_file_t *f);
br_s64      br_file_offset(br_file_t *f);
void        br_file_set_offset(br_file_t *f, br_s64 off);

/* ---- layer 2: inode ---- */

/* inode public head; the FS private tail follows it. fpriv is an addition so vfs-core can hand it
 * to the file handle at open without knowing the tail layout.
 * type is the semantic truth, not iops == BR_NULL: FSs fill iops differently (tmpfs shares one
 * all-node table, devfs fills it for directories only), so judging by iops would change the errno
 * of "walk a file as a directory" (-ENOENT vs -ENOTDIR).
 * getattr/setattr are inode-level: an inode without iops gets -ENOTSUP from br_stat. */
struct br_inode {
    br_u32                type;    /* BR_INODE_DIR / BR_INODE_FILE */
    const br_inode_ops_t *iops;    /* namespace op set: required for directories, may be BR_NULL otherwise */
    const br_file_ops_t  *fops;    /* file face (directories have one too: read = enumerate entries); may be null */
    void                 *fpriv;   /* file-face private: FS node / device entry; handed to br_file_t at open */
};

void *br_inode_priv(br_inode_t *ino);   /* private tail address for `struct { br_inode_t head; ... }` */

/* Directory inode op set. Path walking is in vfs-core; this table only answers "what is this name
 * at my level". lookup returns a newly created transient inode, or BR_NULL on a miss (not an
 * error: the walk uses it to tell "absent" from "failed"); other slots return a negative errno.
 * link/symlink/readlink were appended at the table end, so existing named initializers are
 * unaffected (unfilled = BR_NULL => -ENOTSUP). vfs-core, not the FS, resolves symlinks: the target
 * may be absolute or relative, and only vfs holds the mount table and full path. */
struct br_inode_ops {
    br_inode_t *(*lookup)(br_inode_t *dir, const char *name);
    int  (*create) (br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out);
    int  (*unlink)(br_inode_t *dir, const char *name);
    int  (*mkdir)(br_inode_t *dir, const char *name);
    int  (*rmdir)(br_inode_t *dir, const char *name);
    int  (*rename)(br_inode_t *dir, const char *name, br_inode_t *ndir, const char *nname);
    int  (*getattr)(br_inode_t *ino, br_stat_t *st);
    int  (*setattr)(br_inode_t *ino, const br_stat_t *st);   /* v1 honors BR_STAT_SIZE only (truncation) */
    /* ---- append-only additions ---- */
    /* Symlink named `name` -> `target`; existence unchecked (dangling is legal); over-long target
     * => -ENAMETOOLONG. */
    int  (*symlink)(br_inode_t *dir, const char *name, const char *target);
    /* Target string of the link itself: returns the full length (truncation is not an error);
     * cap == 0 / buf == NULL reports length only; non-symlink => -EINVAL. */
    int  (*readlink)(br_inode_t *ino, char *buf, br_size_t cap);
    /* Hard link in `dir` to `target` (same FS). Directory => -EPERM; cross-FS => -EXDEV. */
    int  (*link)(br_inode_t *dir, const char *name, br_inode_t *target);
};

/* ---- layer 4: dentry (v1: all BR_NULL, no dcache) ---- */
struct br_dentry_ops {
    int (*revalidate)(void *dentry);
    int (*compare)(void *dentry, const char *name);
};

/* ---- layer 1: super ---- */

/* Mount config. dev: device name for media-backed FSs; BR_NULL for tmpfs/devfs. opts: FS-owned
 * config (reserved, always BR_NULL in v1). */
typedef struct br_fs_cfg {
    const char *path;
    const char *dev;
    const void *opts;
} br_fs_cfg_t;

/* FS-level ops. mount runs once at mount registration and produces the root inode (held by the
 * mount table, not transient); free_inode frees transient walk products, never the root.
 * unmount/sync are v1 signature-first slots => may be BR_NULL. */
struct br_fs_ops {
    int  (*mount)(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
    void (*free_inode)(br_inode_t *ino);
    int  (*unmount)(void *fs_priv);
    int  (*sync)(void *fs_priv);
};

/* Mount an FS. `path` must be absolute and already normalized (no trailing '/', no repeated '/'
 * except the root): /, /dev, /data. Records {path, ops, fs_priv, root}; for a non-root path it
 * auto-mkdirs the mount point in the parent mount, level by level, ignoring an existing one.
 * Errors: -EINVAL (bad path/ops) / -EEXIST (path mounted twice) / -ENOSPC (table full) /
 * -ENODEV (parent mount missing) / otherwise the mount return value. */
int br_mount_register(const char *path, const br_fs_ops_t *ops, void *fs_priv);

/* FS driver registry: FS plugins register "what filesystem I am" in their start phase and the APP
 * mounts a driver by name (POSIX fstype). `name` must match brick.toml; the same name twice =>
 * -EEXIST (overwriting would orphan the earlier registrant's priv). */
int br_fs_driver_register(const char *name, const br_fs_ops_t *ops, void *fs_priv);

/* Driver registry observation (diagnostics/dump). */
br_u32      br_fs_driver_count(void);
const char *br_fs_driver_name_at(br_u32 index);      /* out of range => BR_NULL */

/* Mount by driver name: lookup ops/priv, then as br_mount_register(target, ...) with the media name
 * handed to the FS as cfg.dev. source: device name or BR_NULL when there is no media; flags/data:
 * reserved in v1 (always 0 / BR_NULL). Driver absent => -ENODEV, otherwise as br_mount_register. */
int br_vfs_mount(const char *source, const char *target, const char *fstype,
                 unsigned long flags, const void *data);

/* Mount table observation (diagnostics/dump). */
br_u32 br_mount_count(void);
const char *br_mount_path_at(br_u32 index);          /* out of range => BR_NULL */

/* ---- unified entry ---- */

/* Open through the mount table only (longest-prefix match): normalize -> mount hit -> root inode
 * -> level-by-level iops->lookup -> final inode -> create on a miss with O_CREAT -> allocate
 * br_file_t from the core heap -> fops->open(). Returns a handle, or BR_NULL with no errno set;
 * br_open_err() also writes the code (0 on success, err may be null). */
br_file_t *br_open(const char *path, br_u32 flags);
br_file_t *br_open_err(const char *path, br_u32 flags, int *err);

/* File face (layer-3 dispatch; f == BR_NULL => -EINVAL; missing slot => -ENOTSUP). */
br_s64 br_file_read (br_file_t *f, void *buf, br_size_t n);
br_s64 br_file_write(br_file_t *f, const void *buf, br_size_t n);
br_s64 br_file_lseek(br_file_t *f, br_s64 off, int whence);
int    br_file_ioctl(br_file_t *f, br_u32 cmd, void *arg);
int    br_file_fsync(br_file_t *f);
int    br_file_poll (br_file_t *f, br_u32 *events);   /* v1: query ready bits */
int    br_file_close(br_file_t *f);                   /* free session + handle; idempotent (NULL => 0) */

/* Low-level primitive behind fstat. Mandatory because a consumer cannot obtain the path for an fd
 * (it may have been renamed/unlinked) and must not call iops->getattr itself (the ops table is an
 * FS extension point, not a consumer entry point). */
int br_fstat(br_file_t *f, br_stat_t *st);

/* Path-level convenience surface (1:1 mapping for provider/posix), all via "parent inode + name".
 * br_stat follows a final symlink, br_lstat does not; both follow intermediate components. */
int br_stat    (const char *path, br_stat_t *st);
int br_lstat   (const char *path, br_stat_t *st);
int br_mkdir   (const char *path);
int br_rmdir   (const char *path);
int br_unlink  (const char *path);
int br_rename  (const char *old_path, const char *new_path);
int br_truncate(const char *path, br_u64 size);

/* Links: br_symlink(target, path) creates at path a link to target -- target is neither resolved
 * nor existence-checked (dangling is legal), over BR_SYMLINK_MAX => -ENAMETOOLONG, existing path
 * => -EEXIST. br_readlink reads the link itself (no follow) and returns the full target length
 * (truncation is not an error); buf == NULL/cap == 0 reports length only; non-symlink => -EINVAL,
 * absent => -ENOENT. br_link is a hard link (same data and inode); the final component is not
 * followed (POSIX link(2)); directory => -EPERM, across mounts => -EXDEV. */
int br_symlink (const char *target, const char *path);
int br_readlink(const char *path, char *buf, br_size_t cap);
int br_link    (const char *old_path, const char *new_path);

/* Directory iteration: br_opendir is one O_RDONLY open of the directory inode; br_readdir calls
 * fops->read generically for one br_dirent_t. Returns 1 = got one (fills out), 0 = exhausted,
 * negative = errno. */
br_dir_t *br_opendir(const char *path);
int       br_readdir(br_dir_t *d, br_dirent_t *out);
int       br_closedir(br_dir_t *d);

/* ---- lifecycle ---- */
/* Lifecycle hooks referenced by generated code (see brick.toml): vfs_core_early_init/init/start;
 * start runs this domain's conformance cases. */

/* Conformance cases (self-numbered TC-VFS-*). Prints `[VFSCONF] PASS/FAIL <tag> <desc>` and ends
 * with `[VFSCONF] SUMMARY pass=N fail=M`; returns the failure count. Drives only the public API, so
 * it recognizes no FS name. */
int br_vfs_conformance(void);

#endif /* BR_VFS_BR_VFS_H */
