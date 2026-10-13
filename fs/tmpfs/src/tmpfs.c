/*
 * fs/tmpfs — brickOS RAM rootfs: nodes and file data come from the core heap, with full
 * file semantics (read/write/lseek/truncate/unlink/rename/mkdir/stat) and the pre-created
 * /dev /data /tmp directories. Losing data on power-off is a role split, not a defect:
 * persistence is littlefs/EROFS.
 *
 * Nodes are br_malloc'd persistent objects on the parent's child list, so lookup() returns
 * existing nodes and allocates nothing, and free_inode must be a no-op (vfs passes only
 * transient inodes; a stray br_free would tear down the live tree). Node identity rides in
 * head.fpriv, which vfs hands to br_file_t on open.
 *
 * Quotas (128 KiB data / 48 nodes) are hard-coded static upper bounds, safe rather than
 * true, pending derivation from the manifest budget.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

/* Lifecycle hooks (EARLY / CORE / START): name = symbol_prefix + phase, referenced by the
 * generated build/gen/fs/tmpfs/plugin_desc.c. This plugin exports no API, so every other
 * function is static; these prototypes satisfy -Wmissing-prototypes. */
int tmpfs_early_init(void);
int tmpfs_init(void);
int tmpfs_start(void);

/* ---- quotas and name rules (static bounds; see the file header) ---- */

/* Upper bound on the sum of all file capacities (accounting basis = cap, see s_fs). */
#define TMPFS_DATA_MAX   (128u * 1024u)

/* Node count bound (root + three pre-created dirs + test margin); 48 × ~104 B ≈ 5 KiB. */
#define TMPFS_NODE_MAX   48u

/* Minimum capacity on first write: neither a block for a 1-byte write nor +1-byte reallocs
 * (that would turn sequential append, tmpfs's typical write, into O(n²)). */
#define TMPFS_MIN_CAP    64u

/* Mount path constant — only a self-test fixture name (e.g. TC-VFS's "the mount table has
 * a '/'"). Mounting is the APP's `[[mount]]`, so tmpfs_mount() accepts any path. */
#define TMPFS_MOUNT_PATH "/"

/* cap is br_u32: quota and offset arithmetic stay under 2 GiB, so no wraparound. */
_Static_assert(TMPFS_DATA_MAX <= 0x7fffffffu, "quota must keep cap (br_u32) and offset arithmetic safe");

/* ---- node model (common head + private tail) ---- */

typedef struct tmpfs_node tmpfs_node_t;
typedef struct tmpfs_body tmpfs_body_t;

/* Data body: file content is shared by names, not owned by a name. refs > 1 means several
 * nodes share it (a hard link); refs == 0 returns the data buffer and its quota. FILE and
 * SYMLINK always have a body, a directory never does, so `n->body->…` needs no null check. */
struct tmpfs_body {
    br_u8  *data;    /* allocated bytes = cap */
    br_u64  size;    /* file bytes / symlink target length */
    br_u32  cap;     /* data capacity (bytes) — also the quota accounting basis */
    br_u32  refs;    /* nodes referencing it (= st_nlink) */
};

/* `head` must be first: vfs knows only br_inode_t* and uses it as the node directly.
 * Recovering the node goes through head.fpriv (see tmpfs_ino_node), the contract carrier
 * already handed to the file side. */
struct tmpfs_node {
    br_inode_t    head;                 /* common head (type / iops / fops / fpriv) */
    tmpfs_node_t *parent;               /* parent dir; root self-refers (root->parent == root) */
    tmpfs_node_t *child;                /* child list head (dirs); files always BR_NULL */
    tmpfs_node_t *next;                 /* sibling single list */
    tmpfs_body_t *body;                 /* data body (file/symlink); dir = BR_NULL */
    char          name[BR_NAME_MAX];    /* dentry name (root's is "/", diagnostics only) */
};

/* Size budget for the private tail (not an optimization goal, but proof that 48 nodes fit) */
_Static_assert(sizeof(tmpfs_node_t) <= 128u, "a node must stay within 128 B");

/* Directory iterator: allocated by dir open(), freed by close(), stored on the file handle's
 * fpriv (never head.fpriv — that is node identity). It only borrows the node: rmdir
 * mid-iteration is a use-after-free, and v1 has no refcounting, so that is caller misuse. */
typedef struct {
    tmpfs_node_t *dir;      /* directory being enumerated (borrowed, not owned) */
    br_u32        cursor;   /* 0 = ".", 1 = "..", ≥2 = the (cursor-2)th child */
} tmpfs_dir_iter_t;

/* Accounting block, file-scope static because inode/file ops get no fs_priv (they see only
 * nodes); tmpfs_mount checks the fs_priv it receives is this one. Basis = Σ body->cap
 * (capacity), not Σ size: cap is what occupies the heap, and the sum of file lengths may be
 * far below data_used. truncate and unlink both return cap (see tmpfs_file_release). */
typedef struct {
    br_u32 nodes_used;   /* allocated nodes (including root) */
    br_u32 data_used;    /* sum of allocated capacities; bound TMPFS_DATA_MAX */
} tmpfs_fs_t;

static tmpfs_fs_t s_fs;

/* ---- forward declarations (names referenced by the ops tables above) ---- */

static br_inode_t *tmpfs_lookup (br_inode_t *dir, const char *name);
static int  tmpfs_create (br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out);
static int  tmpfs_unlink (br_inode_t *dir, const char *name);
static int  tmpfs_mkdir  (br_inode_t *dir, const char *name);
static int  tmpfs_rmdir  (br_inode_t *dir, const char *name);
static int  tmpfs_rename (br_inode_t *dir, const char *name, br_inode_t *ndir, const char *nname);
static int  tmpfs_getattr(br_inode_t *ino, br_stat_t *st);
static int  tmpfs_setattr(br_inode_t *ino, const br_stat_t *st);
static int  tmpfs_symlink (br_inode_t *dir, const char *name, const char *target);
static int  tmpfs_readlink(br_inode_t *ino, char *buf, br_size_t cap);
static int  tmpfs_link    (br_inode_t *dir, const char *name, br_inode_t *target);

static int   tmpfs_file_open (br_file_t *f, br_u32 flags);
static br_s64 tmpfs_file_read (br_file_t *f, void *buf, br_size_t n);
static br_s64 tmpfs_file_write(br_file_t *f, const void *buf, br_size_t n);
static br_s64 tmpfs_file_lseek(br_file_t *f, br_s64 off, int whence);
static int   tmpfs_file_close(br_file_t *f);

static int   tmpfs_dir_open (br_file_t *f, br_u32 flags);
static br_s64 tmpfs_dir_read (br_file_t *f, void *buf, br_size_t n);
static int   tmpfs_dir_close(br_file_t *f);

static int  tmpfs_mount     (void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
static void tmpfs_free_inode(br_inode_t *ino);

/* ---- ops tables (layers 1 / 2 / 3) ---- */

/* Layer 2: inode ops. Both node kinds share one table — vfs dispatches inode ops only for
 * directories (walks and namespace changes are "parent dir + name"), so the type guards stay
 * in one place; the only cost is a non-null pointer on file nodes (≤48). */
static const br_inode_ops_t s_iops = {
    .lookup  = tmpfs_lookup,
    .create  = tmpfs_create,
    .unlink  = tmpfs_unlink,
    .mkdir   = tmpfs_mkdir,
    .rmdir   = tmpfs_rmdir,
    .rename  = tmpfs_rename,
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
    /* additions at the table tail (existing named initializers unaffected) */
    .symlink  = tmpfs_symlink,
    .readlink = tmpfs_readlink,
    .link     = tmpfs_link,
};

/* Layer 3: file side. Unsupported slots are explicitly BR_NULL -> -ENOTSUP (not "forgotten"). */
static const br_file_ops_t s_file_fops = {
    .open        = tmpfs_file_open,
    .read        = tmpfs_file_read,
    .write       = tmpfs_file_write,
    .lseek       = tmpfs_file_lseek,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,   /* data lives only in the heap; nothing to persist */
    .poll        = BR_NULL,   /* in-memory files are always ready; waiting is v2 */
    .poll_attach = BR_NULL,   /* reserved for v2 (wait-queue); always NULL in v1 */
    .close       = tmpfs_file_close,
};

/* Directories have a file side too (read = enumerate entries), which keeps opendir/readdir
 * generic in vfs-core; write/seek are meaningless for a dir and stay empty. */
static const br_file_ops_t s_dir_fops = {
    .open        = tmpfs_dir_open,
    .read        = tmpfs_dir_read,
    .write       = BR_NULL,
    .lseek       = BR_NULL,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = tmpfs_dir_close,
};

/* Layer 1: super. unmount/sync are signature-first slots (runtime mount is v3); tmpfs has
 * no volume to sync. */
static const br_fs_ops_t s_fs_ops = {
    .mount      = tmpfs_mount,
    .free_inode = tmpfs_free_inode,
    .unmount    = BR_NULL,
    .sync       = BR_NULL,
};

/* ---- small helpers (freestanding: no <string.h>) ---- */

static int tmpfs_str_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b)) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

static br_u32 tmpfs_str_len(const char *s)
{
    br_u32 n = 0u;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* Bounded copy; cap includes the trailing '\0'. Callers are pre-checked by tmpfs_name_ok,
 * so truncation is defense only. */
static void tmpfs_str_copy(char *dst, const char *src, br_u32 cap)
{
    br_u32 i = 0u;

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

/* Block copy/zero via compiler builtins, no <string.h>: core/c_core/src/string.c provides
 * memcpy/memset, and writing our own would hit the "lowered into memcpy" recursion trap. */
static void tmpfs_copy(void *dst, const void *src, br_size_t n)
{
    (void)__builtin_memcpy(dst, src, n);
}

static void tmpfs_zero(void *dst, br_size_t n)
{
    (void)__builtin_memset(dst, 0, n);
}

/* Name rules (single enforcement point for create/mkdir/rename): non-empty, no '/', not
 * "." / "..", length ≤ BR_NAME_MAX-1. Length is scanned byte-by-byte up to NUL or
 * BR_NAME_MAX, so an unterminated caller string is read for at most BR_NAME_MAX bytes.
 * '.'/'..' are rejected only for mutating names; lookup resolves them inline, so "/." and
 * "/.." remain valid walk paths. */
static br_bool tmpfs_name_ok(const char *name)
{
    br_u32 i;

    if (name == BR_NULL) {
        return BR_FALSE;
    }
    if (name[0] == '\0') {
        return BR_FALSE;
    }
    if ((name[0] == '.') &&
        ((name[1] == '\0') || ((name[1] == '.') && (name[2] == '\0')))) {
        return BR_FALSE;
    }
    for (i = 0u; i < BR_NAME_MAX; i++) {
        if (name[i] == '\0') {
            return BR_TRUE;                  /* length = i ≤ BR_NAME_MAX-1 */
        }
        if (name[i] == '/') {
            return BR_FALSE;
        }
    }
    return BR_FALSE;                         /* window scanned, no NUL ⇒ too long */
}

/* Node identity: head.fpriv (set to self at creation), not br_inode_priv() — fpriv is the
 * contract carrier vfs hands to the file side. fops->open() overwrites the br_file_t's
 * fpriv (dir iterator), not this one. */
static tmpfs_node_t *tmpfs_ino_node(const br_inode_t *ino)
{
    return (tmpfs_node_t *)ino->fpriv;
}

/* File side: the session's fpriv is the node (file open keeps it; dir open swaps in an iterator) */
static tmpfs_node_t *tmpfs_session_node(br_file_t *f)
{
    return (tmpfs_node_t *)br_file_fpriv(f);
}

/* Type guard for file-side slots: vfs dispatches by node type, but a corrupted inode gets
 * -EISDIR rather than a crash inside a dereference. */
static int tmpfs_require_file(const tmpfs_node_t *n)
{
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    return (n->head.type == BR_INODE_FILE) ? BR_OK : BR_ERR(EISDIR);
}

/* Child list find/unlink. Linear is fine: at most 48 entries, no hashing needed. */
static tmpfs_node_t *tmpfs_child_find(tmpfs_node_t *dir, const char *name)
{
    tmpfs_node_t *c;

    for (c = dir->child; c != BR_NULL; c = c->next) {
        if (tmpfs_str_eq(c->name, name)) {
            return c;
        }
    }
    return BR_NULL;
}

static void tmpfs_child_unlink(tmpfs_node_t *dir, tmpfs_node_t *victim)
{
    tmpfs_node_t **pp;

    for (pp = &dir->child; *pp != BR_NULL; pp = &(*pp)->next) {
        if (*pp == victim) {
            *pp = victim->next;
            victim->next = BR_NULL;
            return;
        }
    }
}

/* Initialize a node (no accounting: the caller does that). All nodes share s_iops; only the
 * file side differs by type. `body`: file/symlink must be non-null, a directory always
 * BR_NULL. A symlink's `fops` is BR_NULL — a link is "visible but not openable"; vfs stops
 * it at expansion or at the O_NOFOLLOW layer (⇒ -ELOOP) before the file side. */
static void tmpfs_node_init(tmpfs_node_t *n, br_u32 type, tmpfs_node_t *parent,
                            const char *name, tmpfs_body_t *body)
{
    n->head.type  = type;
    n->head.iops  = &s_iops;
    if (type == BR_INODE_DIR) {
        n->head.fops = &s_dir_fops;
    } else if (type == BR_INODE_SYMLINK) {
        n->head.fops = BR_NULL;
    } else {
        n->head.fops = &s_file_fops;
    }
    n->head.fpriv = n;                    /* node identity: vfs hands it to br_file_t on open */
    n->parent     = parent;
    n->child      = BR_NULL;
    n->next       = BR_NULL;
    n->body       = body;
    tmpfs_str_copy(n->name, name, BR_NAME_MAX);
}

static br_bool tmpfs_node_room(void)
{
    return (s_fs.nodes_used < TMPFS_NODE_MAX) ? BR_TRUE : BR_FALSE;
}

/* ---- data body: create / reference / unreference ---- */

/* Create an empty body (refs = 1); capacity grows on demand (see tmpfs_file_reserve). */
static int tmpfs_body_new(tmpfs_body_t **out)
{
    tmpfs_body_t *b = (tmpfs_body_t *)br_malloc(sizeof *b);

    if (b == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    b->data = BR_NULL;
    b->size = 0u;
    b->cap  = 0u;
    b->refs = 1u;
    *out = b;
    return 0;
}

/* Drop one reference; at zero ⇒ return the data buffer and its quota to the heap. */
static void tmpfs_body_unref(tmpfs_body_t *b)
{
    if (b == BR_NULL) {
        return;
    }
    if (b->refs > 1u) {
        b->refs--;
        return;
    }
    if (b->data != BR_NULL) {
        br_free(b->data);
        b->data = BR_NULL;
    }
    s_fs.data_used -= b->cap;             /* quota returned (accounting basis = cap) */
    b->cap = 0u;
    br_free(b);
}

/* Create a node and link it into dir (shared by create/mkdir/symlink/link: name checks,
 * quota and linking in one place). `body` ownership moves to the node on success. */
static int tmpfs_node_add(tmpfs_node_t *dir, const char *name, br_u32 type,
                          tmpfs_body_t *body, tmpfs_node_t **out)
{
    tmpfs_node_t *n;

    if (tmpfs_node_room() == BR_FALSE) {
        return BR_ERR(ENOSPC);         /* node quota exhausted */
    }
    n = (tmpfs_node_t *)br_malloc(sizeof *n);
    if (n == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    tmpfs_node_init(n, type, dir, name, body);

    /* Head insert: entry order is not contractual (readdir guarantees only "." + ".." plus
     * the name set) and head insert avoids a walk. */
    n->next    = dir->child;
    dir->child = n;
    s_fs.nodes_used++;
    if (out != BR_NULL) {
        *out = n;
    }
    return 0;
}

/* Unlink + unreference the body + free the node. Under hard links this deletes one name;
 * dropping the last copy of the data is tmpfs_body_unref's refcount's decision. */
static void tmpfs_node_drop(tmpfs_node_t *dir, tmpfs_node_t *victim)
{
    tmpfs_child_unlink(dir, victim);
    tmpfs_body_unref(victim->body);
    victim->body = BR_NULL;
    br_free(victim);
    s_fs.nodes_used--;
}

/* Fill one dirent; zero the record first, or the name[] tail leaks leftovers. */
static void tmpfs_dirent_fill(br_dirent_t *de, br_u32 type, const char *name, br_u32 name_len)
{
    tmpfs_zero(de, (br_size_t)sizeof *de);
    de->type     = type;
    de->name_len = name_len;
    tmpfs_str_copy(de->name, name, BR_NAME_MAX);
}

/* ---- file data (growable heap buffer + quota) ---- */

/* Return the data buffer to the heap, size/cap to zero. Invariant: data_used == Σ body->cap,
 * so every subtraction matches an addition. Operates on the body, not a name: under hard
 * links truncate affects all names at once. */
static void tmpfs_file_release(tmpfs_node_t *n)
{
    tmpfs_body_t *b = n->body;

    if (b == BR_NULL) {
        return;
    }
    if (b->data != BR_NULL) {
        br_free(b->data);
        b->data = BR_NULL;
    }
    s_fs.data_used -= b->cap;
    b->cap  = 0u;
    b->size = 0u;
}

/* Ensure capacity ≥ need (size untouched; the caller sets it). Returns 0 / -ENOSPC /
 * -ENOMEM. Growth doubles from TMPFS_MIN_CAP: appends are the typical shape, and growing to
 * exactly need each time would be O(n²). Extra capacity is accounted too (see s_fs). */
static int tmpfs_file_reserve(tmpfs_body_t *b, br_u32 need)
{
    br_u32 new_cap;
    br_u8 *p;

    if (need <= b->cap) {
        return 0;
    }
    if (need > TMPFS_DATA_MAX) {
        return BR_ERR(ENOSPC);         /* the single request itself exceeds the bound */
    }

    new_cap = (b->cap > 0u) ? b->cap : TMPFS_MIN_CAP;
    while (new_cap < need) {
        if (new_cap > (TMPFS_DATA_MAX / 2u)) {
            new_cap = TMPFS_DATA_MAX;     /* avoid "×2" wraparound: jump to the bound */
            break;
        }
        new_cap *= 2u;
    }

    /* whole-FS capacity sum after new_cap must stay in bounds */
    if ((s_fs.data_used - b->cap + new_cap) > TMPFS_DATA_MAX) {
        return BR_ERR(ENOSPC);
    }

    p = (br_u8 *)br_realloc(b->data, (br_size_t)new_cap);
    if (p == BR_NULL) {
        return BR_ERR(ENOMEM);         /* old block stays valid (br_realloc POSIX semantics) */
    }
    s_fs.data_used = s_fs.data_used - b->cap + new_cap;
    b->data = p;
    b->cap  = new_cap;
    return 0;
}

/* Truncate/extend to size (POSIX). Extending must explicitly zero the hole: br_realloc does
 * not, and stale heap bytes would be returned as file content (info leak, and not POSIX's
 * "hole reads as 0"). Shrinking takes exact capacity so quota is returned; the basis is cap. */
static int tmpfs_file_truncate(tmpfs_node_t *n, br_u64 size)
{
    tmpfs_body_t *b = n->body;
    br_u8 *p;
    int rc;

    if (size == b->size) {
        return 0;
    }

    if (size > b->size) {                              /* extend (may contain a hole) */
        if (size > (br_u64)TMPFS_DATA_MAX) {
            return BR_ERR(ENOSPC);
        }
        rc = tmpfs_file_reserve(b, (br_u32)size);
        if (rc != 0) {
            return rc;
        }
        tmpfs_zero(b->data + (br_size_t)b->size, (br_size_t)(size - b->size));
        b->size = size;
        return 0;
    }

    if (size == 0u) {                                  /* shrink to 0: whole block to the heap */
        tmpfs_file_release(n);                         /* br_realloc(p,0) frees and returns NULL */
        return 0;
    }
    p = (br_u8 *)br_realloc(b->data, (br_size_t)size);
    if (p == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    s_fs.data_used = s_fs.data_used - b->cap + (br_u32)size;
    b->data = p;
    b->cap  = (br_u32)size;
    b->size = size;
    return 0;
}

/* ---- layer 3 implementation: file side ---- */

static int tmpfs_file_open(br_file_t *f, br_u32 flags)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int rc = tmpfs_require_file(node);

    if (rc != BR_OK) {
        return rc;
    }

    /* O_TRUNC: truncate to 0 on open. Permission checks (a read-only fd must not write) are
     * the vfs/runtime/posix fd layer's job, not repeated here. */
    if ((flags & BR_O_TRUNC) != 0u) {
        rc = tmpfs_file_truncate(node, 0u);
        if (rc != BR_OK) {
            return rc;
        }
    }

    if ((flags & BR_O_APPEND) != 0u) {
        br_file_set_offset(f, (br_s64)node->body->size);
    }
    return 0;
}

static br_s64 tmpfs_file_read(br_file_t *f, void *buf, br_size_t n)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int    rc  = tmpfs_require_file(node);
    br_s64 off;
    br_u64 avail;
    br_size_t cnt;

    if (rc != BR_OK) {
        return rc;
    }
    if ((buf == BR_NULL) && (n > 0u)) {
        return BR_ERR(EINVAL);
    }
    if ((node->body->size > 0u) && (node->body->data == BR_NULL)) {
        return BR_ERR(EIO);            /* invariant broken (size>0 ⇒ data!=NULL) */
    }

    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(EINVAL);
    }
    if ((br_u64)off >= node->body->size) {
        return 0;                         /* at/past EOF: 0 bytes is success (not -EIO) */
    }

    avail = node->body->size - (br_u64)off;
    cnt = (n > (br_size_t)avail) ? (br_size_t)avail : n;   /* avail ≤ 128 KiB ⇒ narrowing safe */
    if (cnt > 0u) {
        tmpfs_copy(buf, node->body->data + (br_size_t)off, cnt);
    }
    br_file_set_offset(f, off + (br_s64)cnt);
    return (br_s64)cnt;
}

static br_s64 tmpfs_file_write(br_file_t *f, const void *buf, br_size_t n)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int    rc  = tmpfs_require_file(node);
    br_s64 off;
    br_u64 need;

    if (rc != BR_OK) {
        return rc;
    }
    if ((buf == BR_NULL) && (n > 0u)) {
        return BR_ERR(EINVAL);
    }

    /* O_APPEND seeks to the end before every write, even a zero-length one. */
    if ((br_file_flags(f) & BR_O_APPEND) != 0u) {
        br_file_set_offset(f, (br_s64)node->body->size);
    }
    if (n == 0u) {
        return 0;
    }

    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(EINVAL);
    }
    need = (br_u64)off + (br_u64)n;
    if (need < (br_u64)off) {
        return BR_ERR(EINVAL);         /* offset arithmetic wrapped */
    }
    if (need > (br_u64)TMPFS_DATA_MAX) {
        return BR_ERR(ENOSPC);         /* write past the 128 KiB quota: the judgment point */
    }

    rc = tmpfs_file_reserve(node->body, (br_u32)need);
    if (rc != BR_OK) {
        return rc;
    }
    if ((br_u64)off > node->body->size) {
        /* hole write: the gap must read back 0 (see truncate) */
        tmpfs_zero(node->body->data + (br_size_t)node->body->size,
                   (br_size_t)((br_u64)off - node->body->size));
    }
    tmpfs_copy(node->body->data + (br_size_t)off, buf, n);
    if (need > node->body->size) {
        node->body->size = need;
    }
    br_file_set_offset(f, (br_s64)need);
    return (br_s64)n;
}

static br_s64 tmpfs_file_lseek(br_file_t *f, br_s64 off, int whence)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int    rc   = tmpfs_require_file(node);
    br_s64 base;
    br_s64 pos;

    if (rc != BR_OK) {
        return rc;
    }
    switch (whence) {
    case BR_SEEK_SET: base = 0; break;
    case BR_SEEK_CUR: base = br_file_offset(f); break;
    case BR_SEEK_END: base = (br_s64)node->body->size; break;
    default:          return BR_ERR(EINVAL);
    }
    if (base < 0) {
        return BR_ERR(EINVAL);
    }
    /* Only non-negative br_s64 results; __builtin_add_overflow avoids signed-overflow UB.
     * Seeking past EOF is allowed: reads then return 0, writes create a hole. */
    if (__builtin_add_overflow(base, off, &pos) || (pos < 0)) {
        return BR_ERR(EINVAL);
    }
    br_file_set_offset(f, pos);
    return pos;
}

static int tmpfs_file_close(br_file_t *f)
{
    /* A file session has nothing to give back: fpriv is still the node (persistent) and vfs
     * frees br_file_t itself; the dir side is the one that br_free's its iterator. */
    (void)f;
    return 0;
}

/* ---- layer 3 implementation: directory side (read = one br_dirent_t) ---- */

static int tmpfs_dir_open(br_file_t *f, br_u32 flags)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    tmpfs_dir_iter_t *it;

    if ((node == BR_NULL) || (node->head.type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);        /* dir side on a non-dir node: use ENOTSUP */
    }
    /* Write-open of a directory is stopped by vfs in br_open, so flags are not judged here
     * (the slot only keeps the contract signature). */
    (void)flags;

    it = (tmpfs_dir_iter_t *)br_malloc(sizeof *it);
    if (it == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    it->dir    = node;
    it->cursor = 0u;
    br_file_set_fpriv(f, it);             /* session state; node identity untouched */
    return 0;
}

static br_s64 tmpfs_dir_read(br_file_t *f, void *buf, br_size_t n)
{
    tmpfs_dir_iter_t *it = (tmpfs_dir_iter_t *)br_file_fpriv(f);
    br_dirent_t *de = (br_dirent_t *)buf;
    tmpfs_node_t *c;
    br_u32 idx;

    if ((it == BR_NULL) || (it->dir == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* Fixed-size records cannot be truncated: too small is -EINVAL (returning 0 would read as
     * "exhausted"); vfs always passes sizeof(br_dirent_t). */
    if ((buf == BR_NULL) || (n < (br_size_t)sizeof(br_dirent_t))) {
        return BR_ERR(EINVAL);
    }

    /* "." and ".." are synthesized, matching lookup */
    if (it->cursor == 0u) {
        it->cursor = 1u;
        tmpfs_dirent_fill(de, BR_INODE_DIR, ".", 1u);
        return (br_s64)sizeof(br_dirent_t);
    }
    if (it->cursor == 1u) {
        it->cursor = 2u;
        tmpfs_dirent_fill(de, BR_INODE_DIR, "..", 2u);
        return (br_s64)sizeof(br_dirent_t);
    }

    /* Re-walk by cursor instead of caching a next pointer: rename/unlink can dangle it, and
     * max 48 nodes makes the re-walk negligible. */
    idx = 2u;
    for (c = it->dir->child; c != BR_NULL; c = c->next) {
        if (idx == it->cursor) {
            it->cursor = idx + 1u;
            tmpfs_dirent_fill(de, c->head.type, c->name, tmpfs_str_len(c->name));
            return (br_s64)sizeof(br_dirent_t);
        }
        idx++;
    }
    return 0;                             /* exhausted */
}

static int tmpfs_dir_close(br_file_t *f)
{
    tmpfs_dir_iter_t *it = (tmpfs_dir_iter_t *)br_file_fpriv(f);

    if (it != BR_NULL) {
        br_free(it);
        br_file_set_fpriv(f, BR_NULL);    /* makes a second close a no-op */
    }
    return 0;
}

/* ---- layer 2 implementation: inode ops (each slot checks the node type) ---- */

/* lookup allocates nothing (nodes are the persistent objects). "." / ".." resolve inline;
 * no error channel, so "not found" and "wrong table (non-dir)" both give BR_NULL (-ENOENT). */
static br_inode_t *tmpfs_lookup(br_inode_t *dir, const char *name)
{
    tmpfs_node_t *d;
    tmpfs_node_t *c;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR) || (name == BR_NULL)) {
        return BR_NULL;
    }
    d = tmpfs_ino_node(dir);
    if (d == BR_NULL) {
        return BR_NULL;
    }
    if (tmpfs_str_eq(name, ".") == BR_TRUE) {
        return &d->head;
    }
    if (tmpfs_str_eq(name, "..") == BR_TRUE) {
        return &d->parent->head;          /* d->parent is always non-null (root self-refers) */
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_NULL;                   /* invalid name: for a walk it is simply "absent" */
    }
    for (c = d->child; c != BR_NULL; c = c->next) {
        if (tmpfs_str_eq(c->name, name) == BR_TRUE) {
            return &c->head;
        }
    }
    return BR_NULL;
}

static int tmpfs_create(br_inode_t *dir, const char *name, br_u32 flags, br_inode_t **out)
{
    tmpfs_node_t *d;
    tmpfs_node_t *ex;
    tmpfs_node_t *n = BR_NULL;
    int rc;

    if (out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *out = BR_NULL;
    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);        /* only a directory can create entries */
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    d = tmpfs_ino_node(dir);

    ex = tmpfs_child_find(d, name);
    if (ex != BR_NULL) {
        if ((flags & BR_O_EXCL) != 0u) {
            return BR_ERR(EEXIST);
        }
        /* O_CREAT without O_EXCL on an existing name: reuse (Linux). A SYMLINK cannot reach
         * here — vfs expands a final link before create, and O_NOFOLLOW is stopped earlier
         * with -ELOOP. */
        if (ex->head.type != BR_INODE_FILE) {
            return BR_ERR(EISDIR);     /* create makes files only; dirs go through mkdir */
        }
        *out = &ex->head;
        return 0;
    }

    /* A file always has a body (empty, grows on demand). */
    tmpfs_body_t *body = BR_NULL;
    rc = tmpfs_body_new(&body);
    if (rc != BR_OK) {
        return rc;
    }
    rc = tmpfs_node_add(d, name, BR_INODE_FILE, body, &n);
    if (rc != BR_OK) {
        tmpfs_body_unref(body);           /* link-in failed: the body is ours again */
        return rc;
    }
    *out = &n->head;
    return 0;
}

static int tmpfs_unlink(br_inode_t *dir, const char *name)
{
    tmpfs_node_t *d;
    tmpfs_node_t *victim;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);
    }
    /* Mutating names take the full rules; unlink(".") is -EINVAL here, matching rmdir/rename. */
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    d = tmpfs_ino_node(dir);
    victim = tmpfs_child_find(d, name);
    if (victim == BR_NULL) {
        return BR_ERR(ENOENT);
    }
    /* A symlink is file-class: unlink removes the link itself, target liveness irrelevant. */
    if ((victim->head.type != BR_INODE_FILE) && (victim->head.type != BR_INODE_SYMLINK)) {
        return BR_ERR(EISDIR);         /* dirs need rmdir (tree removal is the caller's) */
    }
    /* Hard link: this removes one name. The body's fate is the refcount's — while another
     * name points at it the data must stay. */
    tmpfs_node_drop(d, victim);
    return 0;
}

static int tmpfs_mkdir(br_inode_t *dir, const char *name)
{
    tmpfs_node_t *d;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    d = tmpfs_ino_node(dir);
    if (tmpfs_child_find(d, name) != BR_NULL) {
        return BR_ERR(EEXIST);         /* already exists (including "it is a file"): EEXIST */
    }
    /* out = BR_NULL: mkdir only answers rc (vfs looks up the inode itself) */
    return tmpfs_node_add(d, name, BR_INODE_DIR, BR_NULL, BR_NULL);
}

static int tmpfs_rmdir(br_inode_t *dir, const char *name)
{
    tmpfs_node_t *d;
    tmpfs_node_t *victim;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(EINVAL);         /* "." / ".." are rejected here too */
    }
    d = tmpfs_ino_node(dir);
    victim = tmpfs_child_find(d, name);
    if (victim == BR_NULL) {
        return BR_ERR(ENOENT);
    }
    if (victim->head.type != BR_INODE_DIR) {
        return BR_ERR(ENOTDIR);        /* rmdir on a file = wrong tool (not "not empty") */
    }
    if (victim->child != BR_NULL) {
        /* -ENOTEMPTY (39), not -EBUSY: that means "mount point not removable" (TC-VFS-012),
         * and runtime/posix errno mapping needs the distinction. */
        return BR_ERR(ENOTEMPTY);
    }
    /* The root cannot be rmdir'd: it has no parent entry, and "." / ".." are rejected by the
     * name rules, so no special code is needed. */
    tmpfs_node_drop(d, victim);           /* a dir has no body ⇒ unlink + free node only */
    return 0;
}

static int tmpfs_rename(br_inode_t *dir, const char *name, br_inode_t *ndir, const char *nname)
{
    tmpfs_node_t *d;
    tmpfs_node_t *nd;
    tmpfs_node_t *victim;
    tmpfs_node_t *t;

    if ((dir == BR_NULL) || (ndir == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if ((dir->type != BR_INODE_DIR) || (ndir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);
    }
    /* The target dir must be this plugin's node: child/next/parent are tmpfs's private tail,
     * so a foreign node would be read with the wrong layout. */
    if (ndir->iops != &s_iops) {
        return BR_ERR(ENOTSUP);
    }
    /* "." / ".." explicitly rejected (-EINVAL), same as rmdir/unlink */
    if ((tmpfs_name_ok(name) == BR_FALSE) || (tmpfs_name_ok(nname) == BR_FALSE)) {
        return BR_ERR(EINVAL);
    }

    d = tmpfs_ino_node(dir);
    nd = tmpfs_ino_node(ndir);
    victim = tmpfs_child_find(d, name);
    if (victim == BR_NULL) {
        return BR_ERR(ENOENT);
    }
    if (tmpfs_child_find(nd, nname) != BR_NULL) {
        /* v1 never overwrites: an existing target is -EEXIST for every case, and a same-name
         * rename is a caller error (the POSIX no-op would hide script bugs). */
        return BR_ERR(EEXIST);
    }
    /* Refuse moving a directory into its own subtree: the cycle a→…→a would make no walk
     * terminate (lookup("..") and child chains interlock). */
    if (victim->head.type == BR_INODE_DIR) {
        for (t = nd; t != BR_NULL; t = t->parent) {
            if (t == victim) {
                return BR_ERR(EINVAL);
            }
            if (t->parent == t) {
                break;                    /* reached root */
            }
        }
    }

    /* Unlink then insert: the same-parent case goes through this too (no self-cycle) */
    tmpfs_child_unlink(d, victim);
    tmpfs_str_copy(victim->name, nname, BR_NAME_MAX);
    victim->parent = nd;
    victim->next   = nd->child;
    nd->child      = victim;
    return 0;
}

static int tmpfs_getattr(br_inode_t *ino, br_stat_t *st)
{
    tmpfs_node_t *n;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if ((ino->type != BR_INODE_DIR) && (ino->type != BR_INODE_FILE) &&
        (ino->type != BR_INODE_SYMLINK)) {
        return BR_ERR(ENOTSUP);
    }
    n = tmpfs_ino_node(ino);
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /* getattr ignores the input's valid field (that is setattr's). nlink: files/symlinks
     * report the body's refcount, directories report 1 (no subdir count, unlike Linux). */
    st->valid = BR_STAT_SIZE | BR_STAT_NLINK;
    st->type  = ino->type;
    st->size  = (ino->type == BR_INODE_DIR) ? 0u : n->body->size;
    st->nlink = (n->body != BR_NULL) ? n->body->refs : 1u;
    return 0;
}

/* ---- links (layer 2, namespace): vfs-core handles symlink resolution. ---- */

/* Create in `dir` a symlink named `name` with target string `target`. */
static int tmpfs_symlink(br_inode_t *dir, const char *name, const char *target)
{
    tmpfs_node_t *d;
    tmpfs_body_t *body = BR_NULL;
    br_u32        tlen;
    int           rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);
    }
    if (target == BR_NULL || target[0] == '\0') {
        return BR_ERR(EINVAL);
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    /* The target's existence is not checked (dangling links are legal), only its length: the
     * scan stops at NUL or BR_SYMLINK_MAX (same discipline as tmpfs_name_ok). */
    for (tlen = 0u; tlen < BR_SYMLINK_MAX; tlen++) {
        if (target[tlen] == '\0') {
            break;
        }
    }
    if (tlen >= BR_SYMLINK_MAX) {
        return BR_ERR(ENAMETOOLONG);
    }

    d = tmpfs_ino_node(dir);
    if (tmpfs_child_find(d, name) != BR_NULL) {
        return BR_ERR(EEXIST);
    }

    rc = tmpfs_body_new(&body);
    if (rc != BR_OK) {
        return rc;
    }
    /* The target string lives in the data body and goes through tmpfs_file_reserve, so it
     * shares the quota accounting. */
    rc = tmpfs_file_reserve(body, tlen + 1u);
    if (rc == BR_OK) {
        body->size = (br_u64)tlen;
        /* Include the trailing NUL so readlink returns a C string. */
        tmpfs_copy(body->data, target, (br_size_t)tlen + 1u);
    }
    if (rc != BR_OK) {
        tmpfs_body_unref(body);
        return rc;
    }

    rc = tmpfs_node_add(d, name, BR_INODE_SYMLINK, body, BR_NULL);
    if (rc != BR_OK) {
        tmpfs_body_unref(body);
        return rc;
    }
    return 0;
}

/* Read the link's own target (no follow). Returns the full length even when truncated. */
static int tmpfs_readlink(br_inode_t *ino, char *buf, br_size_t cap)
{
    tmpfs_node_t *n;

    if (ino == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (ino->type != BR_INODE_SYMLINK) {
        return BR_ERR(EINVAL);         /* POSIX: not a symlink ⇒ EINVAL */
    }
    n = tmpfs_ino_node(ino);
    if (n == BR_NULL || n->body == BR_NULL || n->body->data == BR_NULL) {
        return BR_ERR(EIO);            /* body expected but missing: FS-side inconsistency */
    }

    const br_size_t len = (br_size_t)n->body->size;
    if (buf != BR_NULL && cap > 0u) {
        br_size_t cnt = (len < cap) ? len : (cap - 1u);   /* leave a byte for NUL */
        tmpfs_copy(buf, n->body->data, cnt);
        buf[cnt] = '\0';
    }
    return (int)len;
}

/* Create a hard link: new name → the same data body (refcount +1), sharing data/length. */
static int tmpfs_link(br_inode_t *dir, const char *name, br_inode_t *target)
{
    tmpfs_node_t *d;
    tmpfs_node_t *t;
    int           rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR) || (target == BR_NULL)) {
        return BR_ERR(ENOTSUP);
    }
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    /* Directories take no hard links (parent/child cycle); POSIX says EPERM. vfs stops it
     * earlier, so this guards direct callers such as self-tests. */
    if (target->type == BR_INODE_DIR) {
        return BR_ERR(EPERM);
    }
    /* Target must be this plugin's node: only this FS interprets the private tail. */
    if (target->iops != &s_iops) {
        return BR_ERR(EXDEV);
    }
    t = tmpfs_ino_node(target);
    if (t == BR_NULL || t->body == BR_NULL) {
        return BR_ERR(EIO);
    }

    d = tmpfs_ino_node(dir);
    if (tmpfs_child_find(d, name) != BR_NULL) {
        return BR_ERR(EEXIST);
    }

    /* Just share the pointer: the body exists, add one reference (that is all a hard link is). */
    t->body->refs++;
    rc = tmpfs_node_add(d, name, target->type, t->body, BR_NULL);
    if (rc != BR_OK) {
        t->body->refs--;                  /* link-in failed: undo that reference */
        return rc;
    }
    return 0;
}

static int tmpfs_setattr(br_inode_t *ino, const br_stat_t *st)
{
    tmpfs_node_t *n;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* v1 honors only size (truncate). Unsupported bits must report -ENOTSUP, not pretend
     * success: the caller would think the attribute took effect. */
    if ((st->valid & ~(br_u32)BR_STAT_SIZE) != 0u) {
        return BR_ERR(ENOTSUP);
    }
    if ((st->valid & BR_STAT_SIZE) == 0u) {
        return 0;                         /* nothing requested: a no-op is the right answer */
    }
    n = tmpfs_ino_node(ino);
    if ((n == BR_NULL) || (ino->type != BR_INODE_FILE)) {
        return BR_ERR(EISDIR);         /* a directory has no "file length" to truncate */
    }
    return tmpfs_file_truncate(n, st->size);
}

/* ---- layer 1 implementation: super (mount / free_inode) ---- */

/* Mount rollback: the tree is only "root + just-created dirs" (depth 1, no file data), so no
 * generic recursive free is needed; a generic one is v2. */
static void tmpfs_drop_tree(tmpfs_node_t *root)
{
    tmpfs_node_t *c = root->child;

    while (c != BR_NULL) {
        tmpfs_node_t *next = c->next;
        tmpfs_body_unref(c->body);
        br_free(c);
        s_fs.nodes_used--;
        c = next;
    }
    tmpfs_body_unref(root->body);
    br_free(root);
    s_fs.nodes_used--;
}

static int tmpfs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    static const char *const pre[] = { "dev", "data", "tmp" };   /* pre-created dirs */
    tmpfs_node_t *r;
    br_size_t     i;
    int           rc = 0;

    if ((cfg == BR_NULL) || (root == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    *root = BR_NULL;

    /* The mount point is not validated (unlike fs/devfs): tmpfs nodes are path-independent,
     * so mounting elsewhere is consistent; v1's "mount / only" comes from the mount plan. */

    /* There is exactly one accounting block (ops take no fs_priv, so they use the file-scope
     * static), hence vfs must hand back that one — an error beats two divergent books. */
    if (fs_priv != (void *)&s_fs) {
        return BR_ERR(EINVAL);
    }

    /* mount is one-shot (a second registration on the same path is -EEXIST in vfs), so
     * zeroing the books means "only one tree from here"; v2 unmount would free the tree. */
    s_fs.nodes_used = 0u;
    s_fs.data_used  = 0u;

    r = (tmpfs_node_t *)br_malloc(sizeof *r);
    if (r == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    tmpfs_node_init(r, BR_INODE_DIR, r, "/", BR_NULL);   /* root: parent = self, dir has no body */
    s_fs.nodes_used = 1u;

    /* Pre-create /dev /data /tmp through the same mkdir path, so fs/devfs finds its /dev
     * mount point already present (vfs would otherwise auto-mkdir). */
    for (i = 0u; i < BR_ARRAY_SIZE(pre); i++) {
        rc = tmpfs_mkdir(&r->head, pre[i]);
        if (rc != 0) {
            break;
        }
    }
    if (rc != 0) {
        br_log_error("tmpfs: mount %s failed at pre-create (%d)", cfg->path, rc);
        tmpfs_drop_tree(r);              /* no half-built tree: vfs won't record this mount */
        return rc;
    }

    *root = &r->head;
    br_log_info("tmpfs: mounted %s (nodes<=%u, data<=%u KiB)",
                cfg->path, TMPFS_NODE_MAX, TMPFS_DATA_MAX / 1024u);
    return 0;
}

/* free_inode is a no-op: vfs passes only transient inodes, while tmpfs nodes are persistent
 * objects returned directly by lookup(). Nodes are freed by unlink/rmdir/rename; a br_free
 * here would tear down the live directory tree. */
static void tmpfs_free_inode(br_inode_t *ino)
{
    (void)ino;
}

/* ---- lifecycle implementation ---- */

/* EARLY phase: no threads and the heap is not claimed yet, so allocation is not allowed;
 * tmpfs has no static declaration to make. */
int tmpfs_early_init(void)
{
    return 0;
}

/* CORE phase: register tmpfs as a driver; mounting is the APP's POSIX mount() call, so
 * "where it is mounted" is not hard-coded here. init builds no tree — the root inode and
 * pre-created dirs belong to the super layer's mount slot, called by br_vfs_mount. Negative
 * errno is passed up (first failure halts via the plugin manager). */
int tmpfs_init(void)
{
    const int rc = br_fs_driver_register("tmpfs", &s_fs_ops, (void *)&s_fs);

    if (rc != 0) {
        br_log_error("tmpfs: register driver failed (%d)", rc);
        return rc;
    }
    return 0;
}

/* START phase: usage summary (boot evidence, not a criterion). data = Σ capacities taken
 * from the heap, not Σ file lengths (see the s_fs note). */
int tmpfs_start(void)
{
    br_log_info("tmpfs: usage nodes %u/%u, data %u/%u B",
                s_fs.nodes_used, TMPFS_NODE_MAX, s_fs.data_used, TMPFS_DATA_MAX);
    return 0;
}
