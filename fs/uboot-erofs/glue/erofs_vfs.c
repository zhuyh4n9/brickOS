/*
 * fs/uboot-erofs - VFS glue + lifecycle hooks; counterpart of upstream fs/erofs/fs.c.
 * Upstream EROFS is path style while brickOS's VFS is two-part (parent inode + name), so this
 * file translates: a block-layer shim over br_bdev_ops_t instead of blk_desc/fs_devread, plus
 * four ops tables and lifecycle hooks.
 *
 * Inodes are transient path wrappers, br_malloc'd per lookup. `path` is relative to the mount
 * point (root = "") because erofs_ilookup() takes paths and the vfs only gives parent + name;
 * every lookup therefore re-walks from the root, acceptable at v1 single-APP scale. Ownership:
 * the root inode is held by the mount table forever and never passed to free_inode, while every
 * other node is transient and must be br_free'd by free_inode.
 *
 * Read-only enforcement: the namespace-mutation slots are filled but always return BR_ERR(EROFS)
 * (more accurate than the -ENOTSUP an empty slot gives); write and write-open also report
 * -EROFS, since the vfs open does not judge access mode -- this is where TC-EROFS-007 lands.
 * Names over this repo's BR_NAME_MAX (32) are skipped and counted on the directory face, and
 * lookup returns BR_NULL for them (the vfs resolve_norm itself gives -ENOSPC).
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/hal/br_mem.h>
#include <br/dev/br_bdev.h>
#include <br/vfs/br_vfs.h>

#include "internal.h"        /* upstream fs/erofs/internal.h (compiled as-is) */
#include "erofs_glue.h"

/* struct erofs_sb_info sbi: upstream stores it in fs.c, which is u-boot's blk/DM glue layer and
 * is not compiled here. internal.h already externs it, so the adapter supplies the storage. */
struct erofs_sb_info sbi;      /* adapter-internal contract (declared by this project) */

/* On-disk layout self-check. Upstream erofs_fs.h holds these in an uncalled static inline, so
 * its __compiletime_error never fires; taking its address trips -Wuninitialized under this
 * project's -Wall -Wextra -Werror. Restating the sizes here is the criterion: breaking any
 * `== N` fails the build at once. */
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
/* Keep both index structs in sync (upstream wording). */
_Static_assert(sizeof(struct erofs_inode_chunk_index) ==
               sizeof(struct z_erofs_lcluster_index), "on-disk: both index structs must match");

/* Lifecycle hooks (symbol_prefix + phase), referenced by the generated plugin_desc.c. This
 * plugin exports no API, so besides these and the self-test hook everything here is static. */
int uboot_erofs_early_init(void);
int uboot_erofs_init(void);
int uboot_erofs_start(void);


/* ---- block-layer shim (counterpart of upstream fs.c's erofs_dev_read()/erofs_blk_read()) ----
 * EROFS addresses by byte offset, the bdev by sector: sector = off / sector_size,
 * sector_off = off % sector_size. Same function names and meaning as upstream, only the medium
 * interface changes (blk_desc/fs_devread -> br_bdev_ops_t).
 *
 * One sector per I/O: satisfies the "single I/O <= geometry.max_rw" constraint (64 sectors =
 * 32 KiB here) and keeps each transfer at one sector position in the caller's buffer, so the
 * buffer only needs sector alignment (-mstrict-align faults on unaligned multi-byte access).
 * Cost is more commands (8 x 512 B per 4 KiB block), acceptable at prototype stage.
 *
 * Deviations from upstream: device_id is ignored (multi-device volumes are rejected at mount,
 * so anything reaching here is device 0; the parameter stays for data.c's call site); out of
 * range -> -EIO, the last line of defence against a corrupt image; and whole-sector staging,
 * see the note at the staging buffer below. */

/* Device facts bound once at mount (see br_erofs_dev_t). Non-static: the self-test reads the
 * geometry through it (reads only, writes no field). */
br_erofs_dev_t uboot_erofs_dev;

/* The medium name comes from the caller: the APP passes it via cfg->dev (mount(source, ...)),
 * rather than this file hard-coding BR_EROFS_DEV_NAME. */
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

/* device_id ignored: multi-device is rejected at mount, so this is device 0. */
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

/* Whole-sector staging: bdev's read(lba, buf, 1) writes 512 bytes, while EROFS wants a range
 * (a 32-byte inode, an 8-byte map header), so writing the caller's buffer directly would go out
 * of bounds. Read one sector into the staging buffer, then copy out the wanted range. The
 * staging buffer is the tail sector of the shared scratch: EROFS parsing stays inside one 4 KiB
 * block (offset % 4096 + len <= 4096) and never touches it. u32 view for DMA alignment. */
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
/* Upstream collapses read failures to -EIO; keep that, but log the real rc for debugging. */
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

/* Shared 4 KiB scratch: static, not on the stack, because APP stacks are on the order of
 * 4 KiB (upstream namei.c's erofs_namei() puts this on the stack). Only needs u32 alignment:
 * every multi-byte access goes through __builtin_memcpy or __le32*. */
static br_u32 s_scratch_words[EROFS_MAX_BLOCK_SIZE / 4u];

u8 *uboot_erofs_scratch(void)
{
    return (u8 *)(void *)s_scratch_words;
}

/* ---- transient inodes (common head + private tail) ---- */

/* `path` is relative to the mount point, not absolute: the mount point has two sources of
 * truth (brick.toml, BR_EROFS_MOUNT_PATH), while erofs_ilookup() walks from the volume root. */
typedef struct uboot_erofs_ino {
    br_inode_t         head;                          /* common head (must be first field) */
    struct erofs_inode vi;                            /* EROFS inode parsed this time */
    char               path[UBOOT_EROFS_REL_PATH_MAX]; /* relative to mount point; root = "" */
} uboot_erofs_ino_t;

/* Node identity via head.fpriv (as fs/tmpfs does): no extra cross-plugin symbol. */
static uboot_erofs_ino_t *uboot_erofs_wrap(const br_inode_t *ino)
{
    return (uboot_erofs_ino_t *)(void *)ino->fpriv;
}

static uboot_erofs_ino_t *uboot_erofs_session(br_file_t *f)
{
    return (uboot_erofs_ino_t *)br_file_fpriv(f);
}

/* Directory iterator, kept on the file handle's fpriv (the same directory can be open for
 * iteration many times at once); dir_open allocates it, dir_close frees it. It caches one
 * directory block: `blk` = block number loaded in s_dir_blk (4 KiB units from the file start),
 * `de_end` = end of its dirent array (= name area start), `max` = valid byte count (the last
 * block may be short). `pos` is the byte offset within the directory file, i.e. the next dirent. */
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

/* Directory block buffer, a static 4 KiB separate from the shared scratch: a directory read
 * goes through erofs_pread(), which uses the scratch for map/index parsing and would overwrite
 * its own block contents. Cost is 4 KiB of static data (stack is not an option here). */
static br_u32 s_dir_blk_words[EROFS_MAX_BLOCK_SIZE / 4u];

static u8 *uboot_erofs_dir_blk(void)
{
    return (u8 *)(void *)s_dir_blk_words;
}

/* ---- type translation: EROFS dirent ftype <-> brickOS inode type ---- */

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

/* Private-tail entry, not br_inode_priv(): with head first both are at the same address, but
 * fpriv is the carrier the vfs hands to the file face (avoids one cross-plugin symbol). */
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
/* rel_path is a C string we built; copy by length, same discipline as on-disk names. */
    uboot_erofs_copy_n(n->path, (br_size_t)sizeof(n->path), rel_path,
                       uboot_erofs_strnlen(rel_path, (br_size_t)sizeof(n->path)));
    return n;
}

/* Relative path join: parent + '/' + name (empty parent -> just the name). -ENOSPC if it does
 * not fit; the caller already bounds the name, so only the total length is checked. */
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
/* Join by length, not via a NUL terminator: a future non-C-string caller cannot overrun. */
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

/* ---- layer 3: file face ---- */

static int uboot_erofs_file_open(br_file_t *f, br_u32 flags)
{
    const uboot_erofs_ino_t *n = uboot_erofs_session(f);

    if ((n == BR_NULL) || (f == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    if (n->head.type == BR_INODE_DIR) {
        return BR_ERR(EISDIR);
    }
/* Write-open fails here, not at the first write: the caller gets -EROFS immediately. */
    if (BR_O_WRITABLE(flags) || ((flags & BR_O_TRUNC) != 0u) || ((flags & BR_O_CREAT) != 0u)) {
        return BR_ERR(EROFS);
    }
    return 0;
}

static br_s64 uboot_erofs_file_read(br_file_t *f, void *buf, br_size_t n)
{
/* `node` not `n`: the signature already uses `n` for the byte count. */
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

/* Read-only: write slot filled (more accurate than empty), always -EROFS. */
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
/* Past-EOF seek allowed (POSIX), reads return 0; __builtin_add_overflow avoids UB. */
    if (__builtin_add_overflow(base, off, &pos) || (pos < 0)) {
        return BR_ERR(EINVAL);
    }
    br_file_set_offset(f, pos);
    return pos;
}

static int uboot_erofs_file_close(br_file_t *f)
{
/* Nothing to release: fpriv is the node, freed via free_inode (the dir face must br_free). */
    (void)f;
    return 0;
}

/* ---- layer 3: directory face (read returns exactly one br_dirent_t) ---- */

/* Fill one fixed-size directory entry. Copy by length, never "scan until NUL": EROFS dirent
 * names are packed with no NUL terminator. Zero the entry first so no stale bytes leak out. */
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

/* Load directory block `blk` if not buffered; returns its valid byte count, or negative errno.
 * Layout: [0, de_end) is the contiguous dirent array (12 B each, sorted by name), [de_end, max)
 * the name area. de_end is the first dirent's nameoff -- EROFS uses it as "length of this
 * block's dirent array", so in-block idx = offset/12 and namelen = next entry's nameoff - this
 * one's (the last entry scans to NUL). */
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

/* Byte offset of entry `idx` within the block; -1 if past the end of the array. */
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
/* Fixed-size records cannot be truncated: too little room -> -EINVAL (0 reads as "exhausted",
 * half an entry would violate the FS contract). The vfs always passes sizeof(br_dirent_t). */
    if ((buf == BR_NULL) || (n < (br_size_t)sizeof(br_dirent_t))) {
        return BR_ERR(EINVAL);
    }

/* "." and ".." are synthesized: EROFS dir blocks do not contain them (as upstream). */
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

/* The dirent array ends at `de_end` (the first dirent's nameoff): at or past it we are already
 * in the name area, so skip to the next block; otherwise in_blk must land on a 12-byte boundary
 * of the array. */
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

/* Advance the cursor whether or not this entry is accepted. */
        it->pos = (erofs_off_t)blk * (erofs_off_t)erofs_blksiz() +
                  (erofs_off_t)(in_blk + (br_u32)sizeof(struct erofs_dirent));

        if (nameoff >= it->max) {
            erofs_err("bogus dirent nameoff %u @ nid %llu", nameoff,
                      (unsigned long long)it->ino->vi.nid);
            return BR_ERR(EIO);
        }

        if (next_off < 0) {
/* Last entry of the block: scan to NUL, bounded by the block's valid bytes. */
            de_namelen = (br_u32)uboot_erofs_strnlen(de_name,
                                                     (br_size_t)(it->max - nameoff));
        } else {
            const struct erofs_dirent *next =
                (const struct erofs_dirent *)(const void *)
                (uboot_erofs_dir_blk() + (br_u32)next_off);
            const br_u32 next_nameoff = (br_u32)le16_to_cpu(next->nameoff);

            de_namelen = (next_nameoff > nameoff) ? (next_nameoff - nameoff) : 0u;
        }

/* Corrupt entry: same criterion as upstream. */
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
/* Over-long name: skip and count it, so one bad name cannot fail the whole readdir. */
            it->skipped++;
            continue;
        }

        uboot_erofs_dirent_fill(de, type, de_name, de_namelen);
        return (br_s64)sizeof(br_dirent_t);
    }

    if (it->skipped != 0u) {
/* Report once, at exhaustion, so repeated reads do not flood the log. */
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

/* ---- ops tables ---- */

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

/* Layer 2: all 8 namespace-mutation slots are filled and always return -EROFS (more accurate
 * than the -ENOTSUP an empty slot gives; readlink is read-only and supported). One shell per
 * signature because C types differ by parameter list, all with the same body. */
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

/* Layer 2's other tables: non-directory nodes also need an iops, since getattr/setattr are
 * inode-level and every inode supporting br_stat must carry one (iops only on directories made
 * br_stat on a file return -ENOTSUP). Three tables: s_iops for directories (namespace face +
 * getattr), s_file_iops for regular files (getattr only), s_link_iops for symlinks (+ readlink). */
static const br_inode_ops_t s_file_iops = {
    .getattr  = uboot_erofs_getattr,
/* .setattr must be filled too: br_truncate() resolves the leaf and calls its own iops->setattr,
 * so an empty slot here yields the vfs's -ENOTSUP instead of -EROFS (TC-EROFS-007 caught this). */
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

/* ---- layer 2 implementation ---- */

/* lookup: "parent inode + name" -> path relative to the mount point, handed to upstream's path
 * walk. Miss / over-long name / unknown type converge to BR_NULL, which the vfs resolve_norm
 * reads as "does not exist" (-ENOENT). lookup returns a pointer, so it has no error channel:
 * the -ENOSPC for an over-long component lives in the vfs itself. */
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
/* Name too long (>= BR_NAME_MAX): treated as "does not exist". */
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
/* Every node needs getattr (see the note above). */
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

/* Directory size reports as 0 (br_stat_t.size is meaningless for dirs); a link's size is its
 * target length, which is i_size, without expanding the target or reading it out. */
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

/* readlink: the target lives in the inode's data face (erofs_pread). Returns the full length
 * (truncation is not an error); cap == 0 / buf == NULL just report it; non-link -> -EINVAL. */
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

/* Read only cnt bytes: the target may exceed cap and overrun the caller's buffer. */
    rc = erofs_pread((struct erofs_inode *)&n->vi, buf, (erofs_off_t)cnt, 0);
    if (rc < 0) {
        return rc;
    }
    buf[cnt] = '\0';
    return (int)len;                        /* contract: return the full length */
}

/* ---- layer 1 implementation: mount / free_inode / sync ---- */

static void uboot_erofs_free_inode(br_inode_t *ino)
{
/* Transient node: just a wrapper, no subobjects. The root inode is never passed here. */
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

/* Mount point and source device are both decided by the APP (via cfg), so only non-null is
 * required here: an EROFS volume relates to the medium, not to where it is mounted. */
    if ((cfg->path == BR_NULL) || (cfg->dev == BR_NULL)) {
        br_log_error("uboot-erofs: incomplete mount args (path=%s dev=%s)",
                     (cfg->path != BR_NULL) ? cfg->path : "(null)",
                     (cfg->dev != BR_NULL) ? cfg->dev : "(null)");
        return BR_ERR(EINVAL);
    }

/* 1) block device: fetch the bdev by the APP's source name and remember the geometry. */
    rc = uboot_erofs_bdev_bind(cfg->dev);
    if (rc != 0) {
        return rc;                          /* first failure stops boot (see brick.toml) */
    }

/* 2) superblock: the magic must be at offset 1024 (EROFS_SUPER_OFFSET). */
    rc = erofs_read_superblock();
    if (rc != 0) {
        return rc;
    }
    if (erofs_blksiz() != EROFS_MAX_BLOCK_SIZE) {
/* The compressed path supports PAGE_SIZE blocks only and this file's static buffers are sized
 * for 4 KiB, so require 4 KiB uniformly (the uncompressed face alone allows 512/1024/2048). */
        br_log_error("uboot-erofs: only %u-byte blocks supported (image blkszbits=%u)",
                     (unsigned)EROFS_MAX_BLOCK_SIZE, (unsigned)sbi.blkszbits);
        return BR_ERR(ENOTSUP);
    }

/* 3) root inode (empty path = volume root). */
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

/* 4) root inode: held long-term by the mount table, br_malloc'd, never freed. */
    r = uboot_erofs_new(&vi, "", &s_dir_fops);
    if (r == BR_NULL) {
        return BR_ERR(ENOMEM);
    }
    r->head.iops = &s_iops;
    *root = &r->head;

/* Gate contract: tests/gates.toml's erofs-test requires the substring `uboot-erofs: /assets
 * ready`, so check that gate before changing the wording. */
    br_log_info("uboot-erofs: %s ready (%s; %s; %llu blocks x %u B; root_nid=%llu; %s)",
                cfg->path, BR_EROFS_TYPE_NAME, BR_EROFS_DEV_NAME,
                (unsigned long long)sbi.total_blocks,
                (unsigned)erofs_blksiz(),
                (unsigned long long)sbi.root_nid,
                (sbi.feature_incompat & EROFS_FEATURE_INCOMPAT_ZERO_PADDING)
                    ? "LZ4/0padding" : "LZ4");
    return 0;
}

/* ---- lifecycle hooks (symbol_prefix = uboot_erofs) ---- */

int uboot_erofs_early_init(void)
{
    return 0;   /* no heap, no block device yet: the real work is all in start */
}

int uboot_erofs_init(void)
{
    /* Mounting happens in start; the [[dep]] init edge guarantees uboot-mmc's start ran first. */
    return 0;
}

int uboot_erofs_start(void)
{
    /* Register the driver only; where and from which card to mount is the APP's decision. */
    const int rc = br_fs_driver_register("erofs", &s_fs_ops, BR_NULL);

    if (rc != 0) {
        br_log_error("uboot-erofs: driver 'erofs' registration failed rc=%d (first failure stops boot)", rc);
        return rc;
    }
    return 0;
}
