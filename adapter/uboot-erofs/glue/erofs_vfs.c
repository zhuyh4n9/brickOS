/*
 * adapter/uboot-erofs — VFS glue + lifecycle hooks
 *
 * ============================== what this file is ==============================
 * Upstream EROFS is **path style** (u-boot's fs layer hands it a path string and it
 * walks it), while brickOS's VFS (`framework/vfs-core/include/br/vfs/br_vfs.h`) is
 * **two-part** (parent dir inode + a name), and inodes are **transient** (SD-3).
 * So this file is that translation layer:
 *
 * this file = counterpart of upstream `fs/erofs/fs.c`, split into two sections:
 *   ① **block-layer shim** (`uboot_erofs_bdev_bind/dev_read/blk_read`): replace
 *      u-boot's `blk_desc`/`fs_devread` with `br_bdev_ops_t`, and fold "byte offset"
 *      into "sector + in-block offset";
 *   ② **VFS glue**: four layers of ops tables + lifecycle hooks.
 *
 *   VFS contract                          this file             upstream EROFS
 *   ---------------------------------------------------------------------------
 *   br_fs_ops.mount                       mount + build root inode erofs_read_superblock()
 *                                                               + erofs_ilookup("")
 *   br_inode_ops.lookup(dir, name)        path segment → rel path erofs_ilookup(path)
 *   br_inode_ops.getattr(ino)             size/type              (fields in the inode)
 *   br_file_ops.read(f, buf, n)           offset + pread         erofs_pread()
 *   br_file_ops(dir).read                 (block, index) → dirent in-block erofs_readdir
 *   create/unlink/mkdir/rmdir/rename/…    **always -EROFS** (read-only FS)
 *
 * ## inode = transient path wrapper (design SD-3)
 *   Every `lookup` allocates a node with `br_malloc`, holding:
 *     - `path`: path **relative to the mount point** (root = empty string). Why not
 *       just store an nid? Because upstream entry `erofs_ilookup(path, &vi)` only
 *       accepts paths, and the vfs only tells us "parent inode + name" (the parent
 *       inode itself only has a path) ⇒ concatenating the prefix is the only option
 *       that avoids a second tree walk. Cost: every lookup re-walks from the root
 *       (EROFS dir blocks are sorted, but this file scans linearly like upstream).
 *       Acceptable at v1 single-APP scale; recorded as README debt.
 *     - `vi`: the `struct erofs_inode` parsed **this time** (EROFS is read-only, so
 *       inode contents cannot change between reads; caching it with the wrapper is
 *       safe — it saves re-parsing on every read).
 *   Ownership rules (read like tmpfs, opposite conclusion):
 *     - the **root inode** handed out by `mount` is held by the mount table forever,
 *       **never** passed to `free_inode` (it too comes from `br_malloc`, just nobody
 *       returns it);
 *     - every other node is transient ⇒ `free_inode` must `br_free` (else each walk
 *       leaks one).
 *
 * ## read-only enforcement
 *   Hard requirement of the task: the namespace-mutation slots **must be filled**,
 *   but always return `BR_ERR(EROFS)` — more accurate than the -ENOTSUP the vfs
 *   gives for empty slots (EROFS is not "unsupported", it is "read-only").
 *   Likewise `br_file_ops.write` and "open for write" (`br_open(path, BR_O_WRONLY)`)
 *   both report -EROFS: the vfs `open` does not judge access mode for the FS
 *   (whether a read-only fd may write is the fd layer's business), so the
 *   `TC-EROFS-007` criterion "write-open must fail" lands in this file's
 *   `uboot_erofs_file_open`.
 *
 * ## name upper bound (this repo BR_NAME_MAX = 32, EROFS is 255)
 *   `br_dirent_t.name[32]` cannot hold names of 32 bytes or more ⇒ on the directory
 *   face **skip and count** (log the total once); in `lookup` over-long names return
 *   `-ENOSPC` (the vfs `resolve_norm` also gives -ENOSPC for an over-long component,
 *   consistent; see the README trade-off note).
 * ==========================================================================
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/dev/br_bdev.h>
#include <br/vfs/br_vfs.h>

#include "internal.h"        /* upstream fs/erofs/internal.h (compiled as-is) */
#include "erofs_glue.h"

/*
 * ★ `struct erofs_sb_info sbi` —— upstream keeps its **storage** in `fs/erofs/fs.c`
 *   (the `struct erofs_sb_info sbi;` line, see third_party/.../fs/erofs/fs.c:5), and
 *   `fs.c` is u-boot's **blk/DM glue layer** (it knows `struct blk_desc`/
 *   `fs_devread`); this plugin does not compile it — its job (block layer + dir
 *   stream) is taken over by this file.
 *   ⇒ the storage for the global `sbi` is provided by the **adapter layer**
 *   (upstream `internal.h` already `extern`s it). This is the typical shape of
 *   "adaptation": we supply the missing environment, upstream code stays untouched.
 */
struct erofs_sb_info sbi;      /* adapter-internal contract (declared by this project) */

/*
 * ★ **compile-time self-check** of the on-disk layout — done in the adapter layer
 *   rather than instantiating that upstream function.
 *
 * Upstream `erofs_fs.h` ends with a `static inline erofs_check_ondisk_layout_definitions()`
 * holding a series of `BUILD_BUG_ON(sizeof(struct …) != N)`. But **upstream never
 * calls it** (tree-wide grep finds only the definition) ⇒ GCC does not instantiate
 * an uncalled `static inline` ⇒ those `__compiletime_error`s are never diagnosed,
 * so **upstream's self-check is effectively dead** (ADR-0024 §7).
 *
 * This plugin earlier rewrote that table into file-scope `_Static_assert`s (always
 * evaluated) ⇒ strictly **stronger**. This change requires `src/erofs_fs.h` to be
 * **byte-identical** to upstream, so that rewrite can no longer be used; but the
 * layout guarantee must not be dropped ⇒ move it here (**this project's code**;
 * the third_party tree is untouched).
 *
 * Why not "take the address of the upstream function to force instantiation": tried
 * it, it does activate the assertions, but the upstream line
 *   `const __le64 fmh __maybe_unused = *(__le64 *)&(struct …){…};`
 * trips `-Wuninitialized` under this project's `-Wall -Wextra -Werror` — silencing
 * a warning in this file just to compile one statement is worse than restating the
 * sizes here. Sizes are the **criterion**; restating them is legitimate.
 *
 * Criterion (negative case in ADR-0024 §7): break any `== N` ⇒ the build fails at once.
 */
_Static_assert(sizeof(struct erofs_super_block) == 128u, "on-disk: super_block must be 128 B");
_Static_assert(sizeof(struct erofs_inode_compact) == 32u, "on-disk: compact inode must be 32 B");
_Static_assert(sizeof(struct erofs_inode_extended) == 64u, "on-disk: extended inode must be 64 B");
_Static_assert(sizeof(struct erofs_xattr_ibody_header) == 12u, "on-disk: xattr ibody header must be 12 B");
_Static_assert(sizeof(struct erofs_xattr_entry) == 4u, "on-disk: xattr entry must be 4 B");
_Static_assert(sizeof(struct erofs_inode_chunk_info) == 4u, "on-disk: chunk_info must be 4 B");
_Static_assert(sizeof(struct erofs_inode_chunk_index) == 8u, "on-disk: chunk_index must be 8 B");
_Static_assert(sizeof(struct z_erofs_map_header) == 8u, "on-disk: map_header must be 8 B");
_Static_assert(sizeof(struct z_erofs_lcluster_index) == 8u, "on-disk: lcluster_index must be 8 B");
_Static_assert(sizeof(struct erofs_dirent) == 12u, "on-disk: dirent must be 12 B");
_Static_assert(sizeof(struct erofs_deviceslot) == 128u, "on-disk: deviceslot must be 128 B");
/* keep in sync between 2 index structures for better extendibility (upstream words) */
_Static_assert(sizeof(struct erofs_inode_chunk_index) ==
               sizeof(struct z_erofs_lcluster_index), "on-disk: both index structs must match");

/* Lifecycle hook prototypes (name = symbol_prefix + phase; referenced by the
 * generated plugin_desc.c). This plugin **exports no API** ⇒ besides these three
 * plus the self-test hook, every function in this file is static. */
int uboot_erofs_early_init(void);
int uboot_erofs_init(void);
int uboot_erofs_start(void);


/* =====================================================================
 * block-layer shim (ported from upstream fs.c `erofs_dev_read()` / `erofs_blk_read()`)
 *
 * ============================== what this section solves ==============================
 * EROFS internally addresses **everything by byte offset** (`off`), while brickOS's
 * block-device contract (`framework/bdev-core/include/br/dev/br_bdev.h`) addresses
 * by **sector**:
 *
 *     sector     = off / sector_size
 *     sector_off = off % sector_size
 *
 * Upstream writes this in `fs/erofs/fs.c`'s `erofs_dev_read()` (it knows u-boot's
 * `struct blk_desc` and `disk_partition` and uses `fs_devread()` for "offset +
 * length" reads). This file keeps **functions of the same name and meaning**, only
 * swapping the medium interface for `br_bdev_ops_t`:
 *
 *   u-boot side                                brickOS side
 *   ---------------------------------------------------------------------------
 *   ctxt.cur_dev + cur_part_info               br_bdev_get("emmc3", &priv) once
 *   fs_devread(dev, part, sect, off, len, buf) per-sector ops->read(priv, lba, buf, 1)
 *   log2blksz / blksz                          geometry.sector_size (512)
 *
 * ★ Why in `erofs_vfs.c` and not a separate unit: upstream `fs.c` is half block layer,
 *   half u-boot fs-layer glue; both halves are "interface with the outside world"
 *   here, and together they still read as the counterpart of `fs.c` (`super.c`/
 *   `namei.c`/`data.c`/`zmap.c`/`decompress.c` are the pure EROFS parts).
 *
 * ## why issue one sector at a time (rather than "many sectors at once")
 *   The task's hard constraint is "a single I/O must not exceed `geometry.max_rw`"
 *   (this platform = 64 sectors = 32 KiB). Per-sector is the **strongest** way to
 *   satisfy it (1 sector each time, always ≤ max_rw), and it also solves buffer
 *   alignment: each transfer lands at one sector position inside the caller's
 *   buffer, so the caller's buffer only has to be sector-aligned (under
 *   `-mstrict-align` an unaligned multi-byte access takes an Alignment fault).
 *   The cost is more commands — this plugin reads a read-only image and every path
 *   goes by 4 KiB blocks (see erofs_data.c), so 8 reads of 512 B per 4 KiB block is
 *   an acceptable prototype-stage trade-off (recorded as README debt).
 *
 * ## deliberate deviations (from upstream)
 *   1. the `device_id` parameter is **ignored**: multi-device volumes are rejected at
 *      mount (see `erofs_init_devices` in erofs_super.c), so anything reaching here is
 *      device 0. Keeping the formal parameter leaves data.c's
 *      `erofs_dev_read(mdev.m_deviceid, ...)` call site untouched.
 *   2. out of range (offset/len beyond the medium) ⇒ `-EIO`. After multi-device is
 *      rejected, "an on-disk block address points outside this device" must mean a
 *      corrupt image — this is the last line of defence (otherwise a bad nid reads
 *      garbage as an inode).
 *   3. **whole-sector staging** (hit in practice): bdev's `read(lba, buf, 1)` writes
 *      **512 bytes** into `buf`, while EROFS wants "offset + length" (maybe a 32-byte
 *      inode, an 8-byte map header). Handing the caller's buffer straight to the
 *      device = writing 480 bytes out of bounds ⇒ read into the staging buffer first,
 *      then copy out the wanted range. The staging buffer uses the **tail** of the
 *      shared scratch (the last sector): EROFS's own parsing always stays inside a
 *      4 KiB block with `offset % 4096 + len <= 4096`, so it never touches the last
 *      sector.
 * ===================================================================== */

/* Device facts bound once at mount (see br_erofs_dev_t in erofs_internal.h).
 * Non-static: the self-test reads on-disk bytes through it as a criterion (reads the
 * geometry only, writes no field). */
br_erofs_dev_t uboot_erofs_dev;

/* ★ ADR-0030: the medium name comes from the **caller** (passed by the APP's
 * `mount(source, ...)` via `cfg->dev`), no longer hard-coded `BR_EROFS_DEV_NAME` —
 * so "which card's EROFS to mount" is the APP's decision, not this file's constant. */
int uboot_erofs_bdev_bind(const char *dev_name)
{
    void *priv = BR_NULL;

    if (dev_name == BR_NULL) {
        br_log_error("uboot-erofs: mount has no source device (cfg->dev is null)");
        return BR_ERR(EINVAL);
    }
    const br_bdev_ops_t *ops = br_bdev_get(dev_name, &priv);
    if ((ops == BR_NULL) || (priv == BR_NULL)) {
        br_log_error("uboot-erofs: block device %s not found", dev_name);
        return BR_ERR(ENODEV);
    }
    if ((ops->read == BR_NULL) || (ops->geometry == BR_NULL)) {
        br_log_error("uboot-erofs: %s bdev ops lacks read/geometry slot", dev_name);
        return BR_ERR(EINVAL);
    }

    br_bdev_geom_t g;
    const int grc = ops->geometry(priv, &g);
    if (grc != 0) {
        br_log_error("uboot-erofs: %s geometry() failed rc=%d", dev_name, grc);
        return grc;
    }
    if ((g.sector_size == 0u) || (g.n_sectors == 0u) || (g.max_rw == 0u)) {
        br_log_error("uboot-erofs: %s bad geometry (sector=%u n=%llu max_rw=%u)",
                     dev_name, (unsigned)g.sector_size,
                     (unsigned long long)g.n_sectors, (unsigned)g.max_rw);
        return BR_ERR(EINVAL);
    }

    uboot_erofs_dev.ops         = ops;
    uboot_erofs_dev.priv        = priv;
    uboot_erofs_dev.n_sectors   = g.n_sectors;
    uboot_erofs_dev.sector_size = g.sector_size;
    uboot_erofs_dev.max_rw      = g.max_rw;
    uboot_erofs_dev.align       = g.align;
    return 0;
}

int erofs_dev_read(int device_id, void *buf, u64 offset, size_t len)
{
    br_u8 *p = (br_u8 *)buf;
    u64    off;

    /* deviation 1: see file header. Anything reaching here is device 0. */
    (void)device_id;

    if (uboot_erofs_dev.ops == BR_NULL) {
        return BR_ERR(EIO);                 /* read before bind: FS-side ordering bug */
    }
    if ((buf == BR_NULL) && (len != 0u)) {
        return BR_ERR(EINVAL);
    }
    if (len == 0u) {
        return 0;
    }

    /*
     * ★ the crucial step (hit in practice): bdev's `read(lba, buf, 1)` has
     *   **whole-sector** semantics — it writes 512 bytes into `buf`. EROFS wants
     *   "offset + length" (maybe a 32-byte inode, an 8-byte map header). Handing the
     *   caller's buffer to the device = **writing 480 bytes out of bounds**. So we
     *   must "read a whole sector into the staging buffer, then copy out the range".
     *
     * The staging buffer uses the **tail** of the shared scratch (the last sector):
     * all EROFS parsing happens inside a 4 KiB block with
     * `offset % 4096 + len <= 4096`, so it never touches the last sector; and when
     * `erofs_blk_read()` reads a whole block at once the staging buffer lies inside
     * the target block, where copying back to the same place is harmless
     * (`in_sec == 0` means source and destination coincide, which memcpy allows —
     * both stay in bounds). Alignment comes from the u32 view (the DMA path needs
     * ≥ 4-byte alignment).
     */
    br_u8 *stage = (br_u8 *)(void *)((br_u32 *)(void *)uboot_erofs_scratch() +
                                     (EROFS_MAX_BLOCK_SIZE / 4u) - (512u / 4u));

    off = offset;
    for (size_t left = len; left > 0u; ) {
        const br_u64 media_bytes = uboot_erofs_dev.n_sectors * (br_u64)uboot_erofs_dev.sector_size;
        const br_u64 lba         = off / (br_u64)uboot_erofs_dev.sector_size;
        const br_u32 in_sec      = (br_u32)(off % (br_u64)uboot_erofs_dev.sector_size);
        br_size_t    cnt         = (br_size_t)((br_u64)uboot_erofs_dev.sector_size - (br_u64)in_sec);

        if (lba >= uboot_erofs_dev.n_sectors) {
            br_log_error("uboot-erofs: read past end off=0x%llx len=%llu (> %llu byte medium)",
                         (unsigned long long)offset, (unsigned long long)len,
                         (unsigned long long)media_bytes);
            return BR_ERR(EIO);
        }
        if (cnt > left) {
            cnt = left;
        }

        const int rc = uboot_erofs_dev.ops->read(uboot_erofs_dev.priv, lba, stage, 1u);
        if (rc < 0) {
            /* ★ deliberate deviation: EROFS's block-layer error table has only -EIO
             *   (upstream likewise collapses every read failure to -EIO). Keep the
             *   upstream code but log the device's real return value — debugging needs it. */
            br_log_error("uboot-erofs: bdev read failed lba=%llu rc=%d",
                         (unsigned long long)lba, rc);
            return BR_ERR(EIO);
        }
        if (rc != 1) {
            br_log_error("uboot-erofs: bdev short read lba=%llu rc=%d (short read = bad medium)",
                         (unsigned long long)lba, rc);
            return BR_ERR(EIO);
        }
        (void)__builtin_memcpy(p, stage + in_sec, cnt);

        p    += cnt;
        off  += (u64)cnt;
        left -= cnt;
    }
    return 0;
}

int erofs_blk_read(void *buf, erofs_blk_t start, u32 nblocks)
{
    return erofs_dev_read(0, buf, erofs_pos(start), (size_t)erofs_pos(nblocks));
}

/*
 * Shared 4 KiB scratch (see "shared static buffers" in erofs_internal.h).
 * ★ Why static and not on the stack: the kernel gives APP threads a stack on the
 *   order of 4 KiB, so a 4 KiB array on the stack exhausts it (upstream does exactly
 *   that in namei.c's `erofs_namei()`).
 * `s_scratch` only needs `u32` alignment (we never use it as a 64-bit object; all
 * multi-byte accesses go through `__builtin_memcpy` or `__le32*`).
 */
static br_u32 s_scratch_words[EROFS_MAX_BLOCK_SIZE / 4u];

u8 *uboot_erofs_scratch(void)
{
    return (u8 *)(void *)s_scratch_words;
}

/* =====================================================================
 * transient inodes (common head + private tail; CA-2)
 * ===================================================================== */

/*
 * ★ Why `path` is "path relative to the mount point" and not absolute: the mount
 *   point is **two sources of truth** (brick.toml's `[[mount]].path` and
 *   erofs_internal.h's BR_EROFS_MOUNT_PATH), while `erofs_ilookup()` walks from the
 *   volume root — the in-volume namespace is independent of "which absolute path it
 *   is mounted under". Relative paths mean "change the mount point" needs no change
 *   to any path-building code.
 */
typedef struct uboot_erofs_ino {
    br_inode_t         head;                          /* common head (must be first field) */
    struct erofs_inode vi;                            /* EROFS inode parsed this time */
    char               path[UBOOT_EROFS_REL_PATH_MAX]; /* relative to mount point; root = "" */
} uboot_erofs_ino_t;

/* Node identity (same style as fs/tmpfs): use head.fpriv, no extra cross-plugin symbol */
static uboot_erofs_ino_t *uboot_erofs_wrap(const br_inode_t *ino)
{
    return (uboot_erofs_ino_t *)(void *)ino->fpriv;
}

static uboot_erofs_ino_t *uboot_erofs_session(br_file_t *f)
{
    return (uboot_erofs_ino_t *)br_file_fpriv(f);
}

/*
 * Directory iterator (kept on the **file handle**'s fpriv, not the inode's fpriv —
 * the same directory can be opened for iteration many times at once). Lifetime:
 * dir_open allocates / dir_close frees.
 *
 * Block-level cache: EROFS directory data is "one 4 KiB block each, block head is a
 * dirent array, block tail is the name area".
 *   `blk`  = number of the block currently loaded in `s_dir_blk` (relative to the
 *            start of the directory file, in 4 KiB units)
 *   `de_end` = end offset of the dirent array in that block (nameoff of the first dirent)
 *   `max`  = valid byte count of that block (the last block may be shorter)
 * `pos` is the byte offset **within the directory file**, i.e. "where the next
 * dirent is".
 */
#define UBOOT_EROFS_DIR_NO_BLK  ((br_u32)-1)

typedef struct uboot_erofs_dir_iter {
    uboot_erofs_ino_t *ino;         /* directory being enumerated (borrowed; vfs holds it) */
    erofs_off_t        pos;         /* offset within dir file (next dirent) */
    br_u32             cursor;      /* 0 = ".", 1 = "..", ≥2 = entry (cursor-2) of the dir block */
    br_u32             blk;         /* block loaded in s_dir_blk; UBOOT_EROFS_DIR_NO_BLK = none */
    br_u32             de_end;      /* dirent array end offset of loaded block (= name area start) */
    br_u32             max;         /* valid byte count of loaded block */
    br_u32             skipped;     /* entries skipped in this dir for names over BR_NAME_MAX */
} uboot_erofs_dir_iter_t;

/*
 * ★ directory block buffer: a static 4 KiB kept **separate** from the shared scratch.
 *   Why not borrow the shared scratch: a directory read is "read one block of dir
 *   data → parse several dirents inside it", and "read one block of dir data" itself
 *   goes through `erofs_pread()` — which uses the shared scratch for map/index
 *   parsing. Sharing them would let the dir block contents be overwritten by its own
 *   map read. Kept apart, the two paths do not interfere; the cost is 4 KiB of static
 *   data (see "stack discipline" in erofs_internal.h: trading static for stack is the
 *   only way out here).
 */
static br_u32 s_dir_blk_words[EROFS_MAX_BLOCK_SIZE / 4u];

static u8 *uboot_erofs_dir_blk(void)
{
    return (u8 *)(void *)s_dir_blk_words;
}

/* =====================================================================
 * type translation (EROFS dirent file type ↔ brickOS inode type)
 * ===================================================================== */

static br_u32 uboot_erofs_type_from_ftype(u8 ftype)
{
    switch (ftype) {
    case EROFS_FT_DIR:     return BR_INODE_DIR;
    case EROFS_FT_SYMLINK: return BR_INODE_SYMLINK;
    case EROFS_FT_REG_FILE:return BR_INODE_FILE;
    default:               return BR_INODE_NONE;
    }
}

static br_u32 uboot_erofs_type_from_mode(umode_t mode)
{
    return uboot_erofs_type_from_ftype(uboot_erofs_ftype_from_mode(mode));
}

/* Private-tail entry (not br_inode_priv(): with head as the first field both are at
 * the same address, but fpriv is the contract carrier the vfs hands to the file
 * face; using it avoids one cross-plugin symbol — same trade-off as fs/tmpfs). */
static uboot_erofs_ino_t *uboot_erofs_new(const struct erofs_inode *vi, const char *rel_path,
                                          const br_file_ops_t *fops)
{
    uboot_erofs_ino_t *n = (uboot_erofs_ino_t *)br_malloc(sizeof *n);

    if (n == BR_NULL) {
        return BR_NULL;
    }
    n->head.type  = uboot_erofs_type_from_mode(vi->i_mode);
    n->head.iops  = BR_NULL;              /* set by the caller (only dirs need the namespace face) */
    n->head.fops  = fops;
    n->head.fpriv = n;                    /* node identity: the vfs hands it to br_file_t at open */
    n->vi         = *vi;
    /* rel_path is a C string we built ourselves; copy it by length (same discipline as on-disk names). */
    uboot_erofs_copy_n(n->path, (br_size_t)sizeof(n->path), rel_path,
                       uboot_erofs_strnlen(rel_path, (br_size_t)sizeof(n->path)));
    return n;
}

/* Relative path join: parent + '/' + name (empty parent ⇒ just the name). Returns
 * -ENOSPC when it does not fit. The name length is already within bounds (the caller
 * checks first), so only the **total length** is checked here. */
static int uboot_erofs_join(char *dst, br_size_t cap, const char *parent, const char *name)
{
    const br_size_t plen = uboot_erofs_strlen(parent);
    const br_size_t nlen = uboot_erofs_strnlen(name, (br_size_t)BR_NAME_MAX);
    const br_size_t need = plen + ((plen != 0u) ? 1u : 0u) + nlen + 1u;

    if ((nlen == 0u) || (nlen >= (br_size_t)BR_NAME_MAX)) {
        return BR_ERR(ENOSPC);
    }
    if (need > cap) {
        return BR_ERR(ENOSPC);
    }
    /* Join by **length**, not by relying on a TPL "\0" (names come from the vfs
     * component buffer and are always C strings; but use the length version uniformly
     * so a future non-C-string caller does not hit the same trap). */
    if (plen != 0u) {
        (void)__builtin_memcpy(dst, parent, plen);
        dst[plen] = '/';
        (void)__builtin_memcpy(dst + plen + 1u, name, nlen);
        dst[plen + 1u + nlen] = '\0';
    } else {
        (void)__builtin_memcpy(dst, name, nlen);
        dst[nlen] = '\0';
    }
    return 0;
}

/* =====================================================================
 * layer 3: file face
 * ===================================================================== */

static int uboot_erofs_file_open(br_file_t *f, br_u32 flags)
{
    const uboot_erofs_ino_t *n = uboot_erofs_session(f);

    if ((n == BR_NULL) || (f == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if (n->head.type == BR_INODE_DIR) {
        return BR_ERR(EISDIR);
    }
    /* ★ read-only enforcement (see file header): write-open fails **here**, not at
     *   the first write. So a caller that opens a file on a read-only volume for
     *   writing gets the correct -EROFS immediately. */
    if (BR_O_WRITABLE(flags) || ((flags & BR_O_TRUNC) != 0u) || ((flags & BR_O_CREAT) != 0u)) {
        return BR_ERR(EROFS);
    }
    return 0;
}

static br_s64 uboot_erofs_file_read(br_file_t *f, void *buf, br_size_t n)
{
    /* Named `node` rather than `n`: in this function `n` is **how many bytes to read**
     * (the contract signature says so). */
    const uboot_erofs_ino_t *node = uboot_erofs_session(f);
    br_s64 off;

    if ((node == BR_NULL) || ((buf == BR_NULL) && (n != 0u))) {
        return BR_ERR(EINVAL);
    }
    if (node->head.type == BR_INODE_DIR) {
        return BR_ERR(EISDIR);
    }
    if (node->head.type != BR_INODE_FILE) {
        return BR_ERR(EINVAL);
    }
    if (n == 0u) {
        return 0;
    }

    off = br_file_offset(f);
    if (off < 0) {
        return BR_ERR(EINVAL);
    }
    if ((erofs_off_t)off >= node->vi.i_size) {
        return 0;                          /* at/past EOF: 0 bytes is **success** */
    }

    erofs_off_t want = node->vi.i_size - (erofs_off_t)off;
    if ((erofs_off_t)n < want) {
        want = (erofs_off_t)n;
    }

    const int rc = erofs_pread((struct erofs_inode *)&node->vi, (char *)buf, want,
                               (erofs_off_t)off);
    if (rc < 0) {
        return rc;
    }
    br_file_set_offset(f, off + (br_s64)want);
    return (br_s64)want;
}

/* read-only: fill the write slot (more accurate than an empty slot), always -EROFS */
static br_s64 uboot_erofs_file_write(br_file_t *f, const void *buf, br_size_t n)
{
    (void)f;
    (void)buf;
    (void)n;
    return BR_ERR(EROFS);
}

static br_s64 uboot_erofs_file_lseek(br_file_t *f, br_s64 off, int whence)
{
    const uboot_erofs_ino_t *n = uboot_erofs_session(f);
    br_s64 base;
    br_s64 pos;

    if ((n == BR_NULL) || (n->head.type != BR_INODE_FILE)) {
        return BR_ERR(EINVAL);
    }
    switch (whence) {
    case BR_SEEK_SET: base = 0; break;
    case BR_SEEK_CUR: base = br_file_offset(f); break;
    case BR_SEEK_END: base = (br_s64)n->vi.i_size; break;
    default:          return BR_ERR(EINVAL);
    }
    if (base < 0) {
        return BR_ERR(EINVAL);
    }
    /* Allow seeking past EOF (POSIX): reads then return 0. Use the builtin to detect
     * overflow so signed addition cannot overflow (UB). */
    if (__builtin_add_overflow(base, off, &pos) || (pos < 0)) {
        return BR_ERR(EINVAL);
    }
    br_file_set_offset(f, pos);
    return pos;
}

static int uboot_erofs_file_close(br_file_t *f)
{
    /* A file session has nothing to give back: fpriv is still the node (transient
     * nodes are freed by the vfs via free_inode). Pairs with the directory face —
     * there the iterator must be br_free'd. */
    (void)f;
    return 0;
}

/* =====================================================================
 * layer 3: directory face (read = exactly one br_dirent_t)
 * ===================================================================== */

/*
 * Fill one fixed-size directory entry.
 * ★ Must copy **by length** using `name_len`, not by "scan until NUL": names inside
 *   EROFS directory blocks are packed back to back with **no** NUL terminator — the
 *   observed symptom was that the first entry "comp" swallowed every following name
 *   ("compdirhello.txtraw.bin"). Zero the whole entry first, so `name[name_len..]` is
 *   all '\0' (no stale bytes from a previous round leak to the caller).
 */
static void uboot_erofs_dirent_fill(br_dirent_t *de, br_u32 type, const char *name,
                                    br_u32 name_len)
{
    (void)__builtin_memset(de, 0, sizeof *de);
    de->type     = type;
    de->name_len = name_len;
    uboot_erofs_copy_n(de->name, (br_size_t)BR_NAME_MAX, name, (br_size_t)name_len);
}

static int uboot_erofs_dir_open(br_file_t *f, br_u32 flags)
{
    uboot_erofs_ino_t *n = uboot_erofs_session(f);
    uboot_erofs_dir_iter_t *it;

    (void)flags;    /* the vfs already rejects "write-open of a dir ⇒ -EISDIR" in br_open */
    if ((n == BR_NULL) || (n->head.type != BR_INODE_DIR)) {
        return BR_ERR(ENOTSUP);
    }

    it = (uboot_erofs_dir_iter_t *)br_malloc(sizeof *it);
    if (it == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    it->ino     = n;
    it->pos     = 0;
    it->cursor  = 0u;
    it->blk      = UBOOT_EROFS_DIR_NO_BLK;
    it->de_end   = 0u;
    it->max     = 0u;
    it->skipped = 0u;
    br_file_set_fpriv(f, it);
    return 0;
}

/*
 * Load directory block number `blk` (if not already buffered). Returns the block's
 * valid byte count, or a negative errno on failure.
 *
 * ★ directory block layout (confirmed in practice, same reading as upstream `fs.c`'s
 *   `erofs_readdir()`):
 *     [0, de_end)        : **contiguous dirent array** (12 bytes per entry, sorted by name bytes)
 *     [de_end, max)      : name area (packed in order; each dirent's `nameoff` points here)
 *   `de_end` is **the `nameoff` of the block's first dirent** — it does not just mean
 *   "start of the name area", EROFS uses it to say "how long this block's dirent
 *   array is". So in-block parsing is very direct:
 *     idx   = in-block offset / 12
 *     namelen = (next entry's nameoff) - (this entry's nameoff); the last one scans to NUL.
 *   (This file's first version mistook it for an "array start offset" and subtracted
 *    once too often, which packed four names into one — kept in the comment because it
 *    is the classic "looks right but off by a constant" case.)
 */
static int uboot_erofs_dir_load_block(uboot_erofs_dir_iter_t *it, br_u32 blk)
{
    const uboot_erofs_ino_t *n = it->ino;
    const erofs_off_t blk_off = (erofs_off_t)blk * (erofs_off_t)erofs_blksiz();
    erofs_off_t want;
    const struct erofs_dirent *de;
    br_u32 de_end;
    int rc;

    if ((it->blk == blk) && (it->max != 0u)) {
        return (int)it->max;               /* already buffered */
    }

    want = n->vi.i_size - blk_off;
    if (want > (erofs_off_t)erofs_blksiz()) {
        want = (erofs_off_t)erofs_blksiz();
    }
    if (want <= 0) {
        it->blk = blk;
        it->max = 0u;
        return 0;
    }

    rc = erofs_pread((struct erofs_inode *)&n->vi, (char *)uboot_erofs_dir_blk(),
                     want, blk_off);
    if (rc < 0) {
        return rc;
    }

    de = (const struct erofs_dirent *)(const void *)uboot_erofs_dir_blk();
    de_end = (br_u32)le16_to_cpu(de->nameoff);
    if ((de_end < sizeof(struct erofs_dirent)) || (de_end > erofs_blksiz())) {
        erofs_err("invalid de[0].nameoff %u @ nid %llu", de_end,
                  (unsigned long long)n->vi.nid);
        return BR_ERR(EIO);
    }

    it->blk    = blk;
    it->de_end = de_end;
    it->max    = (br_u32)want;
    return (int)it->max;
}

/* Byte offset within the block of entry `idx` (0-based, relative to this block);
 * past the end of the array returns -1. */
static int uboot_erofs_dir_de_off(const uboot_erofs_dir_iter_t *it, br_u32 idx)
{
    const br_u32 off = idx * (br_u32)sizeof(struct erofs_dirent);

    if ((off + (br_u32)sizeof(struct erofs_dirent)) > it->de_end) {
        return -1;
    }
    return (int)off;
}

static br_s64 uboot_erofs_dir_read(br_file_t *f, void *buf, br_size_t n)
{
    uboot_erofs_dir_iter_t *it = (uboot_erofs_dir_iter_t *)br_file_fpriv(f);
    br_dirent_t *de = (br_dirent_t *)buf;

    if ((it == BR_NULL) || (it->ino == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* Fixed-size records cannot be truncated: not enough room for one entry ⇒ -EINVAL
     * (returning 0 would be read by br_readdir as "directory exhausted"; returning
     * half an entry would be an -EIO FS violation). The vfs always passes sizeof(br_dirent_t). */
    if ((buf == BR_NULL) || (n < (br_size_t)sizeof(br_dirent_t))) {
        return BR_ERR(EINVAL);
    }

    /* "." and ".." are synthesized in place (EROFS dir blocks **do not** contain these
     * two entries — upstream is the same) */
    if (it->cursor == 0u) {
        it->cursor = 1u;
        uboot_erofs_dirent_fill(de, BR_INODE_DIR, ".", 1u);
        return (br_s64)sizeof(br_dirent_t);
    }
    if (it->cursor == 1u) {
        it->cursor = 2u;
        uboot_erofs_dirent_fill(de, BR_INODE_DIR, "..", 2u);
        return (br_s64)sizeof(br_dirent_t);
    }

    for (;;) {
        if (it->pos >= (erofs_off_t)it->ino->vi.i_size) {
            break;                          /* file fully read: exhausted */
        }

        const br_u32 blk = (br_u32)((erofs_off_t)it->pos >> sbi.blkszbits);
        if (blk != it->blk) {
            const int m = uboot_erofs_dir_load_block(it, blk);
            if (m < 0) {
                return m;
            }
            if (m == 0) {
                it->pos = it->ino->vi.i_size;   /* past the last block: converge */
                break;
            }
        }

        const br_u32 in_blk = (br_u32)erofs_blkoff((erofs_off_t)it->pos);

        /* ★ the block's dirent array is bounded by **the first dirent's nameoff**
         *   (`de_end`):
         *   - `in_blk >= de_end` is already inside the name area (reading on yields
         *     garbage) ⇒ skip to the next block;
         *   - otherwise `in_blk` must land in the array (which advances by 12 bytes). */
        if ((in_blk + (br_u32)sizeof(struct erofs_dirent)) > it->de_end) {
            it->pos = (erofs_off_t)(blk + 1u) * (erofs_off_t)erofs_blksiz();
            it->blk = UBOOT_EROFS_DIR_NO_BLK;
            continue;
        }

        const struct erofs_dirent *dent =
            (const struct erofs_dirent *)(const void *)(uboot_erofs_dir_blk() + in_blk);
        const br_u32 nameoff  = (br_u32)le16_to_cpu(dent->nameoff);
        const char  *de_name  = (const char *)(uboot_erofs_dir_blk() + nameoff);
        const br_u32 de_index = in_blk / (br_u32)sizeof(struct erofs_dirent);
        const int    next_off = uboot_erofs_dir_de_off(it, de_index + 1u);
        const br_u32 type     = uboot_erofs_type_from_ftype(dent->file_type);
        br_u32       de_namelen;

        /* advance the cursor (whether or not this entry is accepted) */
        it->pos = (erofs_off_t)blk * (erofs_off_t)erofs_blksiz() +
                  (erofs_off_t)(in_blk + (br_u32)sizeof(struct erofs_dirent));

        if (nameoff >= it->max) {
            erofs_err("bogus dirent nameoff %u @ nid %llu", nameoff,
                      (unsigned long long)it->ino->vi.nid);
            return BR_ERR(EIO);
        }

        if (next_off < 0) {
            /* last entry of the block: scan the name to NUL, bound = block valid bytes */
            de_namelen = (br_u32)uboot_erofs_strnlen(de_name,
                                                     (br_size_t)(it->max - nameoff));
        } else {
            const struct erofs_dirent *next =
                (const struct erofs_dirent *)(const void *)
                (uboot_erofs_dir_blk() + (br_u32)next_off);
            const br_u32 next_nameoff = (br_u32)le16_to_cpu(next->nameoff);

            de_namelen = (next_nameoff > nameoff) ? (next_nameoff - nameoff) : 0u;
        }

        /* corrupt entry: same criterion as upstream */
        if ((nameoff + de_namelen > it->max) || (de_namelen > EROFS_DIRENT_MAX_NAME)) {
            erofs_err("bogus dirent @ nid %llu", (unsigned long long)it->ino->vi.nid);
            return BR_ERR(EIO);
        }
        if (de_namelen == 0u) {
            continue;                       /* empty name is not a valid dir entry: skip */
        }
        if (type == BR_INODE_NONE) {
            continue;                       /* device files/sockets etc.: this VFS face has none */
        }
        if (de_namelen >= (br_u32)BR_NAME_MAX) {
            /* ★ name upper-bound trade-off (see file header): skip and count, so one
             *   over-long name does not turn the whole readdir into -ENOSPC (the vfs
             *   component buffer could not hold it either). */
            it->skipped++;
            continue;
        }

        uboot_erofs_dirent_fill(de, type, de_name, de_namelen);
        return (br_s64)sizeof(br_dirent_t);
    }

    if (it->skipped != 0u) {
        /* report once only (at the moment of exhaustion), so repeated directory reads
         * do not flood the log */
        br_log_warn("uboot-erofs: %s has %u names over %u bytes, skipped on this repo's dir face",
                    (it->ino->path[0] != '\0') ? it->ino->path : "/",
                    (unsigned)it->skipped, (unsigned)BR_NAME_MAX - 1u);
        it->skipped = 0u;
    }
    return 0;                               /* exhausted */
}

static int uboot_erofs_dir_close(br_file_t *f)
{
    uboot_erofs_dir_iter_t *it = (uboot_erofs_dir_iter_t *)br_file_fpriv(f);

    if (it != BR_NULL) {
        br_free(it);
        br_file_set_fpriv(f, BR_NULL);      /* make a second close a no-op */
    }
    return 0;
}

/* =====================================================================
 * ops tables
 * ===================================================================== */

static const br_file_ops_t s_file_fops = {
    .open        = uboot_erofs_file_open,
    .read        = uboot_erofs_file_read,
    .write       = uboot_erofs_file_write,   /* filled in but always -EROFS (right code for a read-only FS) */
    .lseek       = uboot_erofs_file_lseek,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,                  /* a read-only volume has no "flush to medium" */
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,                  /* reserved for v2 (wait-queue); always NULL in v1 */
    .close       = uboot_erofs_file_close,
};

static const br_file_ops_t s_dir_fops = {
    .open        = uboot_erofs_dir_open,
    .read        = uboot_erofs_dir_read,
    .write       = BR_NULL,
    .lseek       = BR_NULL,
    .ioctl       = BR_NULL,
    .fsync       = BR_NULL,
    .poll        = BR_NULL,
    .poll_attach = BR_NULL,
    .close       = uboot_erofs_dir_close,
};

static br_inode_t *uboot_erofs_lookup(br_inode_t *dir, const char *name);

/* Layer 2. ★ all 8 namespace-mutation slots are **filled** and always return -EROFS
 * (the right error code for a read-only FS, more accurate than the -ENOTSUP the vfs
 * gives for an empty slot — callers can then tell "this FS does not support create"
 * from "this volume is read-only"). `readlink` is a **read-only** operation and is
 * supported (EROFS has symlinks).
 *
 * ★ Why 6 small functions with distinct signatures rather than one void variant: each
 *   slot of `br_inode_ops_t` has a different signature, and C does not allow assigning
 *   `int f(void)` to `int (*)(br_inode_t*, const char*)` (a different parameter count
 *   is a different type) ⇒ one shell per signature, all with the same body (return -EROFS). */
static int uboot_erofs_ro_create(br_inode_t *dir, const char *name, br_u32 flags,
                                 br_inode_t **out);
static int uboot_erofs_ro_name  (br_inode_t *dir, const char *name);
static int uboot_erofs_ro_rename(br_inode_t *dir, const char *name, br_inode_t *ndir,
                                 const char *nname);
static int uboot_erofs_ro_symlink(br_inode_t *dir, const char *name, const char *target);
static int uboot_erofs_ro_link   (br_inode_t *dir, const char *name, br_inode_t *target);
static int uboot_erofs_setattr  (br_inode_t *ino, const br_stat_t *st);
static int uboot_erofs_getattr  (br_inode_t *ino, br_stat_t *st);
static int uboot_erofs_readlink (br_inode_t *ino, char *buf, br_size_t cap);

static const br_inode_ops_t s_iops = {
    .lookup   = uboot_erofs_lookup,
    .create   = uboot_erofs_ro_create,
    .unlink   = uboot_erofs_ro_name,
    .mkdir    = uboot_erofs_ro_name,
    .rmdir    = uboot_erofs_ro_name,
    .rename   = uboot_erofs_ro_rename,
    .getattr  = uboot_erofs_getattr,
    .setattr  = uboot_erofs_setattr,
    .symlink  = uboot_erofs_ro_symlink,
    .readlink = uboot_erofs_readlink,
    .link     = uboot_erofs_ro_link,
};

/*
 * ★ layer 2's second/third tables: **non-directory** nodes must also have an iops.
 *   The contract header says it clearly (br_inode comment in br_vfs.h): "`getattr`/
 *   `setattr` belong to **this layer** (inode level) — so every inode supporting
 *   `br_stat` must carry an iops (even if only getattr is filled)".
 *   Giving iops only to directories made `br_stat("/assets/hello.txt")` return
 *   -ENOTSUP (observed) instead of file attributes — a contract detail this file's
 *   first version **actually hit**.
 *
 *   So this file uses three tables:
 *     s_iops       directories only: namespace face (mutation slots always -EROFS) + getattr
 *     s_file_iops  regular files only: getattr only (namespace face empty ⇒ BR_NULL = unsupported)
 *     s_link_iops  symlinks only: getattr + readlink (the link's **own** attributes and target string)
 */
static const br_inode_ops_t s_file_iops = {
    .getattr  = uboot_erofs_getattr,
    /* ★ `.setattr` must be filled too: `br_truncate()` goes through the **file's own**
     *   iops->setattr (the vfs br_truncate resolves the leaf, then calls
     *   ino->iops->setattr). The first version left an empty slot here ⇒
     *   `br_truncate("/assets/hello.txt", 0)` got the vfs's -ENOTSUP for an empty slot
     *   instead of the -EROFS a read-only volume should give (observed: TC-EROFS-007
     *   went red because of it). */
    .setattr  = uboot_erofs_setattr,
    .readlink = BR_NULL,
};

static const br_inode_ops_t s_link_iops = {
    .getattr  = uboot_erofs_getattr,
    .setattr  = uboot_erofs_setattr,
    .readlink = uboot_erofs_readlink,
};

/* Layer 1: super. `unmount` is a signature-first slot in v1 (runtime unmount is not
 * before v3) ⇒ BR_NULL; `sync` = 0: a read-only volume has no dirty data. */
static int   uboot_erofs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root);
static void  uboot_erofs_free_inode(br_inode_t *ino);
static int   uboot_erofs_sync(void *fs_priv);

static const br_fs_ops_t s_fs_ops = {
    .mount      = uboot_erofs_mount,
    .free_inode = uboot_erofs_free_inode,
    .unmount    = BR_NULL,
    .sync       = uboot_erofs_sync,
};

/* =====================================================================
 * layer 2 implementation
 * ===================================================================== */

/*
 * lookup: translate "parent inode + a name" into a "path relative to the mount
 * point", then hand it to upstream's path walk.
 * Miss / over-long name / unknown type all converge to BR_NULL (the vfs `resolve_norm`
 * uses BR_NULL to tell "does not exist" from "failed" ⇒ gives -ENOENT). The task says
 * over-long names should return -ENOSPC, but lookup's signature has **no error-code
 * channel** (it returns an inode pointer), so only BR_NULL is possible here; the
 * -ENOSPC reading lives in the vfs itself (`resolve_norm` gives -ENOSPC for an
 * over-long component, consistent).
 */
static br_inode_t *uboot_erofs_lookup(br_inode_t *dir, const char *name)
{
    uboot_erofs_ino_t *d;
    uboot_erofs_ino_t *child;
    char               rel[UBOOT_EROFS_REL_PATH_MAX];
    struct erofs_inode vi;
    const br_file_ops_t *fops;
    int rc;

    if ((dir == BR_NULL) || (dir->type != BR_INODE_DIR) || (name == BR_NULL)) {
        return BR_NULL;
    }
    rel[0] = '\0';
    d = uboot_erofs_wrap(dir);
    if (d == BR_NULL) {
        return BR_NULL;
    }

    if (uboot_erofs_strcmp(name, ".") == 0) {
        return dir;                        /* itself is a transient node; return it (vfs ino_put's once) */
    }
    rc = uboot_erofs_join(rel, (br_size_t)sizeof(rel), d->path, name);
    if (rc != 0) {
        /* name too long (≥ BR_NAME_MAX): this file's reading is "skip / does not exist". */
        return BR_NULL;
    }

    vi.nid = 0;
    rc = erofs_ilookup(rel, &vi);
    if (rc != 0) {
        return BR_NULL;
    }

    const br_u32 type = uboot_erofs_type_from_mode(vi.i_mode);
    if (type == BR_INODE_DIR) {
        fops = &s_dir_fops;
    } else if (type == BR_INODE_FILE) {
        fops = &s_file_fops;
    } else if (type == BR_INODE_SYMLINK) {
        fops = BR_NULL;                    /* link is "visible but not openable" (vfs expands or -ELOOP) */
    } else {
        return BR_NULL;                    /* device files/sockets: this VFS face has none */
    }

    child = uboot_erofs_new(&vi, rel, fops);
    if (child == BR_NULL) {
        return BR_NULL;
    }
    /* every node needs getattr (see the note above) */
    if (type == BR_INODE_DIR) {
        child->head.iops = &s_iops;
    } else if (type == BR_INODE_SYMLINK) {
        child->head.iops = &s_link_iops;
    } else {
        child->head.iops = &s_file_iops;
    }
    return &child->head;
}

static int uboot_erofs_getattr(br_inode_t *ino, br_stat_t *st)
{
    const uboot_erofs_ino_t *n;

    if ((ino == BR_NULL) || (st == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    n = uboot_erofs_wrap(ino);
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }

    /* Directory size is reported as 0 per this repo's reading (same as tmpfs:
     * `br_stat_t.size` is meaningless for directories); a link's size = target string
     * length (so says the contract header), and this file knows that length without
     * expanding the target or reading the target string out — `i_size` is it. */
    st->valid = BR_STAT_SIZE;
    st->type  = ino->type;
    st->size  = (ino->type == BR_INODE_DIR) ? 0u : n->vi.i_size;
    st->nlink = 0u;                        /* BR_STAT_NLINK bit not set: meaningless on a read-only volume */
    return 0;
}

/* ---- read-only enforcement: namespace mutation (6 signatures, one answer: -EROFS) ---- */

static int uboot_erofs_ro_create(br_inode_t *dir, const char *name, br_u32 flags,
                                 br_inode_t **out)
{
    (void)dir;
    (void)name;
    (void)flags;
    if (out != BR_NULL) {
        *out = BR_NULL;
    }
    return BR_ERR(EROFS);
}

static int uboot_erofs_ro_name(br_inode_t *dir, const char *name)
{
    (void)dir;
    (void)name;
    return BR_ERR(EROFS);
}

static int uboot_erofs_ro_rename(br_inode_t *dir, const char *name, br_inode_t *ndir,
                                 const char *nname)
{
    (void)dir;
    (void)name;
    (void)ndir;
    (void)nname;
    return BR_ERR(EROFS);
}

static int uboot_erofs_ro_symlink(br_inode_t *dir, const char *name, const char *target)
{
    (void)dir;
    (void)name;
    (void)target;
    return BR_ERR(EROFS);
}

static int uboot_erofs_ro_link(br_inode_t *dir, const char *name, br_inode_t *target)
{
    (void)dir;
    (void)name;
    (void)target;
    return BR_ERR(EROFS);
}

static int uboot_erofs_setattr(br_inode_t *ino, const br_stat_t *st)
{
    (void)ino;
    (void)st;
    return BR_ERR(EROFS);
}

/*
 * readlink: EROFS supports symlinks (the target string lives in the inode's data
 * face and is read with `erofs_pread`).
 * Contract: return the **full length** (truncation is not an error); with `cap == 0`
 * or `buf == NULL` only report the length; a non-link ⇒ -EINVAL.
 */
static int uboot_erofs_readlink(br_inode_t *ino, char *buf, br_size_t cap)
{
    const uboot_erofs_ino_t *n;
    erofs_off_t len;
    int rc;

    if (ino == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (ino->type != BR_INODE_SYMLINK) {
        return BR_ERR(EINVAL);
    }
    n = uboot_erofs_wrap(ino);
    if (n == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    len = n->vi.i_size;
    if (len == 0u) {
        return 0;
    }
    if ((buf == BR_NULL) || (cap == 0u)) {
        return (int)len;
    }

    const br_size_t cnt = ((br_size_t)len < cap) ? (br_size_t)len : (cap - 1u);

    /* read only cnt bytes (not the full length): the target may be longer than cap, and
     * reading more would run past the caller's buffer. */
    rc = erofs_pread((struct erofs_inode *)&n->vi, buf, (erofs_off_t)cnt, 0);
    if (rc < 0) {
        return rc;
    }
    buf[cnt] = '\0';
    return (int)len;                        /* contract: return the full length */
}

/* =====================================================================
 * layer 1 implementation: mount / free_inode / sync
 * ===================================================================== */

static void uboot_erofs_free_inode(br_inode_t *ino)
{
    /* ★ a read-only FS's transient node is just a br_malloc'd wrapper (no subobjects).
     *   The mount table's root inode is **not** passed in here (the vfs ino_put splits
     *   on is_root). */
    if (ino != BR_NULL) {
        br_free(ino);
    }
}

static int uboot_erofs_sync(void *fs_priv)
{
    (void)fs_priv;
    return 0;                              /* read-only volume: no dirty data to flush */
}

static int uboot_erofs_mount(void *fs_priv, const br_fs_cfg_t *cfg, br_inode_t **root)
{
    struct erofs_inode vi;
    uboot_erofs_ino_t *r;
    int rc;

    (void)fs_priv;                         /* no FS-level bookkeeping object: the volume state is sbi */

    if ((cfg == BR_NULL) || (root == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    *root = BR_NULL;

    /* ★ ADR-0030: both the mount point and the source device are **decided by the APP**
     *   (via cfg). This used to pin the mount point to `BR_EROFS_MOUNT_PATH` and report
     *   -EINVAL on mismatch — a leftover from "the FS carries its own mount plan".
     *   An EROFS volume is unrelated to the mount point anyway (only to the medium), so
     *   here we only require both to be non-null. */
    if ((cfg->path == BR_NULL) || (cfg->dev == BR_NULL)) {
        br_log_error("uboot-erofs: incomplete mount args (path=%s dev=%s)",
                     (cfg->path != BR_NULL) ? cfg->path : "(null)",
                     (cfg->dev != BR_NULL) ? cfg->dev : "(null)");
        return BR_ERR(EINVAL);
    }

    /* 1) block device: fetch the bdev by the source device name from the APP and
     *    remember the geometry (sector size / medium byte count) */
    rc = uboot_erofs_bdev_bind(cfg->dev);
    if (rc != 0) {
        return rc;                          /* first failure stops boot (see brick.toml) */
    }

    /* 2) superblock: the magic must be at **offset 1024** (EROFS_SUPER_OFFSET) */
    rc = erofs_read_superblock();
    if (rc != 0) {
        return rc;
    }
    if (erofs_blksiz() != EROFS_MAX_BLOCK_SIZE) {
        /* upstream's compressed path supports PAGE_SIZE blocks only; the uncompressed
         * face supports 512/1024/2048, but this file's static buffers are designed for
         * 4 KiB ⇒ require 4 KiB uniformly (stated in the README trimming notes). */
        br_log_error("uboot-erofs: only %u-byte blocks supported (image blkszbits=%u)",
                     (unsigned)EROFS_MAX_BLOCK_SIZE, (unsigned)sbi.blkszbits);
        return BR_ERR(ENOTSUP);
    }

    /* 3) root inode (path = empty string = volume root) */
    vi.nid = 0;
    rc = erofs_ilookup("", &vi);
    if (rc != 0) {
        br_log_error("uboot-erofs: failed to parse root inode rc=%d", rc);
        return rc;
    }
    if (!S_ISDIR(vi.i_mode)) {
        br_log_error("uboot-erofs: root inode is not a directory (mode=%o)", (unsigned)vi.i_mode);
        return BR_ERR(EIO);
    }

    /* 4) build the root inode (held long-term by the mount table; br_malloc'd too, just never freed) */
    r = uboot_erofs_new(&vi, "", &s_dir_fops);
    if (r == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    r->head.iops = &s_iops;
    *root = &r->head;

    /* ★ the wording of this line is **part of the gate contract** (tests/gates.toml's
     *   `erofs-test` literally requires the substring `uboot-erofs: /assets ready`):
     *   check that gate before changing the wording. */
    br_log_info("uboot-erofs: %s ready (%s; %s; %llu blocks x %u B; root_nid=%llu; %s)",
                cfg->path, BR_EROFS_TYPE_NAME, BR_EROFS_DEV_NAME,
                (unsigned long long)sbi.total_blocks,
                (unsigned)erofs_blksiz(),
                (unsigned long long)sbi.root_nid,
                (sbi.feature_incompat & EROFS_FEATURE_INCOMPAT_ZERO_PADDING)
                    ? "LZ4/0padding" : "LZ4");
    return 0;
}

/* =====================================================================
 * lifecycle hooks (symbol_prefix = uboot_erofs)
 * ===================================================================== */

int uboot_erofs_early_init(void)
{
    return 0;   /* no heap, no block device yet: the real work is all in start */
}

int uboot_erofs_init(void)
{
    return 0;   /* see brick.toml: mounting happens in start (the block device is registered
                 * by uboot-mmc's start, and this plugin declares a [[dep]] init edge on
                 * it ⇒ phase order is guaranteed by the plugin manager) */
}

int uboot_erofs_start(void)
{
    /* ★ ADR-0030: only **register the driver**, do not mount — where to mount and which
     *   card to mount are decided by the APP via POSIX
     *   `mount(source, target, "erofs", 0, NULL)`. */
    const int rc = br_fs_driver_register("erofs", &s_fs_ops, BR_NULL);

    if (rc != 0) {
        br_log_error("uboot-erofs: driver 'erofs' registration failed rc=%d (first failure stops boot)", rc);
        return rc;
    }
    return 0;
}
