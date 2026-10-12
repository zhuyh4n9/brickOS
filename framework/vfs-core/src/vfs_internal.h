/*
 * brickOS prototype v0.2.0 — vfs-core **plugin-private** internal contract (not exported, not in include/)
 *
 * Audience: this plugin's own `src/vfs_selftest.c`. Living in `src/` (not `include/`) is deliberate:
 * everything under `include/` is the **plugin's public surface** (governed by `brickie check`
 * interface domains, and visible to `descriptor_include`), while this header is needed **only by
 * the self-test**.
 *
 * ## Why the self-test reads internals
 *   Some criteria test **the internal mechanism itself**, which the public API cannot express:
 *     - TC-VFS-002 asserts "every mount point's path longest-prefix-matches itself" — exactly
 *       `mount_match` semantics. Testing via `br_open` only covers "which FS a path lands on",
 *       not iteration over the mount table;
 *     - the same case also inspects `s_mounts[i]` directly (the mount table is internal layout).
 *   Exposing these as **public API** would be worse: the internal mount-table view would enter
 *   the golden interface surface, so later layout changes become interface changes.
 *
 * ## Boundary discipline (same origin as core's `*_internal.h`)
 *   Only what the self-test really uses is exposed here, each item documented below. The rest of
 *   the production code (`br_vfs.c`) stays `static` — the file is not opened up for tests.
 */
#ifndef BR_VFS_CORE_INTERNAL_H
#define BR_VFS_CORE_INTERNAL_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

/* Upper bound of the normalized-path buffer (production's `BR_VFS_PATH_MAX`). The self-test
 * builds path buffers, so it needs it; only the **capacity constant** is exposed, not the buffer
 * itself, so there is no shared mutable state.
 * ★ ADR-0013 raised it from 64 to 128 (symlink concatenation must fit) — it **must match the
 *   same-named constant in `br_vfs.c` exactly**: the internal view below is reinterpreted by
 *   layout equivalence, so a mismatch makes the self-test read misaligned bytes (seen in practice:
 *   changing only production turned TC-VFS-002 red, showing "all mounts point to /").
 *   Production's `_Static_assert(sizeof(vfs_mount_t) == sizeof(...))` now enforces this. */
#define BR_VFS_PATH_MAX_INTERNAL  128u

/* Memory compare (production's static vfs_mem_eq). The self-test uses it to check "bytes read
 * back equal the bytes written" — no libc memcmp is available (plugins do not borrow symbols
 * from core/string.c). */
br_bool br_vfs_internal_mem_eq(const void *a, const void *b, br_size_t n);

/* Internal view of one mount. ★ Must match the definition in `br_vfs.c` **field for field** —
 * it is a second declaration of the same type (production does not need it, hence not in the
 * public header).
 * Use: TC-VFS-002 iterates the mount table and checks "path → match result = itself". */
typedef struct vfs_mount_internal {
    br_bool            used;
    char               path[BR_VFS_PATH_MAX_INTERNAL];
    br_u32             len;
    const br_fs_ops_t *ops;
    void              *priv;
    br_inode_t        *root;
} vfs_mount_internal_t;

/* Mount table snapshot (for the self-test). `*n` = number of mounts. */
const vfs_mount_internal_t *br_vfs_internal_mounts(br_u32 *n);

/* Longest-prefix match (for the self-test): returns the matching mount index, or (br_u32)-1.
 * ★ Forwards directly to the production implementation — negative and positive cases must run
 *   the **real algorithm**, else the case only verifies a copy. */
br_u32 br_vfs_internal_mount_match(const char *norm);

/* Path normalization (for the self-test): asserts each expectation on `norm` (fold '//', strip
 * trailing '/', reject relative). */
int br_vfs_internal_path_norm(const char *in, char *out, br_size_t cap);

/* String compare (for the self-test; production implementation is static vfs_str_eq). */
br_bool br_vfs_internal_str_eq(const char *a, const char *b);

#endif /* BR_VFS_CORE_INTERNAL_H */
