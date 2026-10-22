/*
 * fs/devfs — device file system: projects the dev-core registry into /dev nodes
 * (a live enumeration; driver registration decides whether a device exists).
 *
 * Knows no device subclass shape: this file does not include `br_cdev.h` and never
 * names `br_cdev_ops` — it gets {fops, fpriv} from the entry's `open_file` hook.
 * All namespace changes (create/unlink/mkdir/rename) are BR_NULL, which vfs turns
 * into -ENOTSUP: bringing devices up/down is driver registration's job.
 * The root is one static directory inode held by the mount table (free_inode
 * special-cases it); a device-node inode is transient (br_malloc in lookup, br_free
 * in free_inode) and carries only the projected name.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>
#include <br/core/br_types.h>

#include <br/dev/br_dev.h>
#include <br/vfs/br_vfs.h>

/* Lifecycle hook prototypes: name = symbol_prefix + phase; referenced by generated
 * code; not in [[export]] */
int devfs_early_init(void);
int devfs_init(void);
int devfs_start(void);

/* Mount path constant — used only as the self-test fixture name: the mount point comes
 * from the APP's `[[mount]]` (devfs only registers the driver `"devfs"`). */
#define BR_DEVFS_MOUNT_PATH   "/dev"

/* ---- helpers (no libc) ---- */

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

/* ---- inode shape ---- */

/* Private tail after the common inode header: devfs only needs the projected name.
 * fops/fpriv stay in the common header (vfs reads them there). */
typedef struct devfs_node {
    br_inode_t head;
    char       name[BR_DEV_NAME_MAX];
} devfs_node_t;

static br_inode_t s_root;   /* root dir (held by the mount table; free_inode special-cases it) */

/* Forward declarations: make_dev_node uses the two iops tables and getattr, which are
 * defined further down. */
static const br_inode_ops_t s_node_iops;
static const br_inode_ops_t s_root_iops;
static int devfs_getattr(br_inode_t *ino, br_stat_t *st);

/* Directory iteration cursor (one per open, stored in fpriv) */
typedef struct devfs_iter {
    br_u32 cursor;   /* next registry index to enumerate (0 = no device emitted yet) */
    br_u32 phase;    /* 0 = "." pending; 1 = ".." pending; 2 = enumerating devices */
} devfs_iter_t;

/* ---- directory surface ---- */

static br_inode_t *make_dev_node(const char *name, const br_dev_class_entry_t *e)
{
    devfs_node_t *n = (devfs_node_t *)br_malloc((br_size_t)sizeof(devfs_node_t));
    if (n == BR_NULL) {
        return BR_NULL;
    }
    devfs_str_copy(n->name, name, (br_size_t)sizeof(n->name));

    n->head.type    = BR_INODE_FILE;
    /* A device node's iops is not BR_NULL: getattr/setattr belong to the inode layer,
     * and BR_NULL made br_stat("/dev/uart0") return -ENOTSUP (TC-IO-001 red).
     * Namespace changes stay BR_NULL -> -ENOTSUP (device lifecycle is driver registration). */
    n->head.iops    = &s_node_iops;
    n->head.fops    = BR_NULL;          /* given by the hook (NULL ⇒ open gets -ENOTSUP) */
    n->head.fpriv   = BR_NULL;

    /* The only device-knowledge entry: the entry's own hook. A missing hook means no
     * file surface (v1 raw flash/bdev): the node exists but cannot be opened. */
    if (e->open_file != BR_NULL) {
        const br_file_ops_t *fops = BR_NULL;
        void                *fpriv = BR_NULL;
        if (e->open_file(e->dev_priv, &fops, &fpriv) != BR_OK) {
            fops  = BR_NULL;            /* hook error ⇒ "visible but not openable" */
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
        /* Root's parent is the root: return the static root itself. It is non-transient
         * and free_inode special-cases it, so a caller that "releases" it does not free it. */
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
    st->size  = 0u;         /* a device node has no byte count (size belongs to the session) */
    return BR_OK;
}

/* Device-node iops: getattr only; all namespace slots empty. Kept separate from the
 * root table on purpose — the two objects have different capability surfaces. */
static const br_inode_ops_t s_node_iops = {
    .lookup  = BR_NULL,
    .create  = BR_NULL,
    .unlink  = BR_NULL,
    .mkdir   = BR_NULL,
    .rmdir   = BR_NULL,
    .rename  = BR_NULL,
    .getattr = devfs_getattr,
    .setattr = BR_NULL,     /* device-node attributes are immutable (size belongs to the session) */
};

/* Root inode iops: lookup + getattr only. mkdir/unlink/rmdir/rename/create stay empty
 * -> -ENOTSUP (device lifecycle belongs to driver registration). */
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

/* ---- root directory file surface: directory entry stream ---- */

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

    /* Enumerate the registry live on every call. v1 has no unregister, so the table
     * only grows and an index cursor neither misses nor repeats entries; with
     * unregistration this must resume by name (the registry would need a stable key). */
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
    .write       = BR_NULL,     /* read-only: devices come online via driver registration */
    .lseek       = BR_NULL,     /* the directory entry stream is not seekable */
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = devfs_dir_close,
};

/* ---- layer 1: super ---- */

static int devfs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    (void)fs_priv;
    if (cfg == BR_NULL || root == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (devfs_str_eq(cfg->path, BR_DEVFS_MOUNT_PATH) == BR_FALSE) {
        /* Mount point must match the constant: the v1 mount point is a compile-time
         * fact (devfs does not care about the path), so a mismatch is a wiring error. */
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
        return;     /* root is static (mount table owns it); lookup("."/"..") returns it too */
    }
    br_free(ino);
}

static const br_fs_ops_t s_fs_ops = {
    .mount      = devfs_mount,
    .free_inode = devfs_free_inode,
    .unmount    = BR_NULL,      /* v2/v3 (runtime unmount) */
    .sync       = BR_NULL,      /* no persistent state to write to media */
};

/* ---- lifecycle ---- */

int devfs_early_init(void)
{
    return 0;       /* EARLY: no threads, no heap; mounting happens at CORE */
}

/*
 * CORE phase: register devfs as a driver (does not mount; the APP's `[[mount]]` does).
 * "/" must be mounted first (tmpfs), otherwise the /dev mount point has no parent FS
 * and `br_vfs_mount` returns -ENODEV (vfs auto-mkdirs the mount point).
 */
int devfs_init(void)
{
    const int rc = br_fs_driver_register("devfs", &s_fs_ops, BR_NULL);
    if (rc != BR_OK) {
        br_log_error("devfs: register driver failed rc=%d", rc);
        return rc;      /* fail-fast halt is performed by the plugin manager */
    }
    return 0;
}

/* START phase: report the visible device count (boot evidence, not a criterion). */
int devfs_start(void)
{
    br_log_info("devfs: %u node(s) in %s (registry is live, not a snapshot)",
                br_dev_count(), BR_DEVFS_MOUNT_PATH);
    return 0;
}
