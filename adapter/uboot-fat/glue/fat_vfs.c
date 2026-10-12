/*
 * adapter/uboot-fat — VFS integration + lifecycle hooks (new here, no upstream counterpart)
 *
 * ============================== what this file does ==============================
 * Connect the **upstream path-style API** (`fat_exists`/`fat_read_file`/`file_fat_write_at`/
 * `fat_unlink`/…, defined in the single compilation unit `third_party/uboot-imx/fs/fat/fat_write.c`)
 * to this project's VFS contract
 * (framework/vfs-core/include/br/vfs/br_vfs.h):
 *
 *   layer 1 (super): mount (read BPB, decide 12/16/32 + check volume geometry) / sync (bdev flush) /
 *                    free_inode / unmount (BR_NULL)
 *   layer 2 (inode): lookup / create / unlink / mkdir / rmdir / rename / getattr / setattr
 *   layer 3 (file):  file side read/write/lseek/close; dir side read (one br_dirent_t)/close
 *
 * ## inode = transient **path wrapper** (design SD-3, opposite of fs/tmpfs's "persistent node")
 *   Each lookup/create br_malloc's a `uboot_fat_ino_t`:
 *       { volume id, path relative to mount point (root = ""), type, size snapshot }
 *   So a "node" needs no on-media index — the glue hands the path to the upstream path API.
 *   Ownership: **whoever holds it frees it**. free_inode must br_free it (br_vfs.c ino_put contract);
 *   the only exception is the **mount root**: mount allocates it and it lives on uboot_fat_fs_t; vfs
 *   never passes it to free_inode — but it must still br_malloc (never BSS, see glue/fat_glue.h).
 *
 * ## why this layer synthesizes "." and ".." for directory reads
 *   FAT's **root directory** has no "." / ".." (non-FAT32 roots are not even on the cluster chain),
 *   while VFS consumers (and TC-VFS-*) treat it as a POSIX directory ⇒ synthesize the first two here
 *   (same as fs/tmpfs).
 *
 * ## intentional deviations / debt (see README)
 *   · `link`/`symlink`/`readlink` slots are **unfilled** (BR_NULL ⇒ -ENOTSUP): FAT has no links.
 *   · name bound: FAT long names reach 255 bytes, but br_dirent_t.name is only BR_NAME_MAX(32) ⇒
 *     names > 31 bytes return -ENOSPC from readdir (same rule as fs/tmpfs, see uboot_fat_name_ok).
 *   · `BR_O_APPEND` and `O_TRUNC` are handled in fops->open; permission checks (no write on a
 *     read-only fd) belong to vfs/posix.
 * ==========================================================================
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "fat_glue.h"

/* ==================================================================== mount table (plugin-private "mount point → volume")
 * ★ why keep our own table: vfs's mount table only records {path, ops, fs_priv, root}; but the
 *   mapping "which mount point maps to which block device" is now **the mount plan's fact**
 *   (ADR-0030): the APP calls `mount(source, target, "fat", …)` handing (dev, mnt) to this file,
 *   and `uboot_fat_vol_claim()` in `glue/fat_blk.c` claims a volume slot.
 *   Only the id is cached here for mount; no duplicate root inode with vfs's table.
 */
typedef struct uboot_fat_mount_slot {
    /* ★ the mount point is **our own copy** (fixed-size, bounded), not a vfs pointer: `cfg->path`
     *   points at a **stack buffer** (`norm`) inside `br_vfs_mount()` and dies when mount returns —
     *   storing the pointer reads overwritten stack in self-tests (observed: `TC-FAT-001
     *   vol_info_failed` / `TC-FAT-008 fs_ops.sync_rc=-2`, while the mount-time log looked fine).
     *   Bound = UB_FAT_PATH_MAX (this plugin's path rule). */
    char            mnt[UB_FAT_PATH_MAX];
    uboot_fat_fs_t *fs;      /* set only after a successful mount */
    br_bool         mounted;
} uboot_fat_mount_slot_t;

static uboot_fat_mount_slot_t s_slots[UB_FAT_VOL_MAX];

/* ==================================================================== ops tables (forward decls first) */

static br_inode_t *uboot_fat_lookup (br_inode_t *dir, const char *name);
static int  uboot_fat_create (br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out);
static int  uboot_fat_unlink (br_inode_t *dir, const char *name);
static int  uboot_fat_mkdir  (br_inode_t *dir, const char *name);
static int  uboot_fat_rmdir  (br_inode_t *dir, const char *name);
static int  uboot_fat_rename (br_inode_t *dir, const char *name,
                              br_inode_t *ndir, const char *nname);
static int  uboot_fat_getattr(br_inode_t *ino, br_stat_t *st);
static int  uboot_fat_setattr(br_inode_t *ino, const br_stat_t *st);

static int   uboot_fat_file_open (br_file_t *f, br_u32 flags);
static br_s64 uboot_fat_file_read (br_file_t *f, void *buf, br_size_t n);
static br_s64 uboot_fat_file_write(br_file_t *f, const void *buf, br_size_t n);
static br_s64 uboot_fat_file_lseek(br_file_t *f, br_s64 off, int whence);
static int   uboot_fat_file_close(br_file_t *f);

static int   uboot_fat_fsync    (br_file_t *f);
static int   uboot_fat_dir_open (br_file_t *f, br_u32 flags);
static br_s64 uboot_fat_dir_read (br_file_t *f, void *buf, br_size_t n);
static int   uboot_fat_dir_close(br_file_t *f);

static int  uboot_fat_mount     (void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
static void uboot_fat_free_inode(br_inode_t *ino);
static int  uboot_fat_sync      (void *fs_priv);

/*
 * Volume-level flush (**the only implementation**): make the block device flush.
 *
 * Why only one place: `fs_ops.sync` and `br_file_ops.fsync` are two entries (volume/handle) to the
 * same thing; two copies = a second truth that will drift.
 *
 * ★ after ADR-0026 this is **only** bdev.flush — there is no "glue-held FAT cache" anymore:
 *   each upstream write entry (`file_fat_write_at`/`fat_unlink`/`fat_mkdir`/`fat_rename`) flushes
 *   its dirty FAT window (`flush_dirty_fat_buffer`) and directory entries (`flush_dir`) internally,
 *   and its `fsdata` is a per-call local freed before return (the upstream model; the earlier
 *   "fsdata owned by the volume" was our deviation, reverted with ADR-0026). So no unflushed plugin
 *   state spans two API calls, and `sync` degrades to "make the device really commit written
 *   sectors" (SD-8 rule of design `7-02` §1).
 */
static int uboot_fat_flush_volume(uboot_fat_vol_t *vol)
{
    int rc;

    if (vol == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if ((vol->bound == BR_FALSE) || (vol->ops == BR_NULL)) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    return (vol->ops->flush(vol->priv) == 0) ? 0 : BR_ERR(EIO);
}

/* file side: six slots filled + three explicit BR_NULL (contract: unsupported slot = BR_NULL ⇒ -ENOTSUP) */
static const br_file_ops_t s_file_fops = {
    .open        = uboot_fat_file_open,
    .read        = uboot_fat_file_read,
    .write       = uboot_fat_file_write,
    .lseek       = uboot_fat_file_lseek,
    .ioctl       = BR_NULL,
    .fsync       = uboot_fat_fsync,   /* same as fs_ops.sync: write back FAT cache, then flush device */
    .poll        = BR_NULL,   /* block media are always ready; the wait face is v2 (SD-7) */
    .poll_attach = BR_NULL,   /* reserved for v2 (wait-queue); always NULL in v1 */
    .close       = uboot_fat_file_close,
};

/* dir side (read = one br_dirent_t). write/seek have no meaning for a dir; slots left empty. */
static const br_file_ops_t s_dir_fops = {
    .open        = uboot_fat_dir_open,
    .read        = uboot_fat_dir_read,
    .write       = BR_NULL,
    .lseek       = BR_NULL,
    .ioctl       = BR_NULL,
    .fsync       = uboot_fat_fsync,   /* a dir handle's "flush" = this volume's flush */
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = uboot_fat_dir_close,
};

/*
 * Layer 2: inode ops. ★ dirs and files **share one table**: lookup/create/… slots check the type
 * themselves, and a "file node" only ever reaches getattr/setattr (truncate), since vfs dispatches
 * namespace ops to dirs only. Why not share a single table like tmpfs: a namespace op dispatched to
 * a file node must give a clear -ENOTDIR/-EISDIR rather than "the table happened to implement it" —
 * so this file splits by **type** (only two tables, zero cost).
 */
static const br_inode_ops_t s_dir_iops = {
    .lookup  = uboot_fat_lookup,
    .create  = uboot_fat_create,
    .unlink  = uboot_fat_unlink,
    .mkdir   = uboot_fat_mkdir,
    .rmdir   = uboot_fat_rmdir,
    .rename  = uboot_fat_rename,
    .getattr = uboot_fat_getattr,
    .setattr = uboot_fat_setattr,
    /* ADR-0013 addition (table tail): FAT has no links ⇒ three slots explicitly BR_NULL (-ENOTSUP) */
    .symlink  = BR_NULL,
    .readlink = BR_NULL,
    .link     = BR_NULL,
};

/* file node iops: only getattr/setattr are meaningful (vfs never dispatches namespace ops to a
 * non-dir, while br_fstat / br_truncate come through here). */
static const br_inode_ops_t s_file_iops = {
    .lookup  = BR_NULL,
    .create  = BR_NULL,
    .unlink  = BR_NULL,
    .mkdir   = BR_NULL,
    .rmdir   = BR_NULL,
    .rename  = BR_NULL,
    .getattr = uboot_fat_getattr,
    .setattr = uboot_fat_setattr,
    .symlink  = BR_NULL,
    .readlink = BR_NULL,
    .link     = BR_NULL,
};

static const br_fs_ops_t s_fs_ops = {
    .mount      = uboot_fat_mount,
    .free_inode = uboot_fat_free_inode,
    .unmount    = BR_NULL,   /* v1 signature-only slot (runtime unmount no earlier than v3, O-S3) */
    .sync       = uboot_fat_sync,
};

/* ==================================================================== small helpers */

/*
 * Bounded copy (cap **includes** the trailing '\0'). ★ deliberately named apart from
 * `uboot_fat_strcpy()` in the compat env header (2 args, bound 256 — that env implementation is
 * **for upstream**): this is glue's own helper, with the length given by the caller (this plugin's
 * path buffer `UB_FAT_PATH_MAX`).
 */
static void uboot_fat_strcpy_bounded(char *dst, const char *src, br_size_t cap)
{
    br_size_t i = 0u;

    if (cap == 0u) {
        return;
    }
    while ((i + 1u) < cap) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return;
        }
        i++;
    }
    dst[cap - 1u] = '\0';
}

/*
 * Name rule (single enforcement point; lookup/create/mkdir/rmdir/rename all go through it):
 *   non-empty, no '/', length ≤ UB_FAT_NAME_MAX-1 (= BR_NAME_MAX-1 = 31).
 * Why a **length bound** instead of "the 255 FAT allows": this repo's dirent is the fixed-size
 * br_dirent_t with name[] only 32 bytes ⇒ a long name could be created but not listed, which is
 * worse than rejecting it (same rule as fs/tmpfs). Scan byte by byte until NUL or BR_NAME_MAX ⇒ an
 * unterminated string is **read as 32 bytes only**.
 */
br_bool uboot_fat_name_ok(const char *name)
{
    br_u32 i;

    if (name == BR_NULL) {
        return BR_FALSE;
    }
    if (name[0] == '\0') {
        return BR_FALSE;
    }
    for (i = 0u; i < UB_FAT_NAME_MAX; i++) {
        if (name[i] == '\0') {
            return BR_TRUE;
        }
        if (name[i] == '/') {
            return BR_FALSE;
        }
    }
    return BR_FALSE;                 /* window filled without NUL ⇒ longer than BR_NAME_MAX-1 */
}

/* is the name "." / ".."? (mutating ops reject explicitly; root self-reference is in lookup) */
static br_bool uboot_fat_name_dot(const char *name)
{
    if (name == BR_NULL) {
        return BR_FALSE;
    }
    if ((name[0] == '.') && (name[1] == '\0')) {
        return BR_TRUE;
    }
    if ((name[0] == '.') && (name[1] == '.') && (name[2] == '\0')) {
        return BR_TRUE;
    }
    return BR_FALSE;
}

/*
 * Join "parent-relative path + name" → relative path (the only path constructor in this plugin).
 *   dir_path == "" ⇒ the result is just name (a direct child of the mount root)
 *   dir_path != "" ⇒ "dir_path/name"
 * Overwrites out (caller-provided stack buffer, cap ≥ UB_FAT_PATH_MAX). Returns 0 / -ENOSPC.
 */
int uboot_fat_join_path(const char *dir_path, const char *name,
                        char *out, br_size_t cap)
{
    br_size_t n = 0u;
    br_size_t i;

    if ((dir_path == BR_NULL) || (name == BR_NULL) || (out == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if (dir_path[0] != '\0') {
        for (i = 0u; dir_path[i] != '\0'; i++) {
            if ((n + 1u) >= cap) {
                return BR_ERR(ENOSPC);
            }
            out[n++] = dir_path[i];
        }
        if ((n + 1u) >= cap) {
            return BR_ERR(ENOSPC);
        }
        out[n++] = '/';
    }
    for (i = 0u; name[i] != '\0'; i++) {
        if ((n + 1u) >= cap) {
            return BR_ERR(ENOSPC);
        }
        out[n++] = name[i];
    }
    out[n] = '\0';
    return 0;
}

/* parent of a relative path (truncate in place): parent of "" is ""; of "a" is ""; of "a/b" is "a". */
static void uboot_fat_parent_path(const char *path, char *out, br_size_t cap)
{
    br_size_t n = uboot_fat_strlen(path);

    while ((n > 0u) && (path[n - 1u] != '/')) {
        n--;
    }
    if ((n > 0u) && (path[n - 1u] == '/')) {
        n--;                          /* drop the separator itself */
    }
    if (n >= cap) {
        n = (cap > 0u) ? (cap - 1u) : 0u;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

/* depth of a relative path (component count; "" = 0). Gives "loops" a static bound (matches vfs). */
static br_u32 uboot_fat_path_depth(const char *path)
{
    br_u32 depth = 0u;

    for (br_size_t i = 0u; path[i] != '\0'; i++) {
        if ((i == 0u) || ((path[i] != '/') && (path[i - 1u] == '/'))) {
            depth++;
        }
    }
    return depth;
}

/*
 * **Session state** of a dir handle: upstream `fat_opendir()` returns an opaque
 * `struct fs_dir_stream *` (its concrete `fat_dir` type is private to that upstream compilation
 * unit, invisible to glue), while the VFS-side dir read also synthesizes two "."/".." records ⇒ the
 * cursor must be glue-owned. So every open makes a wrapper {upstream stream, cursor}; close returns
 * both.
 * Cursor: 0 = ".", 1 = "..", ≥2 = the (cursor-2)-th real directory entry.
 */
typedef struct uboot_fat_dirstream {
    struct fs_dir_stream *ds;      /* stream from upstream fat_opendir (given back by fat_closedir) */
    br_u32                cursor;  /* vfs-side iteration cursor (see above) */
} uboot_fat_dirstream_t;

/* node identity: head.fpriv == the node itself (CA-2 "first field" layout; same as tmpfs) */
static uboot_fat_ino_t *uboot_fat_node(const br_inode_t *ino)
{
    return (uboot_fat_ino_t *)ino->fpriv;
}

/* make a transient node. `type` = BR_INODE_DIR/BR_INODE_FILE; `size` matters for files only. */
static uboot_fat_ino_t *uboot_fat_node_new(br_u32 vol, br_u32 type,
                                           const char *path, br_u32 size)
{
    uboot_fat_ino_t *n = (uboot_fat_ino_t *)br_malloc(sizeof(*n));

    if (n == BR_NULL) {
        return BR_NULL;
    }
    n->head.type  = type;
    n->head.iops  = (type == BR_INODE_DIR) ? &s_dir_iops : &s_file_iops;
    n->head.fops  = (type == BR_INODE_DIR) ? &s_dir_fops : &s_file_fops;
    n->head.fpriv = n;
    n->vol        = (br_u8)vol;
    n->type       = (br_u8)type;
    n->size       = size;
    /* the path was already length-bounded by uboot_fat_join_path; this second copy is pure defense. */
    uboot_fat_strcpy_bounded(n->path, path, UB_FAT_PATH_MAX);
    return n;
}

/* current length for a file node only (dir ⇒ -EISDIR). Empty path (= volume root) is a dir too. */
static int uboot_fat_size_of(const uboot_fat_ino_t *n, br_u32 *out)
{
    loff_t sz = 0;
    uboot_fat_vol_t *vol;
    int rc;

    vol = uboot_fat_vol_get(n->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    if (n->path[0] == '\0') {
        return BR_ERR(EISDIR);          /* volume root ⇒ directory */
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    if (fat_exists(n->path) == 0) {
        /* ★ key: check existence first. u-boot's fs_size() **deliberately returns success + size 0**
         * for "this is a directory" (its comment says fs_size must not fail on dirs) — using it
         * directly would treat a directory as a 0-byte file. */
        return BR_ERR(ENOENT);
    }
    rc = fat_size(n->path, &sz);
    if (rc < 0) {
        return rc;
    }
    if (sz < 0) {
        return BR_ERR(EIO);
    }
    *out = (br_u32)sz;
    return 0;
}

/* path of a dirent → type (existence + type decided in one pass). Miss ⇒ -ENOENT. */
static int uboot_fat_kind_of(const uboot_fat_ino_t *n, br_bool *is_dir)
{
    uboot_fat_vol_t *vol = uboot_fat_vol_get(n->vol);
    int rc;

    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    if (n->path[0] == '\0') {
        *is_dir = BR_TRUE;               /* volume root */
        return 0;
    }
    /* dirs: only a directory can be opened by fat_opendir (a file ⇒ -ENOTDIR) */
    {
        struct fs_dir_stream *ds = BR_NULL;
        int rc2 = fat_opendir(n->path, &ds);

        if (rc2 == 0) {
            fat_closedir(ds);
            *is_dir = BR_TRUE;
            return 0;
        }
    }
    if (fat_exists(n->path) != 0) {
        *is_dir = BR_FALSE;
        return 0;
    }
    return BR_ERR(ENOENT);
}

/* volume-absolute path (upstream style): "/hello.txt" / "/dir" / "/". Returns 0 or negative errno. */
static int uboot_fat_raw_path(const uboot_fat_ino_t *n, char *out, br_size_t cap)
{
    const br_size_t need = uboot_fat_strlen(n->path) + 2u;

    if ((out == BR_NULL) || (need > cap)) {
        return BR_ERR(ENOSPC);
    }
    out[0] = '/';
    memcpy(out + 1, n->path, uboot_fat_strlen(n->path) + 1u);
    return 0;
}

/* dirent name → br_dirent_t (VFS's fixed-size record). Name over the bound ⇒ -ENOSPC (see header). */
static int uboot_fat_fill_dirent(br_dirent_t *de, br_u32 type, const char *name)
{
    const br_size_t len = uboot_fat_strlen(name);

    if (len >= (br_size_t)UB_FAT_NAME_MAX) {
        return BR_ERR(ENOSPC);
    }
    memset(de, 0, sizeof(*de));
    de->type     = type;
    de->name_len = (br_u32)len;
    memcpy(de->name, name, len + 1u);
    return 0;
}

/* ==================================================================== layer 3: file side */

static int uboot_fat_file_open(br_file_t *f, br_u32 flags)
{
    uboot_fat_ino_t *n = (uboot_fat_ino_t *)br_file_fpriv(f);
    int rc;

    if ((n == BR_NULL) || (n->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);
    }
    /* O_TRUNC: truncate to 0 on open (same truncate primitive, no second "empty it" path) */
    if ((flags & BR_O_TRUNC) != 0u) {
        char raw[UB_FAT_PATH_MAX];

        rc = uboot_fat_raw_path(n, raw, sizeof(raw));
        if (rc != 0) {
            return rc;
        }
        rc = uboot_fat_truncate(raw, 0u);
        if (rc != 0) {
            return rc;
        }
        n->size = 0u;
    }
    if ((flags & BR_O_APPEND) != 0u) {
        br_u32 sz = 0u;

        rc = uboot_fat_size_of(n, &sz);      /* read the truth from media on every open; no cache */
        if (rc != 0) {
            return rc;
        }
        n->size = sz;
        br_file_set_offset(f, (br_s64)sz);
    }
    return 0;
}

static br_s64 uboot_fat_file_read(br_file_t *f, void *buf, br_size_t n)
{
    uboot_fat_ino_t *node = (uboot_fat_ino_t *)br_file_fpriv(f);
    uboot_fat_vol_t *vol;
    char raw[UB_FAT_PATH_MAX];
    br_u32 size = 0u;
    br_s64 off;
    br_size_t want;
    loff_t act = 0;
    int rc;

    if ((node == BR_NULL) || (node->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);
    }
    if (n == 0u) {
        return 0;                            /* an empty read is a no-op */
    }
    rc = uboot_fat_size_of(node, &size);
    if (rc != 0) {
        return rc;
    }
    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(EINVAL);
    }
    if ((br_u64)off >= (br_u64)size) {
        return 0;                            /* at/over EOF: 0 bytes is **success** */
    }

    vol = uboot_fat_vol_get(node->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_raw_path(node, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    want = n;
    if (want > (br_size_t)((br_u64)size - (br_u64)off)) {
        want = (br_size_t)((br_u64)size - (br_u64)off);
    }
    /* ★ len must be given explicitly as want: upstream `len == 0` means "read to EOF", unlike POSIX. */
    rc = fat_read_file(raw, buf, (loff_t)off, (loff_t)want, &act);
    if (rc < 0) {
        return rc;
    }
    br_file_set_offset(f, off + (br_s64)act);
    return (br_s64)act;
}

static br_s64 uboot_fat_file_write(br_file_t *f, const void *buf, br_size_t n)
{
    uboot_fat_ino_t *node = (uboot_fat_ino_t *)br_file_fpriv(f);
    uboot_fat_vol_t *vol;
    char raw[UB_FAT_PATH_MAX];
    br_s64 off;
    loff_t act = 0;
    int rc;

    if ((node == BR_NULL) || (node->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);
    }
    /* BR_O_APPEND means "seek to the end **before every write**", so even a zero-length write seeks */
    if ((br_file_flags(f) & BR_O_APPEND) != 0u) {
        br_u32 sz = 0u;

        rc = uboot_fat_size_of(node, &sz);
        if (rc != 0) {
            return rc;
        }
        node->size = sz;
        br_file_set_offset(f, (br_s64)sz);
    }
    if (n == 0u) {
        return 0;
    }
    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(EINVAL);
    }

    vol = uboot_fat_vol_get(node->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_raw_path(node, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    /* upstream semantics: write size bytes at pos; **no holes allowed** (pos > current len ⇒ -EINVAL) */
    rc = file_fat_write_at(raw, off, (void *)(br_uintptr_t)buf, (loff_t)n, &act);
    if (rc < 0) {
        return rc;
    }
    br_file_set_offset(f, off + (br_s64)act);
    if ((br_u64)(off + (br_s64)act) > (br_u64)node->size) {
        node->size = (br_u32)(off + (br_s64)act);   /* keep the snapshot in step (truth is on media) */
    }
    return (br_s64)act;
}

static br_s64 uboot_fat_file_lseek(br_file_t *f, br_s64 off, int whence)
{
    uboot_fat_ino_t *node = (uboot_fat_ino_t *)br_file_fpriv(f);
    br_s64 base;
    br_s64 pos;
    int rc;

    if ((node == BR_NULL) || (node->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);
    }
    switch (whence) {
    case BR_SEEK_SET:
        base = 0;
        break;
    case BR_SEEK_CUR:
        base = br_file_offset(f);
        break;
    case BR_SEEK_END: {
        br_u32 sz = 0u;

        rc = uboot_fat_size_of(node, &sz);   /* read from media, not a possibly stale snapshot */
        if (rc != 0) {
            return rc;
        }
        node->size = sz;
        base = (br_s64)sz;
        break;
    }
    default:
        return BR_ERR(EINVAL);
    }
    if (base < 0) {
        return BR_ERR(EINVAL);
    }
    if (__builtin_add_overflow(base, off, &pos) || (pos < 0)) {
        return BR_ERR(EINVAL);
    }
    br_file_set_offset(f, pos);
    return pos;
}

/*
 * `fops->fsync` (= `fsync(fd)` in runtime/posix).
 *
 * ★ why it must exist (instead of BR_NULL as in early v1):
 *   `fsync` means "get the bytes I wrote onto the media", and this plugin's write path already
 *   commits data and dirents **call by call**; the only thing possibly left in memory is the **FAT
 *   cache** (`fsdata.fatbuf`, a 6-sector window, marked dirty by set_fatent_value). So fsync =
 *   ① write back the dirty FAT window ② let bdev flush. Leaving BR_NULL would hand consumers
 *   -ENOTSUP, which is not semantically "unsupported".
 *
 * Use `br_file_inode()` rather than `br_file_fpriv()` to get the node: a **dir handle**'s fpriv is
 * already taken by the dir iterator (fat_dir*), while an inode's fpriv always equals the node
 * itself (CA-2 first-field layout).
 */
static int uboot_fat_fsync(br_file_t *f)
{
    const br_inode_t *ino = br_file_inode(f);
    const uboot_fat_ino_t *node = (ino != BR_NULL) ? uboot_fat_node(ino) : BR_NULL;
    uboot_fat_vol_t *vol;

    if (node == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    vol = uboot_fat_vol_get(node->vol);
    return uboot_fat_flush_volume(vol);   /* **the same code** as fs_ops.sync */
}

static int uboot_fat_file_close(br_file_t *f)
{
    /* session state = the path in the inode (no second copy in fpriv) ⇒ nothing to give back.
     * vfs then frees the **transient inode** itself via free_inode. */
    (void)f;
    return 0;
}

/* ==================================================================== layer 3: dir side */

static int uboot_fat_dir_open(br_file_t *f, br_u32 flags)
{
    uboot_fat_ino_t *node = (uboot_fat_ino_t *)br_file_fpriv(f);
    uboot_fat_vol_t *vol;
    uboot_fat_dirstream_t *st;
    struct fs_dir_stream *ds = BR_NULL;
    char raw[UB_FAT_PATH_MAX];
    int rc;

    if ((node == BR_NULL) || (node->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    (void)flags;                     /* vfs already blocks "write-open a dir ⇒ -EISDIR" */
    vol = uboot_fat_vol_get(node->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_raw_path(node, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    rc = fat_opendir(raw, &ds);
    if (rc < 0) {
        return rc;
    }
    /* session state (iteration cursor) goes on the **file handle**'s fpriv, not on node identity */
    st = (uboot_fat_dirstream_t *)br_malloc(sizeof(*st));
    if (st == BR_NULL) {
        fat_closedir(ds);
        return BR_ERR(ENOMEM);
    }
    st->ds     = ds;
    st->cursor = 0u;
    br_file_set_fpriv(f, st);
    return 0;
}

static br_s64 uboot_fat_dir_read(br_file_t *f, void *buf, br_size_t n)
{
    uboot_fat_dirstream_t *st = (uboot_fat_dirstream_t *)br_file_fpriv(f);
    br_dirent_t *de = (br_dirent_t *)buf;
    struct fs_dirent *dent = BR_NULL;
    int rc;

    if ((st == BR_NULL) || (st->ds == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* a fixed-size record cannot be truncated (same rule as fs/tmpfs): too small ⇒ -EINVAL —
     * returning 0 would be read by br_readdir as "directory exhausted", which is a lie. */
    if ((buf == BR_NULL) || (n < (br_size_t)sizeof(br_dirent_t))) {
        return BR_ERR(EINVAL);
    }

    /* first two synthesized in place (see file header): FAT's root dir has no "."/".." */
    if (st->cursor == 0u) {
        st->cursor = 1u;
        rc = uboot_fat_fill_dirent(de, BR_INODE_DIR, ".");
        return (rc == 0) ? (br_s64)sizeof(br_dirent_t) : rc;
    }
    if (st->cursor == 1u) {
        st->cursor = 2u;
        rc = uboot_fat_fill_dirent(de, BR_INODE_DIR, "..");
        return (rc == 0) ? (br_s64)sizeof(br_dirent_t) : rc;
    }
    st->cursor++;

    rc = fat_readdir(st->ds, &dent);
    if (rc < 0) {
        return 0;                    /* upstream uses -ENOENT for "exhausted" ⇒ collapse to 0 */
    }
    if (dent == BR_NULL) {
        return 0;
    }
    rc = uboot_fat_fill_dirent(de,
                               (dent->type == FS_DT_DIR) ? BR_INODE_DIR : BR_INODE_FILE,
                               dent->name);
    if (rc != 0) {
        return rc;
    }
    return (br_s64)sizeof(br_dirent_t);
}

static int uboot_fat_dir_close(br_file_t *f)
{
    uboot_fat_dirstream_t *st = (uboot_fat_dirstream_t *)br_file_fpriv(f);

    if (st != BR_NULL) {
        if (st->ds != BR_NULL) {
            fat_closedir(st->ds);
        }
        br_free(st);
        br_file_set_fpriv(f, BR_NULL);   /* makes a second close a no-op */
    }
    return 0;
}

/* ==================================================================== layer 2: inode ops */

static br_inode_t *uboot_fat_lookup(br_inode_t *dir, const char *name)
{
    uboot_fat_ino_t *d;
    uboot_fat_ino_t *out = BR_NULL;
    char parent[UB_FAT_PATH_MAX];
    char child[UB_FAT_PATH_MAX];
    br_bool is_dir = BR_FALSE;
    br_u32 size = 0u;
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR) || (name == BR_NULL)) {
        return BR_NULL;
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_NULL;
    }

    /* "." / "..": resolved in place (the root's parent = the root, so POSIX "/.." == "/" holds) */
    if (uboot_fat_name_dot(name) == BR_TRUE) {
        const char *parent_path;

        if (uboot_fat_strcmp(name, ".") == 0) {
            return &d->head;
        }
        uboot_fat_parent_path(d->path, parent, sizeof(parent));
        parent_path = parent;
        out = uboot_fat_node_new(d->vol, BR_INODE_DIR, parent_path, 0u);
        return (out != BR_NULL) ? &out->head : BR_NULL;
    }

    /* an invalid name (empty / contains '/') is just "not found" during lookup; too long ⇒ this layer
     * has no error-code channel (returns BR_NULL, vfs yields -ENOENT). create/mkdir/readdir carry an
     * explicit errno for the over-bound **criterion**. */
    if (uboot_fat_name_ok(name) == BR_FALSE) {
        return BR_NULL;
    }
    /* depth bound: ≤ BR_NAME_MAX-1 components (pure defense; vfs's path buffer holds the truth) */
    if (uboot_fat_path_depth(d->path) >= (UB_FAT_NAME_MAX - 1u)) {
        return BR_NULL;
    }

    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return BR_NULL;
    }
    if (child[0] == '\0') {
        return BR_NULL;
    }

    {
        uboot_fat_ino_t probe;

        probe.vol  = d->vol;
        probe.type = BR_INODE_NONE;
        probe.size = 0u;
        uboot_fat_strcpy_bounded(probe.path, child, UB_FAT_PATH_MAX);
        rc = uboot_fat_kind_of(&probe, &is_dir);
        if (rc == BR_ERR(ENOENT)) {
            return BR_NULL;                 /* miss: BR_NULL (not an error code) */
        }
        if (rc != 0) {
            return BR_NULL;
        }
        if (is_dir == BR_FALSE) {
            rc = uboot_fat_size_of(&probe, &size);
            if (rc != 0) {
                size = 0u;                  /* report 0 if unavailable; getattr re-asks the media */
            }
        }
    }

    out = uboot_fat_node_new(d->vol,
                             (is_dir == BR_TRUE) ? BR_INODE_DIR : BR_INODE_FILE,
                             child, size);
    return (out != BR_NULL) ? &out->head : BR_NULL;
}

static int uboot_fat_create(br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out)
{
    uboot_fat_ino_t *d;
    uboot_fat_ino_t *n;
    char child[UB_FAT_PATH_MAX];
    br_bool is_dir = BR_FALSE;
    int rc;

    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *out = BR_NULL;
    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    /* the "too long" branch of the name rule gives -ENOSPC (same rule as fs/tmpfs: fixed dirents) */
    if (uboot_fat_name_dot(name) == BR_TRUE) {
        return BR_ERR(EINVAL);
    }
    if (uboot_fat_name_ok(name) == BR_FALSE) {
        return BR_ERR(ENOSPC);
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }

    {
        uboot_fat_ino_t probe;

        probe.vol  = d->vol;
        probe.type = BR_INODE_NONE;
        probe.size = 0u;
        uboot_fat_strcpy_bounded(probe.path, child, UB_FAT_PATH_MAX);
        rc = uboot_fat_kind_of(&probe, &is_dir);
        if (rc == 0) {
            /* already exists: O_EXCL ⇒ -EEXIST; else reuse (Linux rule; vfs already looked it up) */
            if ((flags & BR_O_EXCL) != 0u) {
                return BR_ERR(EEXIST);
            }
            if (is_dir == BR_TRUE) {
                return BR_ERR(EISDIR);
            }
            n = uboot_fat_node_new(d->vol, BR_INODE_FILE, child, 0u);
            if (n == BR_NULL) {
                return BR_ERR(ENOMEM);
            }
            rc = uboot_fat_size_of(n, &n->size);
            if (rc != 0) {
                br_free(n);
                return rc;
            }
            *out = &n->head;
            return 0;
        }
        if (rc != BR_ERR(ENOENT)) {
            return rc;
        }
    }

    /* actually create the file. Same shape as fs/tmpfs create: first the "admission" checks (can we
     * write), then the media. Write 0 bytes first (upstream file_fat_write_at's create_link path) to
     * create the dirent, so later writes take the "file exists" branch. */
    {
        uboot_fat_vol_t *vol = uboot_fat_vol_get(d->vol);
        char raw[UB_FAT_PATH_MAX];
        loff_t act = 0;

        if (vol == BR_NULL) {
            return BR_ERR(ENODEV);
        }
        rc = uboot_fat_vol_bind(vol);
        if (rc != 0) {
            return rc;
        }
        raw[0] = '/';
        memcpy(raw + 1, child, uboot_fat_strlen(child) + 1u);
        rc = file_fat_write_at(raw, 0, (void *)(br_uintptr_t)"", 0, &act);
        if (rc < 0) {
            return rc;                          /* media full / bad name ⇒ pass through as-is */
        }
    }

    n = uboot_fat_node_new(d->vol, BR_INODE_FILE, child, 0u);
    if (n == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    *out = &n->head;
    return 0;
}

static int uboot_fat_unlink(br_inode_t *dir, const char *name)
{
    uboot_fat_ino_t *d;
    uboot_fat_vol_t *vol;
    char child[UB_FAT_PATH_MAX];
    char raw[UB_FAT_PATH_MAX];
    br_bool is_dir = BR_FALSE;
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    if ((uboot_fat_name_dot(name) == BR_TRUE) || (uboot_fat_name_ok(name) == BR_FALSE)) {
        return BR_ERR(EINVAL);
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }
    {
        uboot_fat_ino_t probe;

        probe.vol  = d->vol;
        probe.type = BR_INODE_NONE;
        probe.size = 0u;
        uboot_fat_strcpy_bounded(probe.path, child, UB_FAT_PATH_MAX);
        rc = uboot_fat_kind_of(&probe, &is_dir);
        if (rc != 0) {
            return rc;                       /* -ENOENT passed through as-is */
        }
        if (is_dir == BR_TRUE) {
            return BR_ERR(EISDIR);        /* a directory needs rmdir */
        }
    }
    vol = uboot_fat_vol_get(d->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    raw[0] = '/';
    memcpy(raw + 1, child, uboot_fat_strlen(child) + 1u);
    rc = fat_unlink(raw);
    return rc;
}

static int uboot_fat_mkdir(br_inode_t *dir, const char *name)
{
    uboot_fat_ino_t *d;
    uboot_fat_vol_t *vol;
    char child[UB_FAT_PATH_MAX];
    char raw[UB_FAT_PATH_MAX];
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    if ((uboot_fat_name_dot(name) == BR_TRUE) || (uboot_fat_name_ok(name) == BR_FALSE)) {
        return BR_ERR(ENOSPC);
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }
    vol = uboot_fat_vol_get(d->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    /* upstream raw paths are "/a/b"; the mount root ⇒ "/" */
    raw[0] = '/';
    memcpy(raw + 1, child, uboot_fat_strlen(child) + 1u);
    /* upstream returns -EEXIST for "already exists" (see fat_mkdir), matching what vfs expects */
    return fat_mkdir(raw);
}

static int uboot_fat_rmdir(br_inode_t *dir, const char *name)
{
    uboot_fat_ino_t *d;
    uboot_fat_vol_t *vol;
    char child[UB_FAT_PATH_MAX];
    char raw[UB_FAT_PATH_MAX];
    br_bool is_dir = BR_FALSE;
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    if ((uboot_fat_name_dot(name) == BR_TRUE) || (uboot_fat_name_ok(name) == BR_FALSE)) {
        return BR_ERR(EINVAL);
    }
    d = uboot_fat_node(dir);
    if (d == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }
    {
        uboot_fat_ino_t probe;

        probe.vol  = d->vol;
        probe.type = BR_INODE_NONE;
        probe.size = 0u;
        uboot_fat_strcpy_bounded(probe.path, child, UB_FAT_PATH_MAX);
        rc = uboot_fat_kind_of(&probe, &is_dir);
        if (rc != 0) {
            return rc;
        }
        if (is_dir == BR_FALSE) {
            return BR_ERR(ENOTDIR);
        }
    }
    vol = uboot_fat_vol_get(d->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    raw[0] = '/';
    memcpy(raw + 1, child, uboot_fat_strlen(child) + 1u);
    return fat_unlink(raw);
}

static int uboot_fat_rename(br_inode_t *dir, const char *name,
                            br_inode_t *ndir, const char *nname)
{
    uboot_fat_ino_t *d;
    uboot_fat_ino_t *nd;
    uboot_fat_vol_t *vol;
    char child[UB_FAT_PATH_MAX];
    char nchild[UB_FAT_PATH_MAX];
    char old_raw[UB_FAT_PATH_MAX];
    char new_raw[UB_FAT_PATH_MAX];
    int rc;

    if ((dir == BR_NULL) || (ndir == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if ((dir->type != BR_INODE_DIR) || (ndir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTDIR);
    }
    /* source and target are on the same volume (cross-mount is stopped earlier by vfs) */
    if (dir->iops != &s_dir_iops) {
        return BR_ERR(EXDEV);
    }
    d  = uboot_fat_node(dir);
    nd = uboot_fat_node(ndir);
    if ((d == BR_NULL) || (nd == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if (d->vol != nd->vol) {
        return BR_ERR(EXDEV);
    }
    if ((uboot_fat_name_dot(name) == BR_TRUE) ||
        (uboot_fat_name_dot(nname) == BR_TRUE) ||
        (uboot_fat_name_ok(name) == BR_FALSE) ||
        (uboot_fat_name_ok(nname) == BR_FALSE)) {
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_join_path(d->path, name, child, sizeof(child));
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_join_path(nd->path, nname, nchild, sizeof(nchild));
    if (rc != 0) {
        return rc;
    }
    vol = uboot_fat_vol_get(d->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    old_raw[0] = '/';
    memcpy(old_raw + 1, child, uboot_fat_strlen(child) + 1u);
    new_raw[0] = '/';
    memcpy(new_raw + 1, nchild, uboot_fat_strlen(nchild) + 1u);
    /* upstream fat_rename already rejects "move a directory into its own subtree"
     * (check_path_prefix); this layer does not re-implement it (second implementation = second truth). */
    return fat_rename(old_raw, new_raw);
}

static int uboot_fat_getattr(br_inode_t *ino, br_stat_t *st)
{
    uboot_fat_ino_t *n;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if ((ino->type != BR_INODE_DIR) && (ino->type != BR_INODE_FILE)) {
        return BR_ERR(ENOTSUP);
    }
    n = uboot_fat_node(ino);
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    st->valid = BR_STAT_SIZE;      /* nlink not set: FAT has no hard links (contract allows omitting) */
    st->type  = ino->type;
    if (ino->type == BR_INODE_DIR) {
        st->size = 0u;             /* a dir's size has no contract here, use 0 (same as tmpfs) */
    } else {
        br_u32 sz = 0u;
        const int rc = uboot_fat_size_of(n, &sz);   /* ask the media each time (dirent size is truth) */

        if (rc != 0) {
            return rc;
        }
        n->size = sz;
        st->size = sz;
    }
    st->nlink = 0u;
    return 0;
}

static int uboot_fat_setattr(br_inode_t *ino, const br_stat_t *st)
{
    uboot_fat_ino_t *n;
    uboot_fat_vol_t *vol;
    char raw[UB_FAT_PATH_MAX];
    int rc;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* v1 honors only size (truncate). Unrecognized bits **must** give -ENOTSUP: faking success is
     * more dangerous than refusing. */
    if ((st->valid & ~(br_u32)BR_STAT_SIZE) != 0u) {
        return BR_ERR(ENOTSUP);
    }
    if ((st->valid & BR_STAT_SIZE) == 0u) {
        return 0;                  /* nothing was asked to change */
    }
    if (ino->type != BR_INODE_FILE) {
        return BR_ERR(EISDIR);
    }
    n = uboot_fat_node(ino);
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    vol = uboot_fat_vol_get(n->vol);
    if (vol == BR_NULL) {
        return BR_ERR(ENODEV);
    }
    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_raw_path(n, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    rc = uboot_fat_truncate(raw, st->size);
    if (rc == 0) {
        n->size = (br_u32)st->size;
    }
    return rc;
}

/* ==================================================================== layer 1: super */

/*
 * BPB check + **volume geometry validation**. Upstream get_fs_info only checks whether the
 * parameters are self-consistent; this file's mount must also answer "does this BPB match the
 * geometry of the device behind it" — if not, refuse to mount (fail-fast is intentional).
 * Check three things (all mechanically decidable from BPB and geometry):
 *   ① total sectors in the volume ≤ device sectors (else reads run past the device);
 *   ② data area start (can be negative under FAT12/16's "negative cluster" rule) + cluster count ≥ 0
 *      and inside the volume;
 *   ③ the cluster count falls in the **defining range** of the FAT type (12: < 4085; 16: 4085..65524;
 *      32: ≥ 65525) — this also validates determine_legacy_fat_bits' decision against the BPB.
 */
static int uboot_fat_mount_check(const uboot_fat_vol_t *vol, char *why, br_size_t cap)
{
    /* ★ the `vol->*` fields here are **observations parsed from the BPB at mount time**
     * (uboot_fat_vol_probe in fat_blk.c), not a second fsdata: every upstream API carries its own
     * fsdata, glue cannot see it and does not need it — the checks only need "type / cluster count /
     * data area inside the volume". */
    const uboot_fat_vol_t *fs = vol;
    const br_u64 n_sectors = vol->geom.n_sectors;
    br_u32 data_start;
    br_u32 total_clusters;
    br_u32 lo, hi;

    if (fs->clust_size == 0u) {
        uboot_fat_strcpy_bounded(why, "cluster size is zero", cap);
        return BR_ERR(EINVAL);
    }
    if ((br_u64)fs->total_sect > n_sectors) {
        uboot_fat_strcpy_bounded(why, "BPB total_sect > device sectors", cap);
        return BR_ERR(EINVAL);
    }

    if (fs->fatsize == 32) {
        /* FAT32: data_begin already "equals" the sector of the first cluster (cluster 2) */
        data_start = fs->data_begin + (fs->clust_size * 2u);
    } else {
        data_start = fs->rootdir_sect + fs->rootdir_size;
    }
    if (data_start >= fs->total_sect) {
        uboot_fat_strcpy_bounded(why, "data area starts past volume end", cap);
        return BR_ERR(EINVAL);
    }
    total_clusters = (fs->total_sect - data_start) / fs->clust_size;

    switch (fs->fatsize) {
    case 12:
        lo = 1u;
        hi = UB_FAT_MAX_FAT12;                  /* ≤ 0xFF4 (source in glue/fat_glue.h) */
        break;
    case 16:
        lo = UB_FAT_MAX_FAT12 + 1u;             /* 4085 */
        hi = 65524u;
        break;
    case 32:
        lo = 65525u;
        hi = 0x0ffffff0u;                       /* FAT32 cluster-number space */
        break;
    default:
        uboot_fat_strcpy_bounded(why, "unknown FAT type", cap);
        return BR_ERR(EINVAL);
    }
    if ((total_clusters < lo) || (total_clusters > hi)) {
        uboot_fat_strcpy_bounded(why, "cluster count inconsistent with FAT type", cap);
        return BR_ERR(EINVAL);
    }
    return 0;
}

static int uboot_fat_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    uboot_fat_fs_t *fs;
    uboot_fat_vol_t *vol;
    uboot_fat_ino_t *r;
    char why[64];
    br_u32 id = 0u;
    int rc;

    (void)fs_priv;      /* ★ ADR-0030: registration carries no mount body — each mount makes one */

    if ((cfg == BR_NULL) || (cfg->path == BR_NULL) || (cfg->dev == BR_NULL) ||
        (root == BR_NULL)) {
        br_log_error("uboot-fat: incomplete mount args (path=%s dev=%s) — "
                     "a media FS needs the APP to supply the source device (mount(source, target, \"fat\", …))",
                     (cfg != BR_NULL && cfg->path != BR_NULL) ? cfg->path : "(null)",
                     (cfg != BR_NULL && cfg->dev != BR_NULL) ? cfg->dev : "(null)");
        return BR_ERR(EINVAL);
    }
    *root = BR_NULL;

    /* a volume = the (source device, mount point) pair given by the APP's mount plan; claim a slot. */
    rc = uboot_fat_vol_claim(cfg->dev, cfg->path, &id);
    if (rc != 0) {
        br_log_error("uboot-fat: volume claim failed (dev=%s mnt=%s) rc=%d", cfg->dev, cfg->path, rc);
        return rc;
    }
    vol = uboot_fat_vol_get(id);
    if (vol == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /* each mount gets its own mount body (it holds the root inode; vfs's mount table keeps it) */
    fs = (uboot_fat_fs_t *)br_malloc(sizeof(*fs));
    if (fs == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    fs->vol  = vol;
    fs->root = BR_NULL;

    rc = uboot_fat_vol_bind(vol);
    if (rc != 0) {
        br_free(fs);
        return rc;
    }
    why[0] = '\0';
    rc = uboot_fat_mount_check(vol, why, sizeof(why));
    if (rc != 0) {
        br_log_error("uboot-fat: %s rejected: %s", cfg->path, why);
        uboot_fat_vol_unbind();
        br_free(fs);
        return rc;
    }

    /* root inode: br_malloc'd, held long-term by this plugin (vfs never calls free_inode on it) */
    r = uboot_fat_node_new(id, BR_INODE_DIR, "", 0u);
    if (r == BR_NULL) {
        uboot_fat_vol_unbind();
        br_free(fs);
        return BR_ERR(ENOMEM);
    }
    fs->root = &r->head;

    /* ★ ADR-0030: the mount body is **made per mount**, so register the internal view **here**
     *   (the query face for sync / vol_info). This table used to be preset in the init phase; now it
     *   is filled by mount facts. `cfg->path` is held by vfs's mount table (until shutdown), long
     *   enough. */
    uboot_fat_strcpy_bounded(s_slots[id].mnt, cfg->path, (br_size_t)sizeof(s_slots[id].mnt));
    s_slots[id].fs      = fs;
    s_slots[id].mounted = BR_TRUE;

    /* ★ the **shape of the ready line is an integration contract**: the `fat-test` gate in
     *   `tests/gates.toml` requires each volume to be named as `uboot-fat: <mount point> ready`
     *   (same as `uboot-erofs: /assets ready` in `adapter/uboot-erofs` and `uboot-mmc: eMMC ready`
     *   in `adapter/uboot-mmc`). The trailing parenthesis is a human diagnostic, not a criterion. */
    br_log_info("uboot-fat: %s ready (dev=%s, FAT%d, %u sectors x %u B, cluster %u sectors, %u clusters)",
                cfg->path, vol->dev, vol->fatsize, vol->total_sect,
                (unsigned)vol->sect_size, vol->clust_size,
                (vol->clust_size != 0u) ? (vol->total_sect / vol->clust_size) : 0u);
    *root = fs->root;
    return 0;
}

static void uboot_fat_free_inode(br_inode_t *ino)
{
    if (ino != BR_NULL) {
        br_free(ino);                        /* transient path wrapper: the maker frees it */
    }
}

/* volume-level sync (= the other entry of `br_file_ops.fsync`, sharing uboot_fat_flush_volume). */
static int uboot_fat_sync(void *fs_priv)
{
    uboot_fat_fs_t *fs = (uboot_fat_fs_t *)fs_priv;

    if (fs == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    return uboot_fat_flush_volume(fs->vol);
}

/* ==================================================================== internal views for self-tests */

/* internal view of volume-level sync (the TC-FAT-008 criterion; fs_ops.sync is exactly it) */
int uboot_fat_test_sync(const char *mnt)
{
    for (br_u32 i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
        if ((s_slots[i].mounted == BR_TRUE) && (s_slots[i].fs != BR_NULL) &&
            (uboot_fat_strcmp(s_slots[i].mnt, mnt) == 0)) {
            return uboot_fat_sync(s_slots[i].fs);
        }
    }
    return BR_ERR(ENOENT);
}

int uboot_fat_mount_vol_info(const char *mnt, uboot_fat_vol_info_t *out)
{
    br_u32 i;

    if ((mnt == BR_NULL) || (out == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    for (i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
        if ((s_slots[i].mounted == BR_TRUE) &&
            (uboot_fat_strcmp(s_slots[i].mnt, mnt) == 0)) {
            return uboot_fat_vol_info(i, out);
        }
    }
    return BR_ERR(ENOENT);
}

/* ==================================================================== lifecycle
 *
 * After ADR-0030 only one thing remains: **register the driver in the start phase** (`"fat"`).
 * Mounting is initiated by the **APP** — it calls POSIX `mount("emmcN", "/fatNN", "fat", …)` per its
 * own `[[mount]]` plan, so `uboot_fat_mount()` claims a volume and makes a mount body each time.
 * Ordering premise: block devices are registered by `adapter/uboot-mmc` in **its** start phase, and
 * the APP's start runs after all non-APP starts (plugin manager phase rule) ⇒ by then the devices
 * must be ready. EARLY/CORE return empty-handed: the early phase has no heap pool yet (br_mem_init
 * is in core.init), and mounting allocates the root inode.
 */
int uboot_fat_early_init(void);
int uboot_fat_init(void);
int uboot_fat_start(void);

int uboot_fat_early_init(void)
{
    /* EARLY phase: no heap pool yet (br_mem_init is in core.init); this file allocates nothing here. */
    return 0;
}

int uboot_fat_start(void)
{
    /* ★ ADR-0030: only **register the driver**, no mounting — which card and which mount point is
     *   the APP mount plan's call (each `mount("emmcN", "/fatNN", "fat", 0, NULL)` claims a volume).
     *   This used to mount volume by volume from the plugin's own three-row table ⇒ two truths for
     *   the "mount plan" (that debt was paid off by ADR-0030). */
    const int rc = br_fs_driver_register("fat", &s_fs_ops, BR_NULL);

    if (rc != 0) {
        br_log_error("uboot-fat: failed to register driver fat rc=%d", rc);
        return rc;
    }
    return 0;
}

int uboot_fat_init(void)
{
    /* CORE phase: nothing to do. Mount points and source devices now come from the **APP mount
     * plan** (ADR-0030), volume slots are claimed at mount time — no preset "three-volume table" here
     * (that was a second truth for the mount plan). */
    return 0;
}


