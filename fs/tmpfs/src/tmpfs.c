/*
 * fs/tmpfs — brickOS RAM rootfs (implementation)
 *
 * Design sources:
 *   - `7-03-concrete-fs.md` §2: tmpfs = RAM filesystem; nodes and data come from the core
 *     heap (`br_malloc`); full file semantics (read/write/lseek/truncate/unlink/rename/mkdir/stat);
 *     mounted as rootfs ("/"); /dev /data /tmp come from the pre-created directory list;
 *   - `7-03-concrete-fs.md` §6: mount order item 1 = fs/tmpfs → "/" (pre-create three dirs);
 *   - `7-01-vfs.md` §2: four op layers (super/inode/file/dentry), **path walk in vfs-core**;
 *   - contract header `framework/vfs-core/include/br/vfs/br_vfs.h` (sole authority here).
 *
 * ============================== what this plugin is ==============================
 * tmpfs is a **management-class FS** (D21): no medium, no block layer; nodes and file data
 * all come from the core heap. Two duties:
 *   1) rootfs fallback — absolute paths with no longer mount point land here (last hop);
 *   2) namespace skeleton — a mediumless product still has /, /dev, /data, /tmp.
 * Losing data on power-off is a **role split**, not a defect: persistence is littlefs/EROFS.
 * ========================================================================
 *
 * ★ Difference from the "transient inode" model (key to reading this file)
 *   Design SD-3 says an inode is a walk by-product discarded by `free_inode` — that is
 *   littlefs's shape (node = path-prefix wrapper, rebuilt per walk). tmpfs is the reverse:
 *   **nodes are br_malloc'd persistent objects** on the parent's child list, so
 *     - `lookup()` returns existing nodes, **allocating nothing** (walks are zero-alloc);
 *     - `free_inode` must be a no-op: vfs passes only transient inodes, and tmpfs has none;
 *       a stray br_free would tear down the live tree (see tmpfs_free_inode).
 *   The node pointer persists in `head.fpriv` — the header's only "private" slot — which vfs
 *   hands to `br_file_t` on open (br_vfs.c `f->fpriv = ino->fpriv`); files find the node by it.
 *
 * ★ Known debt (v0.2 prototype)
 *   Quotas (data 128 KiB / 48 nodes) are **hard-coded static upper bounds**, not manifest-
 *   budget derived — same origin as the hard-coded pool ratios in
 *   platform/qemu-aarch64/src/memmap.c (registered as br-wa-mem-001). Exit condition:
 *   **manifest budget → tmpfs quota** (composer turns [budget] into an FS quota descriptor
 *   read at mount, not a compile-time constant); until then these values are **safe**, not
 *   true, bounds: enough for the VFS conformance cases yet never draining the 1 MiB heap.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

/* =====================================================================
 * Lifecycle hooks (EARLY / CORE / START)
 *
 * Name = symbol_prefix(short) + phase, referenced by the generated
 * build/gen/fs/tmpfs/plugin_desc.c (design 1-01 §9 / 3-05 §2). This plugin **exports no
 * API**: its behavior is driven generically through the VFS contract (see vfs-core's
 * br_vfs_conformance), so every function here but these three hooks is static; the local
 * prototypes satisfy -Wmissing-prototypes (hook names are derived by the generator).
 * ===================================================================== */
int tmpfs_early_init(void);
int tmpfs_init(void);
int tmpfs_start(void);

/* =====================================================================
 * Quotas and name rules (static bounds; see the debt note at the top)
 * ===================================================================== */

/* Upper bound on the sum of all file **capacities** (accounting basis = cap, see s_fs). */
#define TMPFS_DATA_MAX   (128u * 1024u)

/* Node count bound (root + three pre-created dirs + test margin). 48 × ~104 B ≈ 5 KiB of a 1 MiB heap. */
#define TMPFS_NODE_MAX   48u

/* Minimum capacity on first write. Neither request a block for a 1-byte write nor realloc
 * +1 byte at a time (that turns sequential append into O(n²), and append is tmpfs's typical write). */
#define TMPFS_MIN_CAP    64u

/*
 * Mount point constant — **only a self-test fixture name** (e.g. TC-VFS's "the mount table
 * has a '/'" assertion).
 *
 * ★ Since ADR-0030 "where to mount" is **no longer decided here**: tmpfs only registers the
 *   driver via `br_fs_driver_register("tmpfs", …)`; the mount point comes from the **APP's
 *   `[[mount]]`** (see each APP's brick.toml). This was once two sources of truth (code
 *   constant + manifest declaration), debt br-wa-fs-001; the generation pipeline paid it off,
 *   so the constant is now just a **self-test fixture name** and takes no part in mounting.
 *   `tmpfs_mount()` also does **not** refuse other paths: an APP may mount tmpfs anywhere.
 */
#define TMPFS_MOUNT_PATH "/"

/* Premise for "cap is br_u32": quotas and offset arithmetic stay under 2 GiB ⇒ no wraparound. */
_Static_assert(TMPFS_DATA_MAX <= 0x7fffffffu, "quota must keep cap (br_u32) and offset arithmetic safe");

/* =====================================================================
 * Node model (common head + private tail; CA-2)
 * ===================================================================== */

typedef struct tmpfs_node tmpfs_node_t;
typedef struct tmpfs_body tmpfs_body_t;

/*
 * **Data body** (ADR-0013): file content is **shared by names**, not a property of a name.
 * The whole semantics of a hard link (`link`) is "two names → one inode → one set of data,
 * one length, one offset base"; so `data/size/cap` move off the node into a refcounted body:
 *
 *   - `refs > 1` ⇒ the body is shared by several dentries (nodes) — that is a hard link;
 *   - `refs == 0` ⇒ return it to the heap (data buffer plus its `s_fs.data_used` quota).
 *
 * Invariant: **FILE and SYMLINK nodes always have a body** (allocated on creation), a
 * directory never does ⇒ no `n->body->…` needs a null check. A symlink's target string
 * **lives in the body** (it is just "a little data"), so no fixed-size array per node.
 * Because bodies are shared, nodes are actually smaller than in the ADR-0009 version.
 */
struct tmpfs_body {
    br_u8  *data;    /* allocated bytes = cap */
    br_u64  size;    /* file bytes / symlink target length */
    br_u32  cap;     /* data capacity (bytes) — also the quota accounting basis */
    br_u32  refs;    /* nodes referencing it (= st_nlink) */
};

/*
 * `head` must be the first field — all CA-2 asks is "vfs knows only br_inode_t* and can use
 * it as a node directly". Recovering the node goes through `head.fpriv` (see
 * tmpfs_ino_node): fpriv is the contract carrier handed to the file side, so using it costs
 * one fewer cross-plugin symbol; both point to the same place with head first.
 */
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

/*
 * Directory iterator: allocated by the dir's open(), freed by close(), stored on the **file
 * handle's** fpriv — never on node head.fpriv (that is node identity, not session state; one
 * directory can be open for iteration several times at once).
 *
 * Lifetime boundary (a known v1 gap, not specific to this file): the iterator only
 * **borrows** the node. rmdir'ing the directory mid-iteration frees the node, so a later
 * read is use-after-free. v1 has **no refcounting** (br_vfs.h: one APP, no fork, refcounts
 * are v2), so "delete while reading" is caller misuse; no defense here (noted so nobody
 * assumes it is protected).
 */
typedef struct {
    tmpfs_node_t *dir;      /* directory being enumerated (borrowed, not owned) */
    br_u32        cursor;   /* 0 = ".", 1 = "..", ≥2 = the (cursor-2)th child */
} tmpfs_dir_iter_t;

/*
 * Accounting block. Why **file-scope static** instead of passed in via fs_priv: neither
 * inode nor file ops take fs_priv (per contract), they see only nodes ⇒ the books must hang
 * off plugin statics. The fs_priv mount receives must be this one (checked in tmpfs_mount).
 *
 * Accounting basis = Σ body->cap (**capacity**), not Σ size: cap is what occupies the heap;
 * accounting by size would make the extra capacity from geometric growth an "invisible
 * overrun", while the bound means exactly "how much was taken from the heap". The cost is
 * that the sum of file lengths can be far below data_used — honest accounting, not a leak:
 * truncate and unlink both return cap (see tmpfs_file_release / tmpfs_file_truncate).
 */
typedef struct {
    br_u32 nodes_used;   /* allocated nodes (including root) */
    br_u32 data_used;    /* sum of allocated capacities; bound TMPFS_DATA_MAX */
} tmpfs_fs_t;

static tmpfs_fs_t s_fs;

/* =====================================================================
 * Forward declarations
 *
 * Order note: the ops tables are this plugin's **contract surface** and come before the
 * function bodies, so the file reads top-down as "contract (tables) → implementation
 * (functions)"; the tables reference function names, so those need declarations first.
 * ===================================================================== */

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

/* =====================================================================
 * ops tables (layers 1 / 2 / 3)
 * ===================================================================== */

/*
 * Layer 2: inode ops. ★ Both node kinds **share** one table (each slot checks its own type)
 * — unlike br_vfs.h's comment "iops: ops for a directory inode; non-dir = BR_NULL". Reason:
 * vfs dispatches inode ops only for directories (walks and namespace changes are always
 * "parent dir + name"), so sharing concentrates the type guards in one place and keeps one
 * table; the only cost is a non-null pointer on file nodes (≤48 nodes).
 */
static const br_inode_ops_t s_iops = {
    .lookup  = tmpfs_lookup,
    .create  = tmpfs_create,
    .unlink  = tmpfs_unlink,
    .mkdir   = tmpfs_mkdir,
    .rmdir   = tmpfs_rmdir,
    .rename  = tmpfs_rename,
    .getattr = tmpfs_getattr,
    .setattr = tmpfs_setattr,
    /* ADR-0013 additions (table tail — existing FS named initializers unaffected) */
    .symlink  = tmpfs_symlink,
    .readlink = tmpfs_readlink,
    .link     = tmpfs_link,
};

/* Layer 3: file side. Unsupported slots are **explicitly** BR_NULL ⇒ vfs dispatch yields
 * -ENOTSUP (per contract, not a "forgot to fill it" default). */
static const br_file_ops_t s_file_fops = {
    .open        = tmpfs_file_open,
    .read        = tmpfs_file_read,
    .write       = tmpfs_file_write,
    .lseek       = tmpfs_file_lseek,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,   /* data lives only in the heap; nothing to persist */
    .poll        = BR_NULL,   /* in-memory files are always ready; waiting is v2 (SD-7) */
    .poll_attach = BR_NULL,   /* reserved for v2 (wait-queue); always NULL in v1 */
    .close       = tmpfs_file_close,
};

/* Directories have a "file side" too (read = enumerate entries) — D23's layering keeps
 * opendir/readdir **fully generic** in vfs-core, with no "directory-specific ops". Write and
 * seek are meaningless for a dir, so those slots stay empty. */
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

/* Layer 1: super. unmount/sync are v1's **signature-first** slots (runtime mount/unmount is
 * not before v3, O-S3); tmpfs also has no "volume sync" (no volume). */
static const br_fs_ops_t s_fs_ops = {
    .mount      = tmpfs_mount,
    .free_inode = tmpfs_free_inode,
    .unmount    = BR_NULL,
    .sync       = BR_NULL,
};

/* =====================================================================
 * Small helpers (freestanding: no <string.h>; this is the whole "string library")
 * ===================================================================== */

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

/* Bounded copy (cap **includes** the trailing '\0'). Both call sites (node name / dentry
 * name) are already guaranteed ≤ BR_NAME_MAX-1 by tmpfs_name_ok, so the truncation branch is
 * pure defense (never on the normal path). */
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

/*
 * Block copy/zero: use compiler builtins, no <string.h>. Under freestanding GCC already
 * lowers "whole-block move / zero" (struct assignment, recognizable loop patterns) to
 * memcpy/memset calls, and core/src/string.c provides those symbols into the image (a
 * prerequisite for linking this prototype) ⇒ going through builtins needs neither a header
 * nor a second implementation — writing our own would hit the "lowered into memcpy"
 * recursion trap (core/src/string.c records that experiment).
 */
static void tmpfs_copy(void *dst, const void *src, br_size_t n)
{
    (void)__builtin_memcpy(dst, src, n);
}

static void tmpfs_zero(void *dst, br_size_t n)
{
    (void)__builtin_memset(dst, 0, n);
}

/*
 * Name rules (**single** enforcement point; create/mkdir/rename all go through it):
 *   non-empty, no '/', not "." / "..", length ≤ BR_NAME_MAX-1.
 *
 * Why length is checked by scanning byte-by-byte to NUL or BR_NAME_MAX instead of calling
 * tmpfs_str_len first: names come from callers (vfs/runtime/posix), and an unterminated
 * string here is **read for at most BR_NAME_MAX bytes** — an out-of-bounds read is far worse
 * than returning -EINVAL. '.'/'..' are rejected only for **mutating** names; lookup resolves
 * them inline earlier (see tmpfs_lookup), so "/." and "/.." are valid walk paths.
 */
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

/* Node identity: use head.fpriv (set to self at creation), not br_inode_priv(). With head
 * first they point to the same place, but fpriv is the contract carrier vfs hands to the
 * file side and adds no extra cross-plugin symbol.
 * Note: fops->open() overwrites the **br_file_t's** fpriv (dir iterator), not this one. */
static tmpfs_node_t *tmpfs_ino_node(const br_inode_t *ino)
{
    return (tmpfs_node_t *)ino->fpriv;
}

/* File side: the session's fpriv is the node (file open keeps it; dir open swaps in an iterator) */
static tmpfs_node_t *tmpfs_session_node(br_file_t *f)
{
    return (tmpfs_node_t *)br_file_fpriv(f);
}

/*
 * Type guard for file-side slots. vfs dispatches by node type, so a mismatch should not
 * reach here; but "never crash" beats "trust the layer above" — a corrupted inode should get
 * -EISDIR (file op on a directory), not a kernel crash inside a dereference.
 */
static int tmpfs_require_file(const tmpfs_node_t *n)
{
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    return (n->head.type == BR_INODE_FILE) ? BR_OK : BR_ERR(EISDIR);
}

/* Child list find/unlink. Linear is fine: at most TMPFS_NODE_MAX = 48 entries, no hashing needed. */
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
 * file side differs by type. `body` comes from the caller: file/symlink **must** be non-null
 * (see the tmpfs_body invariant), a directory always BR_NULL.
 * A symlink's `fops` is BR_NULL — a link is "visible but not openable"; vfs stops it at
 * expansion or at the O_NOFOLLOW layer (⇒ -ELOOP) before the file side. */
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

/* =====================================================================
 * Data body: create / reference / unreference (ADR-0013)
 * ===================================================================== */

/* Create an empty body (refs = 1). Capacity grows on demand (see tmpfs_file_reserve), so only a header's cost here. */
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

/* Create a node and link it into dir (shared by create/mkdir/symlink/link: one implementation
 * of name checks, quota and linking). `body` may be BR_NULL (dir); otherwise ownership moves
 * to the node (on failure the caller unreferences). */
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

    /* Head insert: entry order is **not contractual** (readdir guarantees only "." + ".." +
     * the name set), and head insert avoids a walk. If creation order ever must equal readdir
     * order, switch to tail insert (max 48, cost negligible). */
    n->next    = dir->child;
    dir->child = n;
    s_fs.nodes_used++;
    if (out != BR_NULL) {
        *out = n;
    }
    return 0;
}

/* Unlink + unreference the body + free the node (under hard links, "delete one name" and
 * "delete the last copy of the data" are two things; this does the former — the latter is
 * decided by tmpfs_body_unref's refcount). */
static void tmpfs_node_drop(tmpfs_node_t *dir, tmpfs_node_t *victim)
{
    tmpfs_child_unlink(dir, victim);
    tmpfs_body_unref(victim->body);
    victim->body = BR_NULL;
    br_free(victim);
    s_fs.nodes_used--;
}

/* Fill one fixed-size dirent. Zero the whole record first: otherwise the name[] tail leaks leftovers. */
static void tmpfs_dirent_fill(br_dirent_t *de, br_u32 type, const char *name, br_u32 name_len)
{
    tmpfs_zero(de, (br_size_t)sizeof *de);
    de->type     = type;
    de->name_len = name_len;
    tmpfs_str_copy(de->name, name, BR_NAME_MAX);
}

/* =====================================================================
 * File data (growable heap buffer + quota)
 * ===================================================================== */

/*
 * Return the data buffer to the heap, size/cap to zero. Invariant: s_fs.data_used ==
 * Σ body->cap — every addition has a matching subtraction, so this subtraction is safe (no
 * path changes cap without accounting).
 * ★ This operates on the **body**, not on a name: under hard links truncate affects all names
 * at once (correct).
 */
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

/*
 * Ensure capacity ≥ need (**size untouched**; the caller sets it afterwards). Returns
 * 0 / -ENOSPC / -ENOMEM. Growth = geometric doubling (from TMPFS_MIN_CAP): sequential append
 * is tmpfs's typical write shape, and growing to exactly need each time would degrade it to
 * O(n²). Extra capacity is accounted too (see the s_fs basis note).
 */
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

    /* Quota enforcement: the **whole-FS capacity sum** after new_cap must stay in bounds */
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

/*
 * Truncate/extend to size (POSIX truncate semantics). Two non-obvious tradeoffs:
 *   1) extending must **explicitly zero** the hole — br_realloc does not zero, and stale
 *      data from the previous heap block would be returned as file content (an info leak,
 *      and not POSIX's "hole reads as 0");
 *   2) shrinking takes **exact** capacity (returning quota), not the old cap — the basis is
 *      cap, so keeping it keeps quota no longer needed. Cost: one extra realloc on shrink,
 *      but shrinks are rare.
 */
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
        tmpfs_file_release(n);                         /* avoid br_realloc(p,0): it frees and returns NULL */
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

/* =====================================================================
 * Layer 3 implementation: file side
 * ===================================================================== */

static int tmpfs_file_open(br_file_t *f, br_u32 flags)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    int rc = tmpfs_require_file(node);

    if (rc != BR_OK) {
        return rc;
    }

    /* O_TRUNC: truncate to 0 on open. **Permissions are not enforced here** (O_TRUNC needs
     * write permission / a read-only fd must not write): that is the vfs/runtime/posix fd
     * layer's job (the contract header puts it in the flags semantics too). Repeating the
     * same policy FS-side would create two sources of truth that eventually diverge. */
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
        return BR_ERR(EIO);            /* broken invariant (size>0 ⇒ data non-null): error, don't deref */
    }

    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(EINVAL);
    }
    if ((br_u64)off >= node->body->size) {
        return 0;                         /* at/past EOF: 0 bytes is **success** (not -EIO) */
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

    /* BR_O_APPEND's contract is "seek to the end **before every write**", so even a
     * zero-length write seeks first (differs from Linux internals; the contract text wins). */
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
        /* Hole write (write after lseek past EOF): the gap must read back 0, see case 1) */
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
    /* Only non-negative br_s64 results; use the builtin to avoid signed-overflow UB.
     * Seeking past EOF is allowed (POSIX): reads then return 0, writes create a hole. */
    if (__builtin_add_overflow(base, off, &pos) || (pos < 0)) {
        return BR_ERR(EINVAL);
    }
    br_file_set_offset(f, pos);
    return pos;
}

static int tmpfs_file_close(br_file_t *f)
{
    /* A file session has nothing to give back: fpriv is still the node (a persistent object)
     * and vfs frees br_file_t itself. Paired with the dir side (which must br_free the
     * iterator) — "open puts it in, close takes it out". */
    (void)f;
    return 0;
}

/* =====================================================================
 * Layer 3 implementation: directory side (read = one br_dirent_t)
 * ===================================================================== */

static int tmpfs_dir_open(br_file_t *f, br_u32 flags)
{
    tmpfs_node_t *node = tmpfs_session_node(f);
    tmpfs_dir_iter_t *it;

    if ((node == BR_NULL) || (node->head.type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);        /* dir side on a non-dir node: no errno for it, use ENOTSUP */
    }
    /* "write-open of a directory ⇒ -EISDIR" is stopped by vfs in br_open (see br_vfs.c);
     * not repeated here, so this slot only reads flags and does not judge them (kept for the
     * contract signature). */
    (void)flags;

    it = (tmpfs_dir_iter_t *)br_malloc(sizeof *it);
    if (it == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    it->dir    = node;
    it->cursor = 0u;
    br_file_set_fpriv(f, it);             /* session state on the **file handle**; node identity untouched */
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
    /* Fixed-size records cannot be truncated: too small ⇒ -EINVAL (returning 0 would read
     * as "directory exhausted", a lie; half a record violates the FS contract with -EIO).
     * vfs always passes sizeof(br_dirent_t). */
    if ((buf == BR_NULL) || (n < (br_size_t)sizeof(br_dirent_t))) {
        return BR_ERR(EINVAL);
    }

    /* "." and ".." are not real entries; enumeration **synthesizes** them (matching lookup) */
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

    /* Children: **re-walk by cursor** instead of caching a next pointer in the iterator —
     * rename/unlink change the list and a cached pointer would dangle; max 48 nodes, so the
     * re-walk is negligible (correctness first). */
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

/* =====================================================================
 * Layer 2 implementation: inode ops (each slot checks the node type)
 * ===================================================================== */

/*
 * lookup: **allocates nothing** (the opposite of littlefs's "transient path-prefix wrapper"
 * — see the file header).
 * "." / ".." resolve inline: root's parent is root, so "/.." == "/" (POSIX, and root needs
 * no special case). The return type is an inode pointer with no error channel ⇒ "not found"
 * and "wrong table (non-dir)" both collapse to BR_NULL; vfs's walk_to uses BR_NULL to tell
 * "does not exist (-ENOENT)" from "failure".
 */
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
        /* O_CREAT without O_EXCL hitting an existing name: reuse (Linux). vfs's walk already
         * looked up once; checking again makes create self-consistent — the semantics hold at
         * both layers, neither alone is lacking.
         * A `SYMLINK` cannot reach here: vfs's walk **expands** a final link before create
         * (dangling links too, so the target is created), and `O_NOFOLLOW` is stopped earlier
         * with -ELOOP (ADR-0013). */
        if (ex->head.type != BR_INODE_FILE) {
            return BR_ERR(EISDIR);     /* create makes files only; dirs go through mkdir */
        }
        *out = &ex->head;
        return 0;
    }

    /* A file always has a body (empty, grows on demand) — see the tmpfs_body invariant. */
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
    /* Mutating names follow the full rules: "." / ".." / contains '/' / empty / too long ⇒
     * -EINVAL (Linux returns -EISDIR for unlink("."); here we match rmdir/rename). */
    if (tmpfs_name_ok(name) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }
    d = tmpfs_ino_node(dir);
    victim = tmpfs_child_find(d, name);
    if (victim == BR_NULL) {
        return BR_ERR(ENOENT);
    }
    /* ★ ADR-0013: a symlink is also a "file-class" entry — `unlink` removes **the link
     *   itself** (POSIX; whether the target is alive is irrelevant). Only dirs need rmdir. */
    if ((victim->head.type != BR_INODE_FILE) && (victim->head.type != BR_INODE_SYMLINK)) {
        return BR_ERR(EISDIR);         /* dirs need rmdir (tree removal is the caller's) */
    }
    /* ★ Hard link semantics: this removes **one name**. The body's fate is the refcount's —
     *   while another name points at it (refs > 1) the data must stay, or that name would
     *   read freed memory. */
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
    /* out = BR_NULL: mkdir only answers rc (vfs looks up the inode itself, see br_vfs.c) */
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
        /* "Non-empty directory" once used -EBUSY when the SD-10 subset lacked a code, but
         * that means "mount point not removable" (TC-VFS-012) ⇒ code collision. The core
         * header now has -ENOTEMPTY (number 39); use it — debugging and runtime/posix errno
         * mapping both need the distinction. */
        return BR_ERR(ENOTEMPTY);
    }
    /* The root **cannot** be rmdir'd: it has no entry in any parent (no parent), so no name
     * points at it; "." / ".." are already rejected by the name rules ⇒ "no removing root"
     * needs no special code. */
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
    /* The target dir must be **this plugin's** node: child/next/parent are tmpfs's private
     * tail, and linking another FS's node into our list interprets memory with the wrong
     * layout. vfs already stops cross-mount rename (-ENOTSUP); this is the second guard for
     * "only this FS interprets the private tail" (and equivalence across future tmpfs mounts). */
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
        /* v1 does not "overwrite an existing target" (Linux overwrites files and rejects
         * non-empty dirs): an existing target is -EEXIST, one code covering every case; it
         * also makes a same-name rename a caller error (the POSIX no-op is not needed here
         * and would hide script bugs). */
        return BR_ERR(EEXIST);
    }
    /* Refuse moving a directory into its own subtree: that makes a cycle a→…→a and
     * **no** walk would terminate (lookup("..") and child chains interlock). Linux reports
     * -EINVAL here too. Ascending costs ≤ node count. */
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

    /* getattr reads attributes and ignores the input's valid (that is setattr's input).
     * `nlink` is an ADR-0013 field: for files/symlinks = the body's refcount (hard link
     * count); directories report 1 (no hard links for dirs, and unlike Linux no subdir
     * count — see ADR-0013 §4). */
    st->valid = BR_STAT_SIZE | BR_STAT_NLINK;
    st->type  = ino->type;
    st->size  = (ino->type == BR_INODE_DIR) ? 0u : n->body->size;
    st->nlink = (n->body != BR_NULL) ? n->body->refs : 1u;
    return 0;
}

/*
 * ===================================================================== links (ADR-0013)
 *
 * All three slots are **layer 2** (namespace), level with create/unlink — a link adds
 * another name to an inode, it does not touch file content. Symlink **resolution** is not
 * here (vfs-core does it, see br_vfs.h): this plugin only answers "what is the target
 * string" and "create one for me".
 */

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
    /* The target string's existence is **not checked** (dangling links are legal), only the
     * length bound — scan to NUL or the window end, so an unterminated string is read for at
     * most BR_SYMLINK_MAX bytes (same discipline as tmpfs_name_ok). */
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
    /* The target string lives in the data body (see tmpfs_body: a link target is "a little
     * data"). Going through tmpfs_file_reserve **shares the quota accounting** — the target
     * occupies the heap and counts against the bound. */
    rc = tmpfs_file_reserve(body, tlen + 1u);
    if (rc == BR_OK) {
        body->size = (br_u64)tlen;
        tmpfs_copy(body->data, target, (br_size_t)tlen + 1u);   /* store the trailing NUL too,
                                                                 * so readlink gets a C string */
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

/* Read the link's own target string (no follow — vfs follows). Returns the full length (even when truncated, per POSIX). */
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
        br_size_t cnt = (len < cap) ? len : (cap - 1u);   /* leave a byte for NUL (caller convenience) */
        tmpfs_copy(buf, n->body->data, cnt);
        buf[cnt] = '\0';
    }
    return (int)len;
}

/* Create a hard link: new name → **the same data body** (refcount +1), sharing data/length/offset. */
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
    /* Directories take no hard links (would make a parent/child cycle); POSIX says EPERM —
     * vfs already stops it earlier; this is the second guard, covering direct callers beyond
     * vfs (self-tests are one). */
    if (target->type == BR_INODE_DIR) {
        return BR_ERR(EPERM);
    }
    /* The target must be **this plugin's** node: only this FS interprets the private tail (as in rename). */
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
    /* v1 honors only size (truncate). Unsupported bits **must** report -ENOTSUP rather than
     * pretend success — pretending makes the caller think the attribute took effect (far more
     * dangerous than refusing). */
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

/* =====================================================================
 * Layer 1 implementation: super (mount / free_inode)
 * ===================================================================== */

/* Mount rollback: the tree can only be "root + just-pre-created dirs" (depth 1, no file
 * data), so no generic recursive free; a generic one (for a future unmount) is v2 — the
 * unmount slot is BR_NULL in v1 anyway. */
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
    static const char *const pre[] = { "dev", "data", "tmp" };   /* pre-created dirs, design 7-03 §6 */
    tmpfs_node_t *r;
    br_size_t     i;
    int           rc = 0;

    if ((cfg == BR_NULL) || (root == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    *root = BR_NULL;

    /* The mount point is **not** validated here (deliberately unlike fs/devfs): tmpfs nodes
     * are path-independent — no devfs-style "node prefix = mount point" coupling, so mounting
     * elsewhere is fully consistent (v2 runtime mounts will use that). v1's "mount / only" is
     * guaranteed by the manifest mount plan and topology order; validation is composition-time. */

    /* There is exactly one accounting block (node/inode ops take no fs_priv, so they must use
     * file-scope statics), hence vfs must hand back **that** one. Two access paths pointing at
     * different objects would make logs lie — better an error than silence. */
    if (fs_priv != (void *)&s_fs) {
        return BR_ERR(EINVAL);
    }

    /* mount is **one-shot** (a second registration on the same path is -EEXIST in vfs) ⇒
     * zeroing the books means "only one tree from here". If v2 supports runtime mount/unmount,
     * this must be replaced by an unmount that frees the whole tree. */
    s_fs.nodes_used = 0u;
    s_fs.data_used  = 0u;

    r = (tmpfs_node_t *)br_malloc(sizeof *r);
    if (r == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    tmpfs_node_init(r, BR_INODE_DIR, r, "/", BR_NULL);   /* root: parent = self, dir has no body */
    s_fs.nodes_used = 1u;

    /* Pre-create /dev /data /tmp through the **same** mkdir path (one implementation of name
     * checks, node quota and linking). This is also what lets fs/devfs mount /dev (the mount
     * point exists, so vfs does not auto-mkdir). */
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

/*
 * free_inode — **no-op**, the key shape difference from littlefs/devfs:
 * vfs passes only **transient** inodes to free_inode (see br_vfs.c's ino_put: the mount
 * table's root inode and temporary walk products are kept clearly apart). tmpfs nodes are
 * explicitly allocated, persistent objects — lookup() returns the node on the list itself,
 * with no "discard after use" product. Nodes are deleted/changed only via
 * unlink/rmdir/rename (that is where br_free happens). So a real br_free here would tear
 * down the live directory tree.
 */
static void tmpfs_free_inode(br_inode_t *ino)
{
    (void)ino;
}

/* =====================================================================
 * Lifecycle implementation
 * ===================================================================== */

/* EARLY phase: the heap is not claimed yet (`br_mem_init` builds the pool in core.init) and
 * there are no threads — this phase allows only "declarations", no allocation. tmpfs has no
 * static declaration to make, so it returns empty-handed. */
int tmpfs_early_init(void)
{
    return 0;
}

/*
 * CORE phase: register tmpfs as a **driver** (no mounting).
 *
 * ★ ADR-0030: **the APP decides mounting**. This file only says "I am tmpfs, these are my
 *   ops"; the APP mounts the root with POSIX `mount(NULL, "/", "tmpfs", 0, NULL)`.
 *   So "where it is mounted" is no longer hard-coded in the FS — one tmpfs can go anywhere.
 *
 * init itself **builds no tree**: creating the root inode and pre-created dirs is the super
 * layer's mount slot, called by `br_vfs_mount` after it finds the driver and checks the
 * parameters (per contract: build cfg → ops->mount → record the slot). Here only "register
 * the driver"; failures are passed up as the negative errno (first failure halts, run by the
 * plugin manager).
 */
int tmpfs_init(void)
{
    const int rc = br_fs_driver_register("tmpfs", &s_fs_ops, (void *)&s_fs);

    if (rc != 0) {
        br_log_error("tmpfs: register driver failed (%d)", rc);
        return rc;
    }
    return 0;
}

/* START phase: one usage summary line (boot evidence, not a criterion). Numbers: nodes =
 * allocated/bound; data = sum of **capacities** requested from the heap/bound (not the sum of
 * file lengths, see the s_fs accounting note). */
int tmpfs_start(void)
{
    br_log_info("tmpfs: usage nodes %u/%u, data %u/%u B",
                s_fs.nodes_used, TMPFS_NODE_MAX, s_fs.data_used, TMPFS_DATA_MAX);
    return 0;
}
