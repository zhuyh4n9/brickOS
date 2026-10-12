/*
 * brickOS prototype v0.2.0 — device file system (fs/devfs): projects the dev-core
 * registry into /dev nodes
 *
 * Design basis: `docs/7-storage/7-03-concrete-fs.md` §3 (devfs: "all devices enter
 * VFS management through devfs" — same shape as Linux devtmpfs / rCore DeviceFS) and
 * `docs/8-device/8-01-device.md` §2/§3 (D21/D23 open_file hook + cdev-core's generic
 * session adaptation).
 * Case-by-case rulings in `docs/decisions/0009-vfs-storage-stack.md`.
 *
 * ## Three hard boundaries (this plugin is entirely about holding them)
 *   (1) **project only, do not own devices**: a node = a **live enumeration** of the
 *       dev-core registry (registering puts it online); devfs has no "device table"
 *       and no lifecycle API — whether a device exists is decided by driver
 *       registration.
 *   (2) **knows no subclass shape** (design SD-13/D21): this file does **not
 *       include** `br_cdev.h`, nor does `br_cdev_ops` appear — opening a device only
 *       goes through the entry's `open_file` hook to get {fops, fpriv}. This is
 *       exactly the slot D21 left to devfs after revoking "vfs-core→dev-core".
 *   (3) **all namespace changes are -ENOTSUP** (design `7-03` §3: "unlink/mkdir →
 *       -ENOTSUP"): bringing devices up/down belongs to driver registration, not the
 *       file system — so those iops slots are **left empty** (NULL), and vfs-core
 *       translates them to -ENOTSUP (no need to write a "return unsupported" line
 *       here).
 *
 * ## inode shape (D23's "root inode = registry projection")
 *   root: one **static** directory inode (held by the mount table, never freed —
 *   `free_inode` special-cases it).
 *   device node: `lookup` **creates** a transient inode on the spot (`br_malloc`)
 *   carrying {fops, fpriv}; the session for opening it is built/destroyed by
 *   cdev-core's adapter layer, devfs only stuffs the pair into the inode.
 *   ⇒ a device node inode's `free_inode` is `br_free` (unlike tmpfs's "node is the
 *   inode").
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>

#include <br/dev/br_dev.h>
#include <br/vfs/br_vfs.h>

/* Lifecycle hook prototypes (name = symbol_prefix + phase; referenced by generated code; not in [[export]]) */
int devfs_early_init(void);
int devfs_init(void);
int devfs_start(void);

/* Mount path constant — **used only as the self-test fixture name**.
 * ★ Since ADR-0030 "where to mount" is decided by the **APP's `[[mount]]`** (devfs only
 *   registers the driver `"devfs"`); it used to be two sources of truth (a code
 *   constant + a manifest declaration, debt br-wa-fs-001), repaid once the generation
 *   pipeline landed. */
#define BR_DEVFS_MOUNT_PATH   "/dev"

/* ==================================================================== helpers (no libc) */

static br_bool devfs_str_eq(const char *a, const char *b)
{
    if (a == BR_NULL || b == BR_NULL) {
        return (a == b) ? BR_TRUE : BR_FALSE;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

static void devfs_str_copy(char *dst, const char *src, br_size_t cap)
{
    br_size_t i = 0u;
    for (; src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ==================================================================== inode shape */

/* Private tail after the common header of a device-node inode: devfs only needs
 * "which name it projects".
 * (fops/fpriv are already in the common header — vfs must be able to read them, see
 * the notes in br_vfs.h.) */
typedef struct devfs_node {
    br_inode_t head;
    char       name[BR_DEV_NAME_MAX];
} devfs_node_t;

static br_inode_t s_root;   /* root directory (held by the mount table; free_inode special-cases it) */

/* Forward declarations: the two iops tables are defined below, but `make_dev_node`
 * uses them first (their getattr is also defined later). C's single-pass declaration
 * rules require this line — less churn than flipping the whole inode section. */
static const br_inode_ops_t s_node_iops;
static const br_inode_ops_t s_root_iops;
static int devfs_getattr(br_inode_t *ino, br_stat_t *st);

/* Directory iteration cursor (one per open, stored in fpriv) */
typedef struct devfs_iter {
    br_u32 cursor;   /* next registry index to enumerate (0 = no device emitted yet) */
    br_u32 phase;    /* 0 = "." not emitted yet; 1 = ".." not emitted yet; 2 = enumerating devices */
} devfs_iter_t;

/* ==================================================================== directory surface */

static br_inode_t *make_dev_node(const char *name, const br_dev_class_entry_t *e)
{
    devfs_node_t *n = (devfs_node_t *)br_malloc((br_size_t)sizeof(devfs_node_t));
    if (n == BR_NULL) {
        return BR_NULL;
    }
    devfs_str_copy(n->name, name, (br_size_t)sizeof(n->name));

    n->head.type    = BR_INODE_FILE;
    /* ★ A device node's iops is **not** BR_NULL: `getattr/setattr` belong to the
     *   inode layer (in D23's four layers, inode level handles attributes, file level
     *   handles read/write), and every inode needs an attribute surface — Linux's
     *   `i_op`/`f_op` split is exactly this (a file inode's i_op fills only
     *   getattr/setattr).
     *   The consequence of BR_NULL was measured: `br_stat("/dev/uart0")` returns
     *   -ENOTSUP (TC-IO-001 red), because vfs's br_stat goes through
     *   `iops->getattr`.
     *   Namespace changes (create/unlink/mkdir/rename) are still **all empty** ⇒ vfs
     *   turns them into -ENOTSUP (design `7-03` §3: "unlink/mkdir → -ENOTSUP, device
     *   lifecycle belongs to driver registration"). */
    n->head.iops    = &s_node_iops;
    n->head.fops    = BR_NULL;          /* given by the hook (no file surface ⇒ NULL ⇒ open gets -ENOTSUP) */
    n->head.fpriv   = BR_NULL;

    /* ★ The only "device knowledge" entry: the entry's own hook.
     * A missing hook = this subclass provides no file surface (v1 raw flash/bdev) ⇒
     * the node still exists, just cannot be opened. */
    if (e->open_file != BR_NULL) {
        const br_file_ops_t *fops = BR_NULL;
        void                *fpriv = BR_NULL;
        if (e->open_file(e->dev_priv, &fops, &fpriv) != BR_OK) {
            fops  = BR_NULL;            /* hook error ⇒ degrade to "visible but not openable", do not invent a file surface */
            fpriv = BR_NULL;
        }
        n->head.fops  = fops;
        n->head.fpriv = fpriv;
    }
    return &n->head;
}

static br_inode_t *devfs_lookup(br_inode_t *dir, const char *name)
{
    (void)dir;      /* devfs has a single level: any dir is the root */

    if (name == BR_NULL) {
        return BR_NULL;
    }
    if (devfs_str_eq(name, ".") == BR_TRUE || devfs_str_eq(name, "..") == BR_TRUE) {
        /* The root's parent is still the root (the device tree is not mounted
         * elsewhere). Return the root pointer itself — it is **non-transient**,
         * `free_inode` special-cases it, so callers doing "whoever takes it releases
         * it" will still not free it. */
        return &s_root;
    }

    const br_dev_class_entry_t *e = br_dev_lookup(name);
    if (e == BR_NULL) {
        return BR_NULL;     /* unregistered ⇒ miss (vfs turns it into -ENOENT) */
    }
    return make_dev_node(name, e);
}

static int devfs_getattr(br_inode_t *ino, br_stat_t *st)
{
    if (ino == BR_NULL || st == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    st->valid = BR_STAT_SIZE;
    st->type  = ino->type;
    st->size  = 0u;         /* a device node has no "byte count" (size semantics belong to the session, not the node) */
    return BR_OK;
}

/* Device-node inode iops: **getattr only** (attribute surface); all namespace changes
 * empty.
 * Keeping it separate from the root iops is deliberate — the two objects have
 * different **capability surfaces**, and merging them into one table would make
 * questions like "can you mkdir on a device node" unanswerable from the code. */
static const br_inode_ops_t s_node_iops = {
    .lookup  = BR_NULL,
    .create  = BR_NULL,
    .unlink  = BR_NULL,
    .mkdir   = BR_NULL,
    .rmdir   = BR_NULL,
    .rename  = BR_NULL,
    .getattr = devfs_getattr,
    .setattr = BR_NULL,     /* device-node attributes are immutable (size semantics belong to the session, not the node) */
};

/* Root inode iops: **lookup + getattr only** — mkdir/unlink/rmdir/rename/create all
 * left empty ⇒ vfs returns -ENOTSUP (design `7-03` §3, verbatim: "unlink/mkdir →
 * -ENOTSUP, device lifecycle belongs to driver registration"). */
static const br_inode_ops_t s_root_iops = {
    .lookup  = devfs_lookup,
    .create  = BR_NULL,
    .unlink  = BR_NULL,
    .mkdir   = BR_NULL,
    .rmdir   = BR_NULL,
    .rename  = BR_NULL,
    .getattr = devfs_getattr,
    .setattr = BR_NULL,
};

/* ---- the root directory's "file surface" = directory entry stream (design `7-01` §2: the file surface of a directory inode) ---- */

static int devfs_dir_open(br_file_t *f, br_u32 flags)
{
    (void)flags;
    devfs_iter_t *it = (devfs_iter_t *)br_malloc((br_size_t)sizeof(devfs_iter_t));
    if (it == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    it->cursor = 0u;
    it->phase  = 0u;
    br_file_set_fpriv(f, it);
    return BR_OK;
}

static br_s64 devfs_dir_read(br_file_t *f, void *buf, br_size_t n)
{
    if (buf == BR_NULL || n < (br_size_t)sizeof(br_dirent_t)) {
        return BR_ERR(EINVAL);
    }
    devfs_iter_t *it = (devfs_iter_t *)br_file_fpriv(f);
    if (it == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    br_dirent_t *de = (br_dirent_t *)buf;
    de->name_len = 0u;

    if (it->phase == 0u) {
        it->phase = 1u;
        de->type = BR_INODE_DIR;
        devfs_str_copy(de->name, ".", (br_size_t)sizeof(de->name));
        de->name_len = 1u;
        return (br_s64)sizeof(*de);
    }
    if (it->phase == 1u) {
        it->phase = 2u;
        de->type = BR_INODE_DIR;
        devfs_str_copy(de->name, "..", (br_size_t)sizeof(de->name));
        de->name_len = 2u;
        return (br_s64)sizeof(*de);
    }

    /* Device enumeration: **query the registry on every call** (live projection,
     * design `7-03` §3).
     * v1 has no unregister ⇒ the table only grows ⇒ an index cursor neither misses
     * nor repeats entries; if unregistration appears later, this must change to
     * "resume by name" (the registry would then need a stable iteration key).
     * (This is not yet in ADR-0009 §5's leftover list — it is companion work for
     * "when the device unregister API lands".) */
    const br_u32 total = br_dev_count();
    while (it->cursor < total) {
        const char *name = br_dev_name_at(it->cursor);
        it->cursor++;
        if (name == BR_NULL) {
            continue;
        }
        de->type = BR_INODE_FILE;
        devfs_str_copy(de->name, name, (br_size_t)sizeof(de->name));
        br_size_t len = 0u;
        while (de->name[len] != '\0') {
            len++;
        }
        de->name_len = (br_u32)len;
        return (br_s64)sizeof(*de);
    }
    return 0;       /* exhausted */
}

static int devfs_dir_close(br_file_t *f)
{
    devfs_iter_t *it = (devfs_iter_t *)br_file_fpriv(f);
    if (it != BR_NULL) {
        br_free(it);
        br_file_set_fpriv(f, BR_NULL);
    }
    return BR_OK;
}

static const br_file_ops_t s_root_fops = {
    .open        = devfs_dir_open,
    .read        = devfs_dir_read,
    .write       = BR_NULL,     /* the device tree is read-only (devices come online via driver registration) */
    .lseek       = BR_NULL,     /* the directory entry stream does not support seeking */
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = devfs_dir_close,
};

/* ==================================================================== layer 1: super */

static int devfs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    (void)fs_priv;
    if (cfg == BR_NULL || root == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (devfs_str_eq(cfg->path, BR_DEVFS_MOUNT_PATH) == BR_FALSE) {
        /* Mount point disagrees with the code constant ⇒ refuse the mount. Why not
         * "follow cfg": that would let devfs's node prefix (which it does not care
         * about) drift from /dev in cases/docs; the v1 mount point is a compile-time
         * fact, a mismatch is a wiring error (this will disappear once the generation
         * pipeline lands). */
        br_log_error("devfs: mount point %s != expected %s", cfg->path, BR_DEVFS_MOUNT_PATH);
        return BR_ERR(EINVAL);
    }

    s_root.type  = BR_INODE_DIR;
    s_root.iops  = &s_root_iops;
    s_root.fops  = &s_root_fops;
    s_root.fpriv = BR_NULL;
    *root = &s_root;
    return BR_OK;
}

static void devfs_free_inode(br_inode_t *ino)
{
    if (ino == BR_NULL || ino == &s_root) {
        return;     /* the root is a static object (held by the mount table); lookup("."/"..") also hands it out */
    }
    br_free(ino);
}

static const br_fs_ops_t s_fs_ops = {
    .mount      = devfs_mount,
    .free_inode = devfs_free_inode,
    .unmount    = BR_NULL,      /* v2/v3 (runtime unmount is O-S3) */
    .sync       = BR_NULL,      /* no persistent state, there is no "write to media" */
};

/* ==================================================================== lifecycle */

int devfs_early_init(void)
{
    return 0;       /* EARLY: no threads, no heap; mounting is at CORE (by then the heap is ready) */
}

/*
 * CORE phase: register devfs as a **driver** (does not mount).
 *
 * ★ ADR-0030: **mounting is decided by the APP**. This file only says "my name is
 *   devfs"; the APP mounts it at /dev with the POSIX call
 *   `mount(NULL, "/dev", "devfs", 0, NULL)`.
 *   The ordering prerequisite also moves to the APP: "/" must be mounted first
 *   (tmpfs), otherwise the /dev mount point has no parent FS to attach to —
 *   `br_vfs_mount` returns -ENODEV (the automatic mkdir of the mount point is done
 *   by vfs-core).
 */
int devfs_init(void)
{
    const int rc = br_fs_driver_register("devfs", &s_fs_ops, BR_NULL);
    if (rc != BR_OK) {
        br_log_error("devfs: register driver failed rc=%d", rc);
        return rc;      /* fail-fast halt is performed by the plugin manager (ruling G6) */
    }
    return 0;
}

/* START phase: after all device registration is done, report "how many devices are visible" (boot evidence, not a criterion). */
int devfs_start(void)
{
    br_log_info("devfs: %u node(s) in %s (registry is live, not a snapshot)",
                br_dev_count(), BR_DEVFS_MOUNT_PATH);
    return 0;
}
