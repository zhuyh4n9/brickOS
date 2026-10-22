/*
 * vfs-core plugin-private internal contract: used only by src/vfs_selftest.c, not exported, not
 * under include/. TC-VFS-002 tests the match algorithm itself, so it reads the mount table via the
 * accessors below; these always forward to the production implementation (no duplicated logic).
 */
#ifndef BR_VFS_CORE_INTERNAL_H
#define BR_VFS_CORE_INTERNAL_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

/* Capacity of the normalized-path buffer (production's BR_VFS_PATH_MAX). Must match br_vfs.c
 * exactly: the internal view below is reinterpreted by layout equivalence. */
#define BR_VFS_PATH_MAX_INTERNAL  128u

/* Memory compare (production's static vfs_mem_eq); no libc memcmp is available to plugins. */
br_bool br_vfs_internal_mem_eq(const void *a, const void *b, br_size_t n);

/* Internal view of one mount; must match vfs_mount_t in br_vfs.c field for field.
 * Use: TC-VFS-002 iterates the mount table and checks "path -> match result = itself". */
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

/* Longest-prefix match: matching mount index, or (br_u32)-1. Forwards to the production
 * implementation, so negative and positive cases exercise the real algorithm. */
br_u32 br_vfs_internal_mount_match(const char *norm);

/* Path normalization (fold '//', strip trailing '/', reject relative); 0 or -errno. */
int br_vfs_internal_path_norm(const char *in, char *out, br_size_t cap);

/* String compare (production implementation is static vfs_str_eq). */
br_bool br_vfs_internal_str_eq(const char *a, const char *b);

#endif /* BR_VFS_CORE_INTERNAL_H */
