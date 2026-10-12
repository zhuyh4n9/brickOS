/*
 * brickOS prototype v0.2.0 — VFS core (framework/vfs-core)
 *
 * Design basis: `docs/7-storage/7-01-vfs.md` (§1 unified openable model SD-1/D21, §2 mount and ops
 * four-layer split SD-15/D23, §3 poll staging SD-7), `docs/8-device/8-01-device.md` §2 (open_file hook's
 * {fops, fpriv}), `docs/3-os-core/3-04-memory.md` (handles come from the core heap).
 * Contract: `include/br/vfs/br_vfs.h`; per-item rulings and deviations: `docs/decisions/0009-vfs-storage-stack.md`.
 *
 * ## Everything this file implements
 *   (1) mount table + longest-prefix match (§1's single route: `br_open` only walks this table);
 *   (2) path normalization + component-by-component lookup walk chain (§2 "path walking lives in vfs-core")
 *       + lifetime management of transient inodes (v1 has no cache; walk and discard — SD-3);
 *   (3) file handle (`br_file_t`) alloc/free + layer-3 dispatch (layers 1/2/4 ops tables are filled by the FS);
 *   (4) directory iteration (generic: reads fixed-size dirents through the file-side `read`);
 *   (5) path-level convenience surface (stat/mkdir/rmdir/unlink/rename/truncate — runtime/posix's 1:1 map);
 *   (6) **no** conformance cases: the storage-domain suite (TC-VFS-*) moved to `src/vfs_selftest.c`,
 *       driven by the core self-test pass (ADR-0010; test entry points are not in [[export]]).
 *
 * ## What this file does **not** do (D21's "pure VFS")
 *   Zero device knowledge: no `br_cdev`/`br_dev`/device-name branches — devices appear via fs/devfs
 *   as file nodes, so vfs only sees "some FS's inode carrying fops/fpriv". This is the shape after
 *   D21 revoked "vfs-core depends on dev-core".
 *
 * ## inode ownership rules (the one easy place to get wrong, so they are at the top of the file)
 *   - **the root inode held by the mount table is not transient**: vfs never hands it to `free_inode`;
 *   - inodes returned by `lookup`/`create` **are transient**: whoever gets one owns it — intermediate
 *     walk levels are discarded after use, the final inode goes to `br_file_t` and is freed by `br_file_close`;
 *   - "use and go" paths like `br_stat`/`br_mkdir`/… release the final inode **immediately**.
 *   The whole file converges this rule into one `ino_put()`; every release site goes through it —
 *   there is no second decision point.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "vfs_internal.h"      /* internal view for self-test (see that header; production paths do not use it) */

/* lifecycle hook prototypes (name = symbol_prefix + phase; referenced by generated `plugin_desc.c`; not in [[export]]) */
int vfs_core_early_init(void);
int vfs_core_init(void);
int vfs_core_start(void);

/* ==================================================================== static limits */

#define BR_VFS_MAX_MOUNTS    8u    /* mount table limit (design SD-4: v1 static mounts) */
#define BR_VFS_DRIVER_NAME_MAX 16u  /* driver name limit (e.g. "erofs"/"fat"); shorter than path names */
#define BR_VFS_PATH_MAX      128u  /* normalized path buffer (incl. trailing '\0'); 64 -> 128 since ADR-0013
                                    * — must fit the symlink "prefix + target + remaining components" concat */
#define BR_VFS_SYMLINK_DEPTH 8u    /* symlink expansion depth limit (exceeded ⇒ -ELOOP) */

/* Any bit outside the allowed flags ⇒ -EINVAL (silently ignoring it would hide "I asked for O_EXCL and nobody cared") */
#define BR_VFS_KNOWN_FLAGS   (BR_O_ACCMODE | BR_O_CREAT | BR_O_EXCL | \
                              BR_O_TRUNC | BR_O_APPEND | BR_O_NONBLOCK | BR_O_NOFOLLOW)

/* ==================================================================== mount table */

typedef struct vfs_mount {
    br_bool            used;
    char               path[BR_VFS_PATH_MAX];   /* normalized: no trailing '/' (except root), no "//" */
    br_u32             len;                     /* strlen(path); root = 1 */
    const br_fs_ops_t *ops;
    void              *priv;
    br_inode_t        *root;                    /* held by the mount table, **never** given to free_inode */
} vfs_mount_t;

static vfs_mount_t s_mounts[BR_VFS_MAX_MOUNTS];

/* ---- FS driver registry (ADR-0029): fstype name -> {ops, fs_priv} -------------------
 * FS plugins register themselves in the start phase; APPs mount by name via POSIX `mount(..., fstype, ...)`.
 * The name is a **contract** (the APP's [[mount]].fstype looks it up) ⇒ declared in brick.toml, checked by name in self-test. */
#define BR_VFS_MAX_DRIVERS   8u

typedef struct vfs_driver {
    br_bool             used;
    char                name[BR_VFS_DRIVER_NAME_MAX];
    const br_fs_ops_t  *ops;
    void               *priv;
} vfs_driver_t;

static vfs_driver_t s_drivers[BR_VFS_MAX_DRIVERS];
static br_u32       s_driver_n;

/* ★ layout guard (ADR-0013 addendum): `br_vfs_internal_mounts()` **casts** `s_mounts` to
 *   `vfs_mount_internal_t*` for the self-test (see src/vfs_internal.h). If the two declarations
 *   disagree, the self-test reads misaligned bytes — a real past pitfall (changing only the
 *   production path limit made TC-VFS-002 red at once, and the symptom was "all mounts point to /"),
 *   so "must match field by field" was upgraded from a comment to a compile-time assertion. */
_Static_assert(sizeof(vfs_mount_t) == sizeof(vfs_mount_internal_t),
               "vfs_mount_t and vfs_mount_internal_t layouts must match (internal view relies on the cast)");
_Static_assert(BR_VFS_PATH_MAX == BR_VFS_PATH_MAX_INTERNAL,
               "BR_VFS_PATH_MAX and BR_VFS_PATH_MAX_INTERNAL must match");
static br_u32      s_mount_n;   /* successful mount count (equals used entries in the table) */

/* ==================================================================== handle model
 * `br_file_t` is **opaque** to plugins (br_vfs.h only forward-declares it) ⇒ the layout lives only here;
 * plugins always go through `br_file_*` accessors (D14's "handles are opaque" + CA-5's surface budget). */
struct br_file {
    const vfs_mount_t   *m;              /* owning mount (its free_inode is used at close) */
    const br_file_ops_t *fops;
    br_inode_t          *ino;
    br_bool              ino_transient;  /* whether the final inode must be freed at close */
    void                *fpriv;
    br_s64               offset;
    br_u32               flags;
};

struct br_dir {
    br_file_t *f;
};

/* ==================================================================== small helpers */

static br_bool vfs_str_eq(const char *a, const char *b)
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

static br_size_t vfs_str_len(const char *s)
{
    br_size_t n = 0u;
    if (s == BR_NULL) {
        return 0u;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void vfs_str_copy(char *dst, const char *src, br_size_t cap)
{
    br_size_t i = 0u;
    if (cap == 0u) {
        return;
    }
    for (; src != BR_NULL && src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

br_bool br_vfs_internal_mem_eq(const void *a, const void *b, br_size_t n)
{
    const br_u8 *x = (const br_u8 *)a;
    const br_u8 *y = (const br_u8 *)b;
    for (br_size_t i = 0u; i < n; i++) {
        if (x[i] != y[i]) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

/*
 * Path normalization (design §2 "native API accepts absolute paths only"): must be absolute; collapse
 * repeated '/'; strip trailing '/' (except root). **Does not resolve "." / ".."** — that is FS lookup
 * semantics (each FS answers "parent of root" differently, e.g. devfs's ".." is itself), vfs does not decide it.
 * Returns 0 / `-EINVAL` (relative or empty) / `-ENOSPC` (buffer exceeded).
 */
static int path_norm(const char *in, char *out, br_size_t cap)
{
    if (in == BR_NULL || in[0] != '/') {
        return BR_ERR(EINVAL);
    }

    br_size_t n = 0u;
    br_bool   prev_slash = BR_FALSE;

    for (br_size_t i = 0u; in[i] != '\0'; i++) {
        if (in[i] == '/') {
            if (prev_slash == BR_TRUE) {
                continue;               /* "//" ⇒ "/" */
            }
            prev_slash = BR_TRUE;
        } else {
            prev_slash = BR_FALSE;
        }
        if (n + 1u >= cap) {
            return BR_ERR(ENOSPC);
        }
        out[n++] = in[i];
    }

    while (n > 1u && out[n - 1u] == '/') {
        n--;                            /* strip trailing '/'; keep "/" */
    }
    out[n] = '\0';
    return BR_OK;
}

/* mount-point prefix compare (**component boundary**, not a bare string prefix — otherwise "/devx" would mismatch "/dev") */
static br_bool mount_prefix_eq(const vfs_mount_t *m, const char *norm)
{
    if (m->len == 1u) {
        return BR_TRUE;                 /* root mount matches everything */
    }
    for (br_u32 i = 0u; i < m->len; i++) {
        if (norm[i] == '\0' || norm[i] != m->path[i]) {
            return BR_FALSE;
        }
    }
    return (norm[m->len] == '\0' || norm[m->len] == '/') ? BR_TRUE : BR_FALSE;
}

/* **longest-prefix match** (design §1: "/data/xxx" -> littlefs, not rootfs) */
static const vfs_mount_t *mount_match(const char *norm)
{
    const vfs_mount_t *best = BR_NULL;

    for (br_u32 i = 0u; i < s_mount_n; i++) {
        const vfs_mount_t *m = &s_mounts[i];
        if (m->used == BR_FALSE || mount_prefix_eq(m, norm) == BR_FALSE) {
            continue;
        }
        if (best == BR_NULL || m->len > best->len) {
            best = m;
        }
    }
    return best;
}

/* path relative to the mount point (**no leading '/'**; returns "" when the target is the mount point) */
static const char *mount_rest(const vfs_mount_t *m, const char *norm)
{
    if (m->len == 1u) {
        return (norm[1] == '\0') ? "" : (norm + 1);
    }
    if (norm[m->len] == '\0') {
        return "";
    }
    return norm + m->len + 1u;
}

/* the **only** entry for releasing transient inodes (see the ownership rules at the file top) */
static void ino_put(const vfs_mount_t *m, br_inode_t *ino, br_bool is_root)
{
    if (ino == BR_NULL || is_root == BR_TRUE) {
        return;                          /* the mount table's root inode is not transient */
    }
    if (m->ops->free_inode != BR_NULL) {
        m->ops->free_inode(ino);
    }
}

/* ==================================================================== walk chain (SD-3, ADR-0013 adds symlinks)
 *
 * **Who resolves symlinks**: vfs-core (not the FS). The target string may be an absolute path (must
 * restart mount-table matching) or a relative path (relative to "the directory containing the link");
 * only vfs, which owns the mount table and the full path, can do both. The FS only provides
 * `readlink` (supplies the target string) and `symlink`/`link` (creates links).
 *
 * **Expansion restarts instead of recursing**: this function is called by APP threads with a 4 KiB
 * stack; 8 levels of recursion would blow it. Restarting costs "walk again from the mount root",
 * negligible at v1's single-APP file scale (SD-3 already logged the "no cache" performance ceiling as R-S3).
 *
 * **`..` does not cross mounts**: the walk always descends **within one mount** (`mount_match` picks
 * the mount, `lookup` resolves `..`), so `..` inside a relative target stops at the mount root — a
 * deliberate v1 boundary (not Linux semantics), logged in ADR-0013 §2.4.
 */

/* push one char into the concat buffer (single length enforcement point ⇒ overlong is always -ENAMETOOLONG). */
static int np_push(char *np, br_size_t *n, char c)
{
    if (*n + 1u >= (br_size_t)BR_VFS_PATH_MAX) {
        return BR_ERR(ENAMETOOLONG);
    }
    np[(*n)++] = c;
    return BR_OK;
}

/*
 * Resolve a **normalized** absolute path starting from the mount table, returning the final inode.
 * `*is_root_out` = whether the final level is "the mount table's root" (the caller decides if it can free).
 * `nofollow_last` = do **not** expand when the final level is a symlink (used by stat/readlink/link/unlink).
 * Errors: `-ENOENT` (miss) / `-ENOTDIR` (intermediate level not a directory) / `-ENOSPC` (component over limit) /
 *       `-ENAMETOOLONG` (expansion exceeds the path buffer) / `-ELOOP` (expansion depth over limit) / readlink errors.
 *
 * ★ Looking up at every level (no caching) is a direct consequence of SD-3: v1 **has inode ops, no inode cache**.
 */
static int resolve_norm(const char *norm_in, br_bool nofollow_last,
                        const vfs_mount_t **m_out, br_inode_t **ino_out, br_bool *is_root_out,
                        char *final_out, br_size_t final_cap)
{
    char path[BR_VFS_PATH_MAX];
    vfs_str_copy(path, norm_in, (br_size_t)sizeof(path));

    for (br_u32 depth = 0u; depth <= (br_u32)BR_VFS_SYMLINK_DEPTH; depth++) {
        /* Record "which path this walk is actually on" (it changes after expansion). Callers of
         * `-ENOENT` need it: when `O_CREAT` hits a dangling link, what must be created is the **target**,
         * not the link name (ADR-0013 §2.5). */
        if (final_out != BR_NULL && final_cap > 0u) {
            vfs_str_copy(final_out, path, final_cap);
        }
        const vfs_mount_t *m = mount_match(path);
        if (m == BR_NULL) {
            return BR_ERR(ENOENT);
        }

        const char   *rest     = mount_rest(m, path);
        const br_size_t rest_off = (br_size_t)(rest - path);
        br_inode_t   *cur      = m->root;
        br_bool       cur_is_root = BR_TRUE;
        br_size_t     i        = 0u;
        br_bool       expanded = BR_FALSE;

        while (rest[i] != '\0') {
            br_size_t j = i;
            while (rest[j] != '\0' && rest[j] != '/') {
                j++;
            }
            const br_bool   last = (rest[j] == '\0') ? BR_TRUE : BR_FALSE;
            const br_size_t clen = j - i;

            /* ★ Use `type` to decide "is a directory", **not** `iops == NULL`:
             *   whether iops is null is each FS's **shape convention** (tmpfs shares one table with a
             *   lookup slot across all nodes), while type is the semantic fact. Judging semantics from
             *   a shape convention ⇒ the same mistake yields different errno per FS (walking a file as
             *   a directory would become -ENOENT instead of -ENOTDIR). */
            if (cur->type != BR_INODE_DIR || cur->iops == BR_NULL || cur->iops->lookup == BR_NULL) {
                ino_put(m, cur, cur_is_root);
                return BR_ERR(ENOTDIR);
            }
            if (clen == 0u || clen >= (br_size_t)BR_NAME_MAX) {
                ino_put(m, cur, cur_is_root);
                return BR_ERR(ENOSPC);
            }

            char comp[BR_NAME_MAX];
            for (br_size_t k = 0u; k < clen; k++) {
                comp[k] = rest[i + k];
            }
            comp[clen] = '\0';

            br_inode_t *next = cur->iops->lookup(cur, comp);
            if (next == BR_NULL) {
                ino_put(m, cur, cur_is_root);
                return BR_ERR(ENOENT);
            }

            /* ---- symlink expansion (skipped when the final level is nofollow) ---- */
            if ((next->type == BR_INODE_SYMLINK) &&
                !((last == BR_TRUE) && (nofollow_last == BR_TRUE))) {
                char tgt[BR_SYMLINK_MAX + 1u];
                int  rl = BR_ERR(ENOTSUP);

                if (next->iops != BR_NULL && next->iops->readlink != BR_NULL) {
                    rl = next->iops->readlink(next, tgt, (br_size_t)BR_SYMLINK_MAX);
                }
                ino_put(m, next, BR_FALSE);
                if (rl < 0) {
                    ino_put(m, cur, cur_is_root);
                    return rl;
                }
                if ((br_size_t)rl > (br_size_t)BR_SYMLINK_MAX) {
                    rl = (int)BR_SYMLINK_MAX;          /* contract: truncate silently, return the full length */
                }
                tgt[rl] = '\0';

                char      np[BR_VFS_PATH_MAX];
                br_size_t n  = 0u;
                int       pr = BR_OK;

                if (tgt[0] != '/') {
                    /* relative target: prefix = path of the directory **containing** the link
                     * (when i == 0 it is the mount path itself; otherwise the segment before '/') */
                    const br_size_t plen = (i == 0u) ? (br_size_t)m->len : (rest_off + i - 1u);
                    for (br_size_t k = 0u; k < plen && pr == BR_OK; k++) {
                        pr = np_push(np, &n, path[k]);
                    }
                    if (pr == BR_OK && (plen == 0u || path[plen - 1u] != '/')) {
                        pr = np_push(np, &n, '/');
                    }
                }
                for (int k = 0; (k < rl) && (pr == BR_OK); k++) {
                    pr = np_push(np, &n, tgt[k]);
                }
                for (br_size_t k = j; (rest[k] != '\0') && (pr == BR_OK); k++) {
                    pr = np_push(np, &n, rest[k]);      /* remaining components (carry their own leading '/') */
                }
                if (pr == BR_OK) {
                    np[n] = '\0';
                    pr = path_norm(np, path, (br_size_t)sizeof(path));
                    if (pr != BR_OK) {
                        pr = BR_ERR(ENAMETOOLONG);   /* over buffer after concat ⇒ semantically name too long */
                    }
                }

                ino_put(m, cur, cur_is_root);
                if (pr != BR_OK) {
                    return pr;
                }
                expanded = BR_TRUE;
                break;                                  /* restart the walk */
            }

            ino_put(m, cur, cur_is_root);              /* intermediate levels discarded after use */
            cur         = next;
            cur_is_root = BR_FALSE;

            if (last == BR_TRUE) {
                break;
            }
            i = j + 1u;
        }

        if (expanded == BR_TRUE) {
            continue;                                   /* retry with a different path */
        }
        *m_out       = m;
        *ino_out     = cur;
        *is_root_out = cur_is_root;
        return BR_OK;
    }

    return BR_ERR(ELOOP);
}

/*
 * Resolve "parent directory inode + final name" (shared by mkdir/unlink/rmdir/rename/create/symlink/link).
 *   `buf`/`cap`: caller-provided **writable** buffer (the final name is copied into it; `*leaf_out` points to it).
 * Intermediate components **follow** symlinks (parent is a link ⇒ lands in the target directory) — POSIX semantics.
 * Errors: `-EINVAL` (illegal path) / `-EBUSY` (target is the mount point itself: there is no "final name") /
 *       `-ENOENT` (parent missing) / `-ENOTDIR` (a non-directory on the parent path) / `-ENOSPC` (name too long).
 */
static int resolve_parent(const char *path,
                          char *buf, br_size_t cap,
                          const vfs_mount_t **m_out,
                          br_inode_t **parent_out, br_bool *parent_is_root_out,
                          char **leaf_out)
{
    char norm[BR_VFS_PATH_MAX];
    int  rc = path_norm(path, norm, (br_size_t)sizeof(norm));
    if (rc != BR_OK) {
        return rc;
    }

    const vfs_mount_t *m = mount_match(norm);
    if (m == BR_NULL) {
        return BR_ERR(ENOENT);
    }

    /* Target is the **mount point itself** ⇒ there is no operable "final name".
     * Distinguish mount root from subdirectory: "/dev/x" has final name x (legal), "/dev" is a mount point (illegal). */
    const char *rest = mount_rest(m, norm);
    if (rest[0] == '\0') {
        return BR_ERR(EBUSY);
    }

    const br_size_t rest_off = (br_size_t)(rest - norm);
    br_size_t slash = 0u;
    br_bool   found = BR_FALSE;
    for (br_size_t k = 0u; rest[k] != '\0'; k++) {
        if (rest[k] == '/') {
            slash = k;
            found = BR_TRUE;
        }
    }

    char        parent[BR_VFS_PATH_MAX];
    br_size_t   plen     = 0u;
    const char *leaf_src = BR_NULL;
    br_size_t   llen     = 0u;

    if (found == BR_FALSE) {
        plen     = (br_size_t)m->len;               /* parent = mount path itself */
        leaf_src = rest;
    } else {
        plen     = rest_off + slash;                /* parent = segment before the last '/' */
        leaf_src = rest + slash + 1u;
    }
    for (br_size_t k = 0u; k < plen; k++) {
        parent[k] = norm[k];
    }
    parent[plen] = '\0';
    llen = vfs_str_len(leaf_src);

    rc = resolve_norm(parent, BR_FALSE, m_out, parent_out, parent_is_root_out, BR_NULL, 0u);
    if (rc != BR_OK) {
        return rc;
    }

    if (llen == 0u || llen >= (br_size_t)BR_NAME_MAX) {
        ino_put(*m_out, *parent_out, *parent_is_root_out);
        return BR_ERR(ENOSPC);
    }
    if (llen + 1u > cap) {
        ino_put(*m_out, *parent_out, *parent_is_root_out);
        return BR_ERR(ENOSPC);
    }
    for (br_size_t k = 0u; k < llen; k++) {
        buf[k] = leaf_src[k];
    }
    buf[llen] = '\0';
    *leaf_out = buf;
    return BR_OK;
}

/* resolve to the "final inode" (used by stat/lstat/truncate/open/link/readlink) */
static int resolve_leaf(const char *path, br_bool nofollow_last,
                        const vfs_mount_t **m_out, br_inode_t **ino_out, br_bool *is_root_out,
                        char *final_out, br_size_t final_cap)
{
    char norm[BR_VFS_PATH_MAX];
    int  rc = path_norm(path, norm, (br_size_t)sizeof(norm));
    if (rc != BR_OK) {
        return rc;
    }
    return resolve_norm(norm, nofollow_last, m_out, ino_out, is_root_out, final_out, final_cap);
}

/* ==================================================================== auto-create mount point directories
 * Design `7-03` §6: "**auto mkdir the mount point when missing in the parent FS**" — static composition
 * convenience beats explicit mkdir ceremony (the manifest already validated it). Here directories are
 * created **level by level** (`/a/b` creates `/a` first).
 *
 * Since ADR-0013 resolution goes level by level over **prefixes of the normalized full path** (rather
 * than "descending rest from the parent mount root"): this also expands symlinks on intermediate
 * components (a previous mount point may be a link). The mount point itself is **not yet registered**,
 * so `mount_match` hits the parent mount — exactly the one we want it to land in. */
static int mkdir_p(const char *full_norm)
{
    br_size_t i = 0u;
    while (full_norm[i] == '/') {
        i++;                                            /* skip the root '/' */
    }

    while (full_norm[i] != '\0') {
        br_size_t j = i;
        while (full_norm[j] != '\0' && full_norm[j] != '/') {
            j++;
        }

        char pfx[BR_VFS_PATH_MAX];
        for (br_size_t k = 0u; k < j; k++) {
            pfx[k] = full_norm[k];
        }
        pfx[j] = '\0';

        const vfs_mount_t *mm = BR_NULL;
        br_inode_t        *ino = BR_NULL;
        br_bool            ir = BR_FALSE;
        int rc = resolve_norm(pfx, BR_FALSE, &mm, &ino, &ir, BR_NULL, 0u);

        if (rc == BR_ERR(ENOENT)) {
            char        pbuf[BR_VFS_PATH_MAX];
            const vfs_mount_t *pm = BR_NULL;
            br_inode_t *parent = BR_NULL;
            br_bool     pir = BR_FALSE;
            char       *leaf = BR_NULL;

            rc = resolve_parent(pfx, pbuf, (br_size_t)sizeof(pbuf), &pm, &parent, &pir, &leaf);
            if (rc == BR_OK) {
                if (parent->type != BR_INODE_DIR) {
                    rc = BR_ERR(ENOTDIR);
                } else if (parent->iops == BR_NULL || parent->iops->mkdir == BR_NULL) {
                    rc = BR_ERR(ENOTSUP);
                } else {
                    rc = parent->iops->mkdir(parent, leaf);
                    if (rc == BR_ERR(EEXIST)) {
                        rc = BR_OK;                     /* race/already exists ⇒ not an error (original semantics) */
                    }
                }
                ino_put(pm, parent, pir);
            }
        } else if (rc == BR_OK) {
            if (ino->type != BR_INODE_DIR) {
                rc = BR_ERR(ENOTDIR);
            }
            ino_put(mm, ino, ir);
        }

        if (rc != BR_OK) {
            return rc;
        }
        if (full_norm[j] == '\0') {
            break;
        }
        i = j + 1u;
    }
    return BR_OK;
}

/* ==================================================================== layer 1: mount registration */

int br_mount_register(const char *path, const br_fs_ops_t *ops, void *fs_priv)
{
    if (ops == BR_NULL || ops->mount == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    char norm[BR_VFS_PATH_MAX];
    int  rc = path_norm(path, norm, (br_size_t)sizeof(norm));
    if (rc != BR_OK) {
        return rc;
    }
    if (s_mount_n >= (br_u32)BR_VFS_MAX_MOUNTS) {
        return BR_ERR(ENOSPC);
    }
    /* Duplicate mount of the same path (compared after normalization) ⇒ -EEXIST. Why not "overwrite":
     * overwriting would leave the old FS's root inode and its fs_priv without a unique owner — a silent
     * leak is worse than an error. */
    for (br_u32 i = 0u; i < s_mount_n; i++) {
        if (s_mounts[i].used == BR_TRUE && vfs_str_eq(s_mounts[i].path, norm) == BR_TRUE) {
            return BR_ERR(EEXIST);
        }
    }

    /* (1) The mount point must land in the **parent mount**; create it level by level when missing (design `7-03` §6) */
    if (vfs_str_eq(norm, "/") == BR_FALSE) {
        const vfs_mount_t *parent = mount_match(norm);
        if (parent == BR_NULL) {
            return BR_ERR(ENODEV);          /* no parent FS to host the mount point */
        }
        const char *rest = mount_rest(parent, norm);
        if (rest[0] == '\0') {
            return BR_ERR(EEXIST);          /* same path as an existing mount point (already blocked above; pure defense) */
        }
        rc = mkdir_p(norm);                    /* ADR-0013: create level by level by full-path prefix (expands links) */
        if (rc != BR_OK) {
            return rc;
        }
    }

    /* (2) FS-side mount (creates the root inode). cfg.path gives the FS its own normalized path. */
    const br_fs_cfg_t cfg = {
        .path = norm,
        .dev  = BR_NULL,     /* used by media FSes (littlefs/EROFS); tmpfs/devfs do not need it */
        .opts = BR_NULL,
    };
    br_inode_t *root = BR_NULL;
    rc = ops->mount(fs_priv, &cfg, &root);
    if (rc != BR_OK) {
        return rc;
    }
    if (root == BR_NULL) {
        return BR_ERR(EINVAL);              /* mount succeeded but returned no root inode: FS contract violation */
    }

    vfs_mount_t *m = &s_mounts[s_mount_n];
    m->used = BR_TRUE;
    vfs_str_copy(m->path, norm, (br_size_t)sizeof(m->path));
    m->len  = (br_u32)vfs_str_len(m->path);
    m->ops  = ops;
    m->priv = fs_priv;
    m->root = root;
    s_mount_n++;

    br_log_info("vfs: mount %s (root type=%u)", m->path, root->type);
    return BR_OK;
}

/* ==================================================================== FS driver registry */

int br_fs_driver_register(const char *name, const br_fs_ops_t *ops, void *fs_priv)
{
    if (name == BR_NULL || ops == BR_NULL || ops->mount == BR_NULL || name[0] == '\0') {
        return BR_ERR(EINVAL);
    }
    const br_size_t n = vfs_str_len(name);
    if (n >= (br_size_t)BR_VFS_DRIVER_NAME_MAX) {
        return BR_ERR(ENAMETOOLONG);
    }
    for (br_u32 i = 0u; i < s_driver_n; i++) {
        if (s_drivers[i].used == BR_TRUE && vfs_str_eq(s_drivers[i].name, name) == BR_TRUE) {
            return BR_ERR(EEXIST);      /* no overwrite: see br_vfs.h */
        }
    }
    if (s_driver_n >= (br_u32)BR_VFS_MAX_DRIVERS) {
        return BR_ERR(ENOSPC);
    }
    vfs_driver_t *d = &s_drivers[s_driver_n];
    d->used = BR_TRUE;
    vfs_str_copy(d->name, name, (br_size_t)sizeof(d->name));
    d->ops  = ops;
    d->priv = fs_priv;
    s_driver_n++;
    br_log_info("vfs: driver %s", d->name);
    return BR_OK;
}

br_u32 br_fs_driver_count(void)
{
    return s_driver_n;
}

const char *br_fs_driver_name_at(br_u32 index)
{
    if (index >= s_driver_n || s_drivers[index].used == BR_FALSE) {
        return BR_NULL;
    }
    return s_drivers[index].name;
}

/* the **media-carrying** form of `br_mount_register`: one extra `dev` in cfg (media FSes use it to get a bdev). */
static int mount_with_dev(const char *path, const br_fs_ops_t *ops, void *fs_priv,
                          const char *dev)
{
    /* Same body as br_mount_register plus cfg.dev — factored out so the two mount paths cannot drift. */
    if (ops == BR_NULL || ops->mount == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    char norm[BR_VFS_PATH_MAX];
    int  rc = path_norm(path, norm, (br_size_t)sizeof(norm));
    if (rc != BR_OK) {
        return rc;
    }
    if (s_mount_n >= (br_u32)BR_VFS_MAX_MOUNTS) {
        return BR_ERR(ENOSPC);
    }
    for (br_u32 i = 0u; i < s_mount_n; i++) {
        if (s_mounts[i].used == BR_TRUE && vfs_str_eq(s_mounts[i].path, norm) == BR_TRUE) {
            return BR_ERR(EEXIST);
        }
    }
    if (vfs_str_eq(norm, "/") == BR_FALSE) {
        const vfs_mount_t *parent = mount_match(norm);
        if (parent == BR_NULL) {
            return BR_ERR(ENODEV);
        }
        rc = mkdir_p(norm);
        if (rc != BR_OK) {
            return rc;
        }
    }
    /* ★ `cfg.path` must be a **long-lived** buffer, not the stack buffer `norm` here.
     *   Why: media FSes record `cfg->path` (and `cfg->dev`) in their volume table and still need it in
     *   the **self-test phase** after mount returns (e.g. `uboot-fat`'s `vol_info`/`sync`). With a stack
     *   buffer it becomes invalid the moment mount returns — the symptom is "logs at mount time look
     *   perfectly fine, yet self-test reports vol_info_failed / sync_rc=-2" (hit in practice).
     *   Fix: first write the normalized path into the **mount table slot** (lifetime = until shutdown),
     *   then point `cfg.path` at it. The slot is rolled back if mount fails (not occupied). */
    vfs_mount_t *m = &s_mounts[s_mount_n];
    vfs_str_copy(m->path, norm, (br_size_t)sizeof(m->path));
    m->len = (br_u32)vfs_str_len(m->path);

    const br_fs_cfg_t cfg = { .path = m->path, .dev = dev, .opts = BR_NULL };
    br_inode_t *root = BR_NULL;
    rc = ops->mount(fs_priv, &cfg, &root);
    if (rc != BR_OK) {
        m->path[0] = '\0';      /* rollback: leave no half-state that "looks mounted" */
        m->len = 0u;
        return rc;
    }
    if (root == BR_NULL) {
        m->path[0] = '\0';
        m->len = 0u;
        return BR_ERR(EINVAL);
    }
    m->used = BR_TRUE;
    m->ops  = ops;
    m->priv = fs_priv;
    m->root = root;
    s_mount_n++;
    br_log_info("vfs: mount %s (dev=%s root type=%u)", m->path,
                (dev != BR_NULL) ? dev : "-", root->type);
    return BR_OK;
}

int br_vfs_mount(const char *source, const char *target, const char *fstype,
                 unsigned long flags, const void *data)
{
    (void)flags;
    (void)data;                     /* reserved in v1: POSIX's last two arguments */
    if (target == BR_NULL || fstype == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    for (br_u32 i = 0u; i < s_driver_n; i++) {
        if (s_drivers[i].used == BR_TRUE && vfs_str_eq(s_drivers[i].name, fstype) == BR_TRUE) {
            return mount_with_dev(target, s_drivers[i].ops, s_drivers[i].priv, source);
        }
    }
    return BR_ERR(ENODEV);          /* no driver for this fstype (FS plugin not selected/registered) */
}

br_u32 br_mount_count(void)
{
    return s_mount_n;
}

const char *br_mount_path_at(br_u32 index)
{
    if (index >= s_mount_n || s_mounts[index].used == BR_FALSE) {
        return BR_NULL;
    }
    return s_mounts[index].path;
}

/* ==================================================================== unified entry */

br_file_t *br_open_err(const char *path, br_u32 flags, int *err)
{
    if (err != BR_NULL) {
        *err = BR_OK;
    }

    const br_u32 acc = flags & BR_O_ACCMODE;
    if (acc != BR_O_RDONLY && acc != BR_O_WRONLY && acc != BR_O_RDWR) {
        if (err != BR_NULL) {
            *err = BR_ERR(EINVAL);
        }
        return BR_NULL;
    }
    if ((flags & ~(br_u32)BR_VFS_KNOWN_FLAGS) != 0u) {
        if (err != BR_NULL) {
            *err = BR_ERR(EINVAL);
        }
        return BR_NULL;
    }

    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *ino = BR_NULL;
    br_bool            ino_is_root = BR_FALSE;
    char open_final[BR_VFS_PATH_MAX];
    int rc = resolve_leaf(path, (flags & BR_O_NOFOLLOW) != 0u, &m, &ino, &ino_is_root,
                          open_final, (br_size_t)sizeof(open_final));

    if (rc != BR_OK) {
        /* miss + O_CREAT ⇒ create on the parent directory (design §2's "miss and O_CREAT") */
        if (rc != BR_ERR(ENOENT) || (flags & BR_O_CREAT) == 0u) {
            if (err != BR_NULL) {
                *err = rc;
            }
            return BR_NULL;
        }

        char        buf[BR_VFS_PATH_MAX];
        br_inode_t *parent = BR_NULL;
        br_bool     parent_is_root = BR_FALSE;
        char       *leaf = BR_NULL;

        /* ★ Create on the **post-expansion** path, not the original one: if the final level is a
         *   **dangling symlink**, POSIX `open(link, O_CREAT)` creates the **target** (`link` itself
         *   already exists, so it cannot be created). `open_final` is written by resolve_leaf as
         *   "which path this walk actually reached". */
        rc = resolve_parent(open_final, buf, (br_size_t)sizeof(buf), &m, &parent,
                            &parent_is_root, &leaf);
        if (rc != BR_OK) {
            if (err != BR_NULL) {
                *err = rc;
            }
            return BR_NULL;
        }
        if (parent->type != BR_INODE_DIR) {
            ino_put(m, parent, parent_is_root);
            if (err != BR_NULL) {
                *err = BR_ERR(ENOTDIR);     /* "parent" is actually a file ⇒ same code as "intermediate level not a directory" */
            }
            return BR_NULL;
        }
        if (parent->iops == BR_NULL || parent->iops->create == BR_NULL) {
            ino_put(m, parent, parent_is_root);
            if (err != BR_NULL) {
                *err = BR_ERR(ENOTSUP);     /* this FS does not support create (read-only / device projection) */
            }
            return BR_NULL;
        }

        br_inode_t *created = BR_NULL;
        rc = parent->iops->create(parent, leaf, flags, &created);
        ino_put(m, parent, parent_is_root);
        if (rc != BR_OK) {
            if (err != BR_NULL) {
                *err = rc;
            }
            return BR_NULL;
        }
        ino         = created;
        ino_is_root = BR_FALSE;
    } else if ((flags & (BR_O_CREAT | BR_O_EXCL)) == (BR_O_CREAT | BR_O_EXCL)) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(EEXIST);
        }
        return BR_NULL;
    }

    /* The final level is still a symlink ⇒ only one possibility: the caller passed O_NOFOLLOW. POSIX's
     * answer is -ELOOP (**not** -ENOTSUP) — a link has no file face and should not be opened. */
    if (ino->type == BR_INODE_SYMLINK) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(ELOOP);
        }
        return BR_NULL;
    }

    /* Directories may only be opened read-only (their "file face" is the dirent stream); write open ⇒ -EISDIR.
     * Files/devices must have a file face, otherwise it is "visible but unopenable" (v2's raw flash/bdev is this). */
    if (ino->type == BR_INODE_DIR && (flags & BR_O_ACCMODE) != BR_O_RDONLY) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(EISDIR);
        }
        return BR_NULL;
    }
    if (ino->fops == BR_NULL) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(ENOTSUP);
        }
        return BR_NULL;
    }

    br_file_t *f = (br_file_t *)br_malloc((br_size_t)sizeof(br_file_t));
    if (f == BR_NULL) {
        ino_put(m, ino, ino_is_root);
        if (err != BR_NULL) {
            *err = BR_ERR(ENOMEM);
        }
        return BR_NULL;
    }
    f->m             = m;
    f->fops          = ino->fops;
    f->ino           = ino;
    f->ino_transient = (ino_is_root == BR_TRUE) ? BR_FALSE : BR_TRUE;
    f->fpriv         = ino->fpriv;      /* {fops, fpriv} carried by the inode (D21/D23) */
    f->offset        = 0;
    f->flags         = flags;

    if (f->fops->open != BR_NULL) {
        rc = f->fops->open(f, flags);
        if (rc != BR_OK) {
            ino_put(m, ino, ino_is_root);
            br_free(f);
            if (err != BR_NULL) {
                *err = rc;              /* an exclusive device's -EBUSY propagates from here (policy belongs to the driver) */
            }
            return BR_NULL;
        }
    }

    return f;
}

br_file_t *br_open(const char *path, br_u32 flags)
{
    return br_open_err(path, flags, BR_NULL);
}

/* ==================================================================== accessors */

br_inode_t *br_file_inode(br_file_t *f)
{
    return (f != BR_NULL) ? f->ino : BR_NULL;
}

void *br_file_fpriv(br_file_t *f)
{
    return (f != BR_NULL) ? f->fpriv : BR_NULL;
}

void br_file_set_fpriv(br_file_t *f, void *p)
{
    if (f != BR_NULL) {
        f->fpriv = p;
    }
}

br_u32 br_file_flags(br_file_t *f)
{
    return (f != BR_NULL) ? f->flags : 0u;
}

br_s64 br_file_offset(br_file_t *f)
{
    return (f != BR_NULL) ? f->offset : 0;
}

void br_file_set_offset(br_file_t *f, br_s64 off)
{
    if (f != BR_NULL) {
        f->offset = off;
    }
}

/* ==================================================================== layer 3 dispatch */

br_s64 br_file_read(br_file_t *f, void *buf, br_size_t n)
{
    if (f == BR_NULL || buf == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (f->fops->read == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return f->fops->read(f, buf, n);
}

br_s64 br_file_write(br_file_t *f, const void *buf, br_size_t n)
{
    if (f == BR_NULL || buf == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (f->fops->write == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return f->fops->write(f, buf, n);
}

br_s64 br_file_lseek(br_file_t *f, br_s64 off, int whence)
{
    if (f == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (f->fops->lseek == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return f->fops->lseek(f, off, whence);
}

int br_file_ioctl(br_file_t *f, br_u32 cmd, void *arg)
{
    if (f == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (f->fops->ioctl == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return f->fops->ioctl(f, cmd, arg);
}

int br_file_fsync(br_file_t *f)
{
    if (f == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (f->fops->fsync == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return f->fops->fsync(f);
}

int br_file_poll(br_file_t *f, br_u32 *events)
{
    if (f == BR_NULL || events == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (f->fops->poll == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    *events = 0u;
    return f->fops->poll(f, events);
}

int br_file_close(br_file_t *f)
{
    if (f == BR_NULL) {
        return BR_OK;                   /* idempotent: closing a null handle is not an error */
    }
    int rc = BR_OK;
    if (f->fops->close != BR_NULL) {
        rc = f->fops->close(f);
    }
    ino_put(f->m, f->ino, (f->ino_transient == BR_TRUE) ? BR_FALSE : BR_TRUE);
    br_free(f);
    return rc;
}

/* ==================================================================== directory iteration */

br_dir_t *br_opendir(const char *path)
{
    int err = BR_OK;
    br_file_t *f = br_open_err(path, BR_O_RDONLY, &err);
    if (f == BR_NULL) {
        return BR_NULL;
    }
    if (br_file_inode(f)->type != BR_INODE_DIR) {
        (void)br_file_close(f);
        return BR_NULL;
    }

    br_dir_t *d = (br_dir_t *)br_malloc((br_size_t)sizeof(br_dir_t));
    if (d == BR_NULL) {
        (void)br_file_close(f);
        return BR_NULL;
    }
    d->f = f;
    return d;
}

int br_readdir(br_dir_t *d, br_dirent_t *out)
{
    if (d == BR_NULL || d->f == BR_NULL || out == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    const br_s64 rc = br_file_read(d->f, out, (br_size_t)sizeof(*out));
    if (rc < 0) {
        return (int)rc;
    }
    if (rc == 0) {
        return 0;                       /* directory exhausted */
    }
    if (rc != (br_s64)sizeof(*out)) {
        return BR_ERR(EIO);          /* dirents are fixed-size records: a partial one is an FS contract violation */
    }
    return 1;
}

int br_closedir(br_dir_t *d)
{
    if (d == BR_NULL) {
        return BR_OK;
    }
    const int rc = br_file_close(d->f);
    br_free(d);
    return rc;
}

/* ==================================================================== path-level convenience surface
 *
 * Why this layer exists (design `7-01` §1 only wrote `br_open`/`br_file_*`): directory namespace
 * changes (create/mkdir/unlink/rename/getattr) are two-part "parent directory inode + name" in the ops
 * table, and **consumers cannot get an inode** (that is a transient walk product). So vfs must provide
 * the "path -> two-part" translation layer — runtime/posix's POSIX face is its 1:1 map (ADR-0009 §3 ruling 3). */

int br_fstat(br_file_t *f, br_stat_t *st)
{
    if (f == BR_NULL || f->ino == BR_NULL || st == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (f->ino->iops == BR_NULL || f->ino->iops->getattr == BR_NULL) {
        return BR_ERR(ENOTSUP);
    }
    return f->ino->iops->getattr(f->ino, st);
}

/* `stat` follows the final symlink, `lstat` does not (POSIX). Both follow intermediate components. */
static int stat_common(const char *path, br_stat_t *st, br_bool nofollow_last)
{
    if (st == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *ino = BR_NULL;
    br_bool            is_root = BR_FALSE;
    int rc = resolve_leaf(path, nofollow_last, &m, &ino, &is_root, BR_NULL, 0u);
    if (rc != BR_OK) {
        return rc;
    }
    if (ino->iops == BR_NULL || ino->iops->getattr == BR_NULL) {
        ino_put(m, ino, is_root);
        return BR_ERR(ENOTSUP);
    }
    rc = ino->iops->getattr(ino, st);
    ino_put(m, ino, is_root);
    return rc;
}

int br_stat(const char *path, br_stat_t *st)
{
    return stat_common(path, st, BR_FALSE);
}

int br_lstat(const char *path, br_stat_t *st)
{
    return stat_common(path, st, BR_TRUE);
}

/* Common skeleton for mkdir/rmdir/unlink/rename: resolve the parent → call the slot → release the parent inode. */
static int parent_op(const char *path, int (*fn)(br_inode_t *, const char *))
{
    char        buf[BR_VFS_PATH_MAX];
    const vfs_mount_t *m = BR_NULL;
    br_inode_t *parent = BR_NULL;
    br_bool     parent_is_root = BR_FALSE;
    char       *leaf = BR_NULL;

    int rc = resolve_parent(path, buf, (br_size_t)sizeof(buf), &m, &parent,
                            &parent_is_root, &leaf);
    if (rc != BR_OK) {
        return rc;
    }
    if (parent->type != BR_INODE_DIR) {
        ino_put(m, parent, parent_is_root);
        return BR_ERR(ENOTDIR);
    }
    if (parent->iops == BR_NULL || fn == BR_NULL) {
        ino_put(m, parent, parent_is_root);
        return BR_ERR(ENOTSUP);
    }
    rc = fn(parent, leaf);
    ino_put(m, parent, parent_is_root);
    return rc;
}

/* Three thin adapters (only to extract the function pointers from iops — avoids scattered null checks in parent_op) */
static int call_mkdir(br_inode_t *d, const char *name)
{
    return (d->iops->mkdir != BR_NULL) ? d->iops->mkdir(d, name) : BR_ERR(ENOTSUP);
}
static int call_rmdir(br_inode_t *d, const char *name)
{
    return (d->iops->rmdir != BR_NULL) ? d->iops->rmdir(d, name) : BR_ERR(ENOTSUP);
}
static int call_unlink(br_inode_t *d, const char *name)
{
    return (d->iops->unlink != BR_NULL) ? d->iops->unlink(d, name) : BR_ERR(ENOTSUP);
}

int br_mkdir(const char *path)
{
    return parent_op(path, call_mkdir);
}

int br_rmdir(const char *path)
{
    return parent_op(path, call_rmdir);
}

int br_unlink(const char *path)
{
    return parent_op(path, call_unlink);
}

int br_rename(const char *old_path, const char *new_path)
{
    char buf_old[BR_VFS_PATH_MAX];
    char buf_new[BR_VFS_PATH_MAX];
    const vfs_mount_t *m_old = BR_NULL;
    const vfs_mount_t *m_new = BR_NULL;
    br_inode_t *d_old = BR_NULL;
    br_inode_t *d_new = BR_NULL;
    br_bool     root_old = BR_FALSE;
    br_bool     root_new = BR_FALSE;
    char       *leaf_old = BR_NULL;
    char       *leaf_new = BR_NULL;

    int rc = resolve_parent(old_path, buf_old, (br_size_t)sizeof(buf_old), &m_old,
                            &d_old, &root_old, &leaf_old);
    if (rc != BR_OK) {
        return rc;
    }
    rc = resolve_parent(new_path, buf_new, (br_size_t)sizeof(buf_new), &m_new,
                        &d_new, &root_new, &leaf_new);
    if (rc != BR_OK) {
        ino_put(m_old, d_old, root_old);
        return rc;
    }

    /* Cross-mount rename: not supported in v1 (it needs "cross-FS move" semantics, which is a copy+unlink
     * combination, in v2/v3 scope). Use -ENOTSUP rather than inventing -EXDEV (not in the SD-10 subset). */
    if (m_old != m_new) {
        ino_put(m_old, d_old, root_old);
        ino_put(m_new, d_new, root_new);
        return BR_ERR(ENOTSUP);
    }
    if (d_old->type != BR_INODE_DIR || d_new->type != BR_INODE_DIR) {
        ino_put(m_old, d_old, root_old);
        ino_put(m_new, d_new, root_new);
        return BR_ERR(ENOTDIR);
    }
    if (d_old->iops == BR_NULL || d_old->iops->rename == BR_NULL) {
        ino_put(m_old, d_old, root_old);
        ino_put(m_new, d_new, root_new);
        return BR_ERR(ENOTSUP);
    }

    rc = d_old->iops->rename(d_old, leaf_old, d_new, leaf_new);
    ino_put(m_old, d_old, root_old);
    ino_put(m_new, d_new, root_new);
    return rc;
}

int br_truncate(const char *path, br_u64 size)
{
    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *ino = BR_NULL;
    br_bool            is_root = BR_FALSE;

    int rc = resolve_leaf(path, BR_FALSE, &m, &ino, &is_root, BR_NULL, 0u);   /* truncate follows links */
    if (rc != BR_OK) {
        return rc;
    }
    if (ino->iops == BR_NULL || ino->iops->setattr == BR_NULL) {
        ino_put(m, ino, is_root);
        return BR_ERR(ENOTSUP);
    }
    const br_stat_t st = { .valid = BR_STAT_SIZE, .type = ino->type, .size = size };
    rc = ino->iops->setattr(ino, &st);
    ino_put(m, ino, is_root);
    return rc;
}

/* ==================================================================== links (ADR-0013)

 * Common shape of the three entries: namespace changes ⇒ all need "parent directory + name"
 * (`resolve_parent`), so **symlinks on the parent path are expanded when creating a link** (the link
 * gets created in the directory the link points to), which is POSIX semantics.
 */

int br_symlink(const char *target, const char *path)
{
    if (target == BR_NULL || target[0] == '\0') {
        return BR_ERR(EINVAL);
    }
    /* The target's existence is **not checked** (a dangling link is legal); but its length has a static limit. */
    if (vfs_str_len(target) > (br_size_t)BR_SYMLINK_MAX) {
        return BR_ERR(ENAMETOOLONG);
    }

    char               buf[BR_VFS_PATH_MAX];
    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *parent = BR_NULL;
    br_bool            pir = BR_FALSE;
    char              *leaf = BR_NULL;

    int rc = resolve_parent(path, buf, (br_size_t)sizeof(buf), &m, &parent, &pir, &leaf);
    if (rc != BR_OK) {
        return rc;
    }
    if (parent->type != BR_INODE_DIR) {
        ino_put(m, parent, pir);
        return BR_ERR(ENOTDIR);
    }
    if (parent->iops == BR_NULL || parent->iops->symlink == BR_NULL) {
        ino_put(m, parent, pir);
        return BR_ERR(ENOTSUP);           /* this FS does not support symlinks (devfs etc.) */
    }
    rc = parent->iops->symlink(parent, leaf, target);
    ino_put(m, parent, pir);
    return rc;
}

int br_readlink(const char *path, char *buf, br_size_t cap)
{
    const vfs_mount_t *m = BR_NULL;
    br_inode_t        *ino = BR_NULL;
    br_bool            ir = BR_FALSE;

    int rc = resolve_leaf(path, BR_TRUE, &m, &ino, &ir, BR_NULL, 0u);   /* readlink does **not** follow the final level */
    if (rc != BR_OK) {
        return rc;
    }
    if (ino->type != BR_INODE_SYMLINK) {
        ino_put(m, ino, ir);
        return BR_ERR(EINVAL);            /* POSIX: not a symlink ⇒ EINVAL */
    }
    if (ino->iops == BR_NULL || ino->iops->readlink == BR_NULL) {
        ino_put(m, ino, ir);
        return BR_ERR(ENOTSUP);
    }
    rc = ino->iops->readlink(ino, buf, cap);
    ino_put(m, ino, ir);
    return rc;
}

int br_link(const char *old_path, const char *new_path)
{
    /* POSIX `link(2)`: the final level is **not** followed (link to the link itself, not to its target). */
    const vfs_mount_t *m_old = BR_NULL;
    br_inode_t        *target = BR_NULL;
    br_bool            tir = BR_FALSE;

    int rc = resolve_leaf(old_path, BR_TRUE, &m_old, &target, &tir, BR_NULL, 0u);
    if (rc != BR_OK) {
        return rc;
    }
    if (target->type == BR_INODE_DIR) {
        ino_put(m_old, target, tir);
        return BR_ERR(EPERM);             /* directories cannot be hard-linked (POSIX: EPERM) */
    }

    char               buf[BR_VFS_PATH_MAX];
    const vfs_mount_t *m_new = BR_NULL;
    br_inode_t        *parent = BR_NULL;
    br_bool            pir = BR_FALSE;
    char              *leaf = BR_NULL;

    rc = resolve_parent(new_path, buf, (br_size_t)sizeof(buf), &m_new, &parent, &pir, &leaf);
    if (rc != BR_OK) {
        ino_put(m_old, target, tir);
        return rc;
    }

    /* Cross-mount hard links do not exist (v1 has no "cross-FS inode" concept) ⇒ -EXDEV.
     * ★ This is the **first enablement point** of `EXDEV` (18) — it was always in the error-code table
     *   and only gains meaning here. */
    if (m_old != m_new) {
        ino_put(m_old, target, tir);
        ino_put(m_new, parent, pir);
        return BR_ERR(EXDEV);
    }
    if (parent->type != BR_INODE_DIR) {
        ino_put(m_old, target, tir);
        ino_put(m_new, parent, pir);
        return BR_ERR(ENOTDIR);
    }
    if (parent->iops == BR_NULL || parent->iops->link == BR_NULL) {
        ino_put(m_old, target, tir);
        ino_put(m_new, parent, pir);
        return BR_ERR(ENOTSUP);
    }

    rc = parent->iops->link(parent, leaf, target);
    ino_put(m_old, target, tir);
    ino_put(m_new, parent, pir);
    return rc;
}

/* ==================================================================== internal view for self-test
 *
 * See `src/vfs_internal.h`: these exist **only** for `vfs_selftest.c` — what they expose under test is
 * this file's own mechanism (mount table / longest-prefix match / path normalization), which the public
 * API cannot express. ★ Always **forward** to the production implementation, never copy logic: cases must
 * run the real algorithm, otherwise they verify a copy.
 */
const vfs_mount_internal_t *br_vfs_internal_mounts(br_u32 *n)
{
    if (n != BR_NULL) {
        *n = s_mount_n;
    }
    return (const vfs_mount_internal_t *)(const void *)s_mounts;
}

br_u32 br_vfs_internal_mount_match(const char *norm)
{
    const vfs_mount_t *m = mount_match(norm);
    if (m == BR_NULL) {
        return (br_u32)-1;
    }
    return (br_u32)(m - &s_mounts[0]);
}

int br_vfs_internal_path_norm(const char *in, char *out, br_size_t cap)
{
    return path_norm(in, out, cap);
}

br_bool br_vfs_internal_str_eq(const char *a, const char *b)
{
    return vfs_str_eq(a, b);
}

/* ==================================================================== lifecycle */

int vfs_core_early_init(void)
{
    return 0;   /* EARLY: the static tables are BSS; mounting waits for each FS's CORE init (heap ready by then) */
}

int vfs_core_init(void)
{
    /* vfs-core has no state of its own to initialize: it "enters" when each FS calls `br_mount_register`.
     * Deliberately no empty API (R-S4: every line added to framework code must have a consumer). */
    return 0;
}

int vfs_core_start(void)
{
    /* The START phase leaves only a one-line mount summary as boot evidence.
     * ★ The storage domain's **full suite** (TC-VFS-*) does not run here: it lives in
     *   `src/vfs_selftest.c`, driven uniformly by core's self-test pass **after all starts**
     *   (ADR-0010). ADR-0009 §2.5 once hung it on the APP's start (there was no unified self-test
     *   point then); that version was voided with ADR-0010. */
    br_log_info("vfs: %u mount(s) active", s_mount_n);
    for (br_u32 i = 0u; i < s_mount_n; i++) {
        br_log_info("vfs:   [%u] %s", i, s_mounts[i].path);
    }
    return 0;
}

