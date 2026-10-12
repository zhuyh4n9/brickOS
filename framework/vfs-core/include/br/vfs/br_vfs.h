/*
 * brickOS prototype v0.2.0 — VFS contract (the public face of the framework/vfs-core plugin)
 *
 * Design basis (design repo):
 *   - `7-01-vfs.md` §1 (unified openable model SD-1/D21: everything = `br_file_t`, `br_open` **goes only through the mount table**)
 *   - `7-01-vfs.md` §2 (mount and ops four-layer split SD-15/D23: super / inode / file / dentry reserved;
 *     **path walk lives in vfs-core**, inode is v1 **transient** (has inode ops, no cache — SD-3))
 *   - `7-01-vfs.md` §3 (poll staging: v1 only queries ready bits; `poll_attach` slot reserved, NULL → -ENOTSUP)
 *   - `8-01-device.md` §2 (dev-core's open_file hook returns **{fops, fpriv}** — this header provides both types)
 *   - `3-04-memory.md` (identity map + three pools: `br_open` allocates `br_file_t` from the core heap)
 * Per-item rulings and **deviations from the design**: `docs/decisions/0009-vfs-storage-stack.md` (this header is its landing contract).
 *
 * ============================== contract ownership ==============================
 * Every API in this header is implemented by the **framework/vfs-core plugin** — not by core. Design `1-01` §4.5 / D19:
 * capability frameworks exist as **plugins** ("framework piece = plugin identity, core discipline"), so the `br_*`
 * prefix here is **governance discipline** (the API surface enters golden), not "core proper".
 * **vfs-core is pure VFS (D21)**: zero device knowledge — devices attach via `fs/devfs` as /dev nodes.
 * =====================================================================
 *
 * ## Three-layer roles (who implements what)
 *   FS plugins (fs/tmpfs, fs/devfs, fs/littlefs…): fill `br_fs_ops` + `br_inode_ops` + `br_file_ops`,
 *     mount via `br_mount_register()`; **transient inodes are created by the FS** and freed via `free_inode`.
 *   Device frameworks (framework/dev-core + framework/cdev-core): provide the `open_file` hook returning
 *     the {fops, fpriv} pair (D21/D23) — devfs stuffs them into the device node inode.
 *   Consumers (APP / runtime/posix / services): `br_open` + `br_file_*` + `br_opendir/br_readdir`
 *     + the path-level convenience surface (`br_stat`/`br_mkdir`/…).
 *
 * ## Private data of br_file_t (why no "private tail" is needed)
 *   The hook in design `8-01` §2 returns {fops, fpriv}: `fpriv` travels with the **inode** (one per inode),
 *   is handed by vfs to `br_file_t` at open; `fops->open()` **may overwrite** it (storing a session pointer),
 *   and `fops->close()` frees whatever it stored — same shape as Linux `file->private_data`.
 *   So "per-file private state" has a home without making the FS report a "private tail size" (the sched
 *   `tcb_size` trick is unnecessary here). Item-by-item rationale in ADR-0009 §3 ruling 2.
 */
#ifndef BR_VFS_BR_VFS_H
#define BR_VFS_BR_VFS_H

#include <br/core/br_types.h>

/* ==================================================================== open flags */
/* `flags` for `br_open` (append-only; semantics aligned with POSIX O_*, but **values are not reused**). */
#define BR_O_RDONLY    0x0001u
#define BR_O_WRONLY    0x0002u
#define BR_O_RDWR      0x0003u
#define BR_O_ACCMODE   0x0003u   /* access mode mask */
#define BR_O_CREAT     0x0010u   /* create if absent (files only; dirs use br_mkdir) */
#define BR_O_EXCL      0x0020u   /* with O_CREAT: already exists ⇒ -EEXIST */
#define BR_O_TRUNC     0x0040u   /* truncate to 0 on open (needs write permission) */
#define BR_O_APPEND    0x0080u   /* seek to end before each write */
#define BR_O_NONBLOCK  0x0100u   /* non-blocking: no data ⇒ -EAGAIN (device-like; v1 per ADR-0009 §3) */
#define BR_O_NOFOLLOW  0x0200u   /* final component is a symlink ⇒ -ELOOP (not expanded; middle components still are) */

#define BR_O_WRITABLE(f)  (((f) & BR_O_ACCMODE) != BR_O_RDONLY)

/* ==================================================================== lseek */
#define BR_SEEK_SET    0
#define BR_SEEK_CUR    1
#define BR_SEEK_END    2

/* ==================================================================== inode type */
#define BR_INODE_NONE    0u
#define BR_INODE_DIR     1u
#define BR_INODE_FILE    2u
#define BR_INODE_SYMLINK 3u   /* symlink (append-only: added by ADR-0013; old values unchanged) */

/* ==================================================================== poll event bits */
#define BR_POLLIN      0x01u
#define BR_POLLOUT     0x02u
#define BR_POLLERR     0x04u

/* Static upper bound for a directory entry / device name (devfs node name = path component, design `7-01` §1: `[a-z][a-z0-9]*`;
 * for tmpfs file names it is a **static bound** — longer ⇒ the -ENAMETOOLONG substitute is -ENOSPC, see ADR-0009). */
#define BR_NAME_MAX    32u

/* Static upper bound for a symlink **target string** (single one; not the "total resolved length" — the vfs path buffer handles that).
 * Longer ⇒ `-ENAMETOOLONG`. Value 96: much wider than BR_NAME_MAX (a link usually points at a path, not one name),
 * yet smaller than the path buffer, so "a relative target can always be joined" holds by the numeric relation. */
#define BR_SYMLINK_MAX 96u

/* ==================================================================== forward declarations */
typedef struct br_file       br_file_t;       /* opaque open handle (D14) */
typedef struct br_inode      br_inode_t;      /* transient walk product: public head + FS private tail (CA-2) */
typedef struct br_dir        br_dir_t;        /* directory iteration handle */
typedef struct br_file_ops   br_file_ops_t;
typedef struct br_inode_ops  br_inode_ops_t;
typedef struct br_dentry_ops br_dentry_ops_t; /* all NULL in v1 (D23 reserved) */
typedef struct br_fs_ops     br_fs_ops_t;

/* ==================================================================== stat / dirent */

/* Attributes (the getattr/setattr carrier of design `7-01` §2). v1 implements **size only** (truncation);
 * other fields and valid bits are reserved append-only — a valid mask lets us honestly express
 * "which fields this setattr wants to change" instead of guessing via sentinel values.
 * `nlink` is the second field added by ADR-0013 (hard links need it: consumers must see whether
 * another name besides this one points at the same data). For a symlink, `size` = **target string length**. */
#define BR_STAT_SIZE   0x0001u
#define BR_STAT_NLINK  0x0002u

typedef struct br_stat {
    br_u32 valid;   /* BR_STAT_* bits: fields **valid** in this call/answer */
    br_u32 type;    /* BR_INODE_DIR / BR_INODE_FILE / BR_INODE_SYMLINK */
    br_u64 size;    /* bytes (directory = 0; symlink = target string length) */
    br_u32 nlink;   /* hard link count (directory = 1; FS without links may leave 0 and **not set the valid bit**) */
} br_stat_t;

/* Directory entry (design `7-01` §2: produced by read on the file face of a directory inode). Fixed-size record —
 * `read()` fills one and returns `sizeof(br_dirent_t)`, returns 0 when exhausted (the iteration model of `7-01` §2). */
typedef struct br_dirent {
    br_u32 type;                /* BR_INODE_DIR / BR_INODE_FILE */
    br_u32 name_len;            /* excluding the trailing '\0' */
    char   name[BR_NAME_MAX];
} br_dirent_t;

/* ==================================================================== layer 3: file */

/*
 * File-face ops (layer 3 of design `7-01` §1/§2). **Unsupported slot = BR_NULL ⇒ caller gets -ENOTSUP**.
 * `open` is called when the session is established (D23: moved from fs level to file level); `close` pairs with it and
 * frees whatever `open` stored via `br_file_set_fpriv()`.
 *
 * A directory also has a `br_file_ops` (its `read` produces `br_dirent_t`) — so `br_opendir/br_readdir`
 * are **purely generic** code in vfs-core and need no "directory-specific ops" (the layering payoff of D23).
 */
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

/* ---- accessors for the open handle (used by fops implementers; `br_file_t` itself is opaque) ----
 * Rationale: `br_file_ops` slots only receive a `br_file_t*`, while implementations need to know
 * "which inode I am on / my private data / current offset / open flags".
 * vfs-core owns the layout (D14) ⇒ read/write only via accessors; plugins must not touch fields. */
br_inode_t *br_file_inode(br_file_t *f);
void       *br_file_fpriv(br_file_t *f);
void        br_file_set_fpriv(br_file_t *f, void *p);
br_u32      br_file_flags(br_file_t *f);
br_s64      br_file_offset(br_file_t *f);
void        br_file_set_offset(br_file_t *f, br_s64 off);

/* ==================================================================== layer 2: inode */

/*
 * inode **public head** (object model of design `7-01` §2; the FS private tail follows it — CA-2 same shape).
 *
 * ★ The **only difference** from the design: an extra `fpriv` field (the design says "a file inode carries the
 *   {fops, fpriv} pair" but placed only `fops` in the public head). Reason: vfs-core must **hand** `fpriv`
 *   to the file handle at `open`, yet must not know any FS's private tail layout; without this field every FS
 *   would need another "get fpriv" op (i.e. another interface surface). Recorded as an increment in ADR-0009 §3 ruling 2.
 *
 * ★ `type` is the **single semantic truth**, not `iops == BR_NULL`:
 *   vfs-core always checks `type` to decide "is this a directory" (see walk_to in src/br_vfs.c). The reason is that
 *   FSs fill `iops` differently — e.g. fs/tmpfs makes **all** nodes share one table with lookup (slots self-check by
 *   type), while fs/devfs fills it only for directories. If vfs judged by `iops == NULL`, the same "walk a file as a
 *   directory" call would yield a different errno under the two FSs (-ENOENT vs -ENOTDIR).
 *   So `iops == BR_NULL` only means "this namespace operation set does not exist", not "this is not a directory".
 *
 * ★ `getattr`/`setattr` belong to **this layer** (inode level), not the file level — so **every** inode supporting
 *   `br_stat` must carry an iops (even if only getattr): device file nodes are exactly that (devfs gives them a
 *   table with only getattr and all namespace-change slots empty). `br_stat` on a node with `iops = BR_NULL` gets
 *   `-ENOTSUP` — a direct consequence of the Linux `i_op`/`f_op` split, not a limitation.
 */
struct br_inode {
    br_u32                type;    /* BR_INODE_DIR / BR_INODE_FILE */
    const br_inode_ops_t *iops;    /* namespace op set: required for directories, may be BR_NULL otherwise */
    const br_file_ops_t  *fops;    /* file face (directories have one too: read = enumerate entries); may be null */
    void                 *fpriv;   /* file-face private: FS node / device entry; handed to br_file_t at open */
};

/* inode private tail entry (convenience address when an FS uses `struct { br_inode_t head; ... }`) */
void *br_inode_priv(br_inode_t *ino);

/*
 * Directory inode op set (layer 2 of design `7-01` §2). **Path walking (level-by-level descent/name comparison) is in vfs-core**;
 * this table only answers "what is this name at my level".
 * `lookup` returns a **newly created transient inode**, and `BR_NULL` on a miss (**not** an error code — the walk relies on it
 * to tell "absent" from "failed"); the other slots return a negative errno.
 *
 * ★ The last three slots (link/symlink/readlink) were **added by ADR-0013** (design `7-01` §2 originally wrote them
 *   as "v2 reserved: symlink / readlink / mknod"). Appending at the **table end** ⇒ named initialization in existing FSs
 *   is unaffected (unfilled = BR_NULL ⇒ that FS does not support it, vfs gets -ENOTSUP). `mknod` still takes no slot.
 *
 * **Who resolves symlinks**: **vfs-core**, not the FS. Reason: the target string may be an absolute path (needing
 * a restart of mount-table matching) or a relative path (relative to "the directory containing the link"), and only
 * the vfs, holding the mount table and the full path, can do either. The FS only supplies "the read-out target string"
 * (`readlink`) and "create a link" (`symlink`).
 */
struct br_inode_ops {
    br_inode_t *(*lookup)(br_inode_t *dir, const char *name);
    int  (*create) (br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out);
    int  (*unlink)(br_inode_t *dir, const char *name);
    int  (*mkdir)(br_inode_t *dir, const char *name);
    int  (*rmdir)(br_inode_t *dir, const char *name);
    int  (*rename)(br_inode_t *dir, const char *name, br_inode_t *ndir, const char *nname);
    int  (*getattr)(br_inode_t *ino, br_stat_t *st);
    int  (*setattr)(br_inode_t *ino, const br_stat_t *st);   /* v1 honors BR_STAT_SIZE only (truncation) */
    /* ---- ADR-0013 additions (append-only) ---- */
    /* Create in `dir` a symlink named `name` with target string = `target` (existence is not checked —
     * a dangling link is a legal state, as in POSIX). Over-long target ⇒ -ENAMETOOLONG. */
    int  (*symlink)(br_inode_t *dir, const char *name, const char *target);
    /* Read the target string of the **link itself**. Filling `cap` is not a truncation error; instead the **full length** is
     * returned (following POSIX readlink); with `cap == 0` or `buf == NULL` only the length is reported. Non-symlink ⇒ -EINVAL. */
    int  (*readlink)(br_inode_t *ino, char *buf, br_size_t cap);
    /* Create in `dir` a **hard link** named `name` pointing at `target` (an inode in the same FS).
     * Directories cannot be linked (⇒ -EPERM, as in POSIX); cross-FS is blocked earlier by vfs (⇒ -EXDEV). */
    int  (*link)(br_inode_t *dir, const char *name, br_inode_t *target);
};

/* ==================================================================== layer 4: dentry */
/* v1: **all BR_NULL** (SD-3: no dcache); slots reserved now so v2 need not change layout (D14/D23). */
struct br_dentry_ops {
    int (*revalidate)(void *dentry);
    int (*compare)(void *dentry, const char *name);
};

/* ==================================================================== layer 1: super */

/* Mount config (the `br_fs_cfg_t` of design `7-01` §2). `dev`: device name for media-backed FSs (littlefs/EROFS)
 * (from the manifest mount plan); tmpfs/devfs = BR_NULL. `opts`: FS-owned config (reserved, always BR_NULL in v1). */
typedef struct br_fs_cfg {
    const char *path;
    const char *dev;
    const void *opts;
} br_fs_cfg_t;

/*
 * FS-level ops (layer 1). `mount` is called once at **mount registration** and produces the root inode (held by the mount
 * table, not transient); `free_inode` frees **transient** inodes produced by walks (the root inode held by the mount table
 * is never passed in — see the contract of `br_mount_register`).
 * `unmount`/`sync` are v1 **signature-first** slots (runtime mount/unmount = O-S3, no earlier than v3) ⇒ may be BR_NULL.
 */
struct br_fs_ops {
    int  (*mount)(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
    void (*free_inode)(br_inode_t *ino);
    int  (*unmount)(void *fs_priv);
    int  (*sync)(void *fs_priv);
};

/*
 * Mount an FS (signature of design `7-01` §2).
 *   `path` must be absolute and **already normalized** (no trailing '/', no repeated '/' except the root): `/`, `/dev`, `/data`.
 *   Semantics: build cfg{path} → `ops->mount(fs_priv, cfg, &root)` → record the slot {path, ops, fs_priv, root};
 *         if `path != "/"`, auto-mkdir the mount point in the **parent mount** (design `7-03` §6: "auto-mkdir the mount point
 *         when missing in the parent FS", creating intermediate components level by level; ignore if it already exists).
 *   Errors: `-EINVAL` (bad or non-normalized path/ops) / `-EEXIST` (same path mounted twice) /
 *         `-ENOSPC` (mount table full) / `-ENODEV` (parent mount missing) / otherwise = mount's return value passed through.
 */
int br_mount_register(const char *path, const br_fs_ops_t *ops, void *fs_priv);

/* ==================================================================== FS driver registry
 *
 * ★ Why it exists (ADR-0029): **mounting is decided by the APP**. FS plugins no longer mount themselves; in their
 *   start phase they register "what filesystem I am" as a **driver**; the APP uses POSIX `mount(source, target,
 *   fstype, …)` to mount a driver at some mount point **by name** (`"fat"`/`"erofs"`/`"tmpfs"`/`"devfs"`).
 *   Thus `brick.toml`'s `[[mount]]` no longer means "where this plugin mounts"; the APP's
 *   `[[mount]]` means "what to mount" — the mount plan belongs to the **consumer**.
 *
 * The spelling of `name` is a driver contract (the APP's `[[mount]].fstype` looks it up): it must match what
 * is declared in brick.toml; registering the same name twice ⇒ `-EEXIST` (no overwrite: overwriting would orphan
 * the earlier registrant's priv).
 */
int br_fs_driver_register(const char *name, const br_fs_ops_t *ops, void *fs_priv);

/* Driver registry observation (diagnostics/dump). */
br_u32      br_fs_driver_count(void);
const char *br_fs_driver_name_at(br_u32 index);      /* out of range ⇒ BR_NULL */

/*
 * Mount by **driver name**: look up ops/priv ⇒ equivalent to `br_mount_register(target, ops, priv)`,
 * but also hands the media name (`source`) to the FS (`cfg.dev`).
 *   `source`: device name for media-backed FSs (`"emmc0"`); BR_NULL when there is no media (tmpfs/devfs).
 *   `flags` / `data`: reserved in v1 (the last two POSIX `mount()` arguments; always 0 / BR_NULL this generation).
 * Errors: driver absent ⇒ `-ENODEV`; otherwise as in `br_mount_register`.
 * ★ This is the **only** landing point of POSIX `mount()` on the vfs side — `runtime/posix` only forwards and shapes arguments.
 */
int br_vfs_mount(const char *source, const char *target, const char *fstype,
                 unsigned long flags, const void *data);

/* Mount table observation (diagnostics/dump; append-only increment, same shape as lookup by name). */
br_u32 br_mount_count(void);
const char *br_mount_path_at(br_u32 index);          /* out of range ⇒ BR_NULL */

/* ==================================================================== unified entry */

/*
 * Open (SD-1 of design `7-01` §1: **through the mount table only**, longest-prefix match). Full flow:
 *   normalize path → longest-prefix mount hit → root inode → level-by-level `iops->lookup` → final inode
 *   → `O_CREAT` and a miss ⇒ `iops->create` in the parent directory → allocate `br_file_t` (core heap)
 *   → `fops->open(f, flags)` to build the session.
 * Returns a handle, or `BR_NULL` on failure with **no errno set** (prototype convention: return NULL; callers needing
 * an error code use the `br_open_err()` variant below — the only entry through which runtime/posix maps errno back to user space).
 *
 * `br_open_err(path, flags, int *err)` is the same implementation: `err` may be null; 0 is written on success.
 */
br_file_t *br_open(const char *path, br_u32 flags);
br_file_t *br_open_err(const char *path, br_u32 flags, int *err);

/* File face (layer-3 dispatch; `f == BR_NULL` ⇒ -EINVAL; missing slot ⇒ -ENOTSUP). */
br_s64 br_file_read (br_file_t *f, void *buf, br_size_t n);
br_s64 br_file_write(br_file_t *f, const void *buf, br_size_t n);
br_s64 br_file_lseek(br_file_t *f, br_s64 off, int whence);
int    br_file_ioctl(br_file_t *f, br_u32 cmd, void *arg);
int    br_file_fsync(br_file_t *f);
int    br_file_poll (br_file_t *f, br_u32 *events);   /* v1: query ready bits (SD-7) */
int    br_file_close(br_file_t *f);                   /* free session + handle; idempotent (NULL ⇒ 0) */

/*
 * The low-level primitive behind `fstat` (ADR-0013): attributes of an already-open handle.
 * Why it is mandatory: a consumer cannot obtain "the path for this fd" (it may already have been renamed/unlinked),
 * and although `br_inode_t` is visible in the header, having consumers call `iops->getattr` themselves would **cross the contract**
 * (the ops table is an extension point for FS implementers, not an entry point for consumers). Providing this layer is what makes
 * runtime/posix's `fstat` a true "1:1 mapping" instead of "reaching into the inode".
 */
int br_fstat(br_file_t *f, br_stat_t *st);

/* Path-level convenience surface (1:1 mapping for runtime/posix; all go through the two steps "parent directory inode + name").
 *
 * `br_stat` **follows** a final symlink (POSIX `stat`); `br_lstat` does **not** follow (POSIX `lstat`).
 * Both follow intermediate components. */
int br_stat    (const char *path, br_stat_t *st);
int br_lstat   (const char *path, br_stat_t *st);
int br_mkdir   (const char *path);
int br_rmdir   (const char *path);
int br_unlink  (const char *path);
int br_rename  (const char *old_path, const char *new_path);
int br_truncate(const char *path, br_u64 size);

/* ==================================================================== links (ADR-0013)

 * `br_symlink(target, path)`: create at `path` a symlink pointing at `target`.
 *   `target` is **neither resolved nor existence-checked** (a dangling link is legal); over `BR_SYMLINK_MAX` ⇒ -ENAMETOOLONG;
 *   `path` already exists ⇒ -EEXIST.
 * `br_readlink(path, buf, cap)`: read the target string of the **link itself** (no following).
 *   Returns the **full length** of the target string (per POSIX: truncation is not an error); with `buf == NULL`/`cap == 0` only the
 *   length is reported; `path` is not a symlink ⇒ -EINVAL; absent ⇒ -ENOENT.
 * `br_link(old_path, new_path)`: hard link — two names pointing at **the same data and the same inode**.
 *   `old_path` absent ⇒ -ENOENT; intermediate components follow symlinks, **does the final one not**? — no:
 *   POSIX `link(2)` **does not follow** the final component by default (Linux semantics), and we follow suit (use `br_stat` first to follow).
 *   Directory ⇒ -EPERM; across mounts ⇒ -EXDEV (`EXDEV` is already in the errno table, first enabled by this cut). */
int br_symlink (const char *target, const char *path);
int br_readlink(const char *path, char *buf, br_size_t cap);
int br_link    (const char *old_path, const char *new_path);

/* ==================================================================== directory iteration */

/*
 * Directory iteration (design `7-01` §2: "the file face of a directory inode (opendir → br_dir_t iteration)").
 * `br_opendir` = one `O_RDONLY` open of the directory inode; `br_readdir` generically calls `fops->read`
 * in vfs-core to fetch one `br_dirent_t`.
 * Returns: `1` = got one (fills `out`); `0` = directory exhausted; negative = errno.
 */
br_dir_t *br_opendir(const char *path);
int       br_readdir(br_dir_t *d, br_dirent_t *out);
int       br_closedir(br_dir_t *d);

/* ==================================================================== lifecycle */

/* This plugin's lifecycle hooks are referenced by generated code (see brick.toml): `vfs_core_early_init/init/start`.
 * The `start` phase runs this domain's conformance cases (the APP's start also calls `br_vfs_conformance()` once; see ADR-0009 §3). */

/*
 * Conformance cases (self-numbered `TC-VFS-*`; this group is absent from the 6-01 case table ⇒ recorded in `br-wa-test-001`).
 * Prints `[VFSCONF] PASS/FAIL <tag> <desc>` and ends with `[VFSCONF] SUMMARY pass=N fail=M`;
 * returns the failure count. **Purely generic**: it drives only the public API of this header (mount table + path walks + file/directory faces),
 * so it treats "rootfs is tmpfs" merely as "the mount table has a '/'", recognizing no FS name.
 */
int br_vfs_conformance(void);

#endif /* BR_VFS_BR_VFS_H */
