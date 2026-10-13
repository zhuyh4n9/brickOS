/*
 * adapter/uboot-fat - block-layer shim, volume table, BPB observation, and the environment
 * functions this plugin must implement. Layering and ownership are described in fat_glue.h.
 *
 * blk_dread/blk_dwrite land on br_bdev_ops_t.read/write, chunked by geometry.max_rw with
 * out-of-range rejected; the volume table turns upstream's static cur_dev into s_cur_vol and
 * binds through upstream's own fat_set_blk_dev() (whole device = one partition), so no upstream
 * line changes.
 *
 * Glue parses the BPB (read-only, from the vendored boot_sector) because upstream's
 * get_fs_info()/determine_legacy_fat_bits() are static with no observation point; it feeds
 * criteria and logs only, takes no part in any I/O decision, and a disagreement shows up as a
 * red mount criterion, never a bad read.
 */
#include "fat_glue.h"

/* On-disk structures (boot_sector/dir_entry/MAX_LFN_SLOT/...); compat's <fat.h> forwards to the
 * vendor include/fat.h, so we copy not one field. */
#include <fat.h>

/* ---- volume table and current volume ----
 * Same shape as upstream's global cur_dev/cur_part_info: a current-volume pointer plus the
 * struct blk_desc/disk_partition built from it. Mounting and selftest are single-threaded, so
 * no locking (the v1 assumption shared with cdev/bdev; the gap is br-wa-boot-002). */

static uboot_fat_vol_t  s_vols[UB_FAT_VOL_MAX];
static uboot_fat_vol_t *s_cur_vol;

/* "whole device is one partition" descriptor + partition entry (refreshed per bind) */
static struct blk_desc       s_desc;
static struct disk_partition s_part;

uboot_fat_vol_t *uboot_fat_vol_get(br_u32 id)
{
    if (id >= (br_u32)UB_FAT_VOL_MAX) {
        return BR_NULL;
    }
    return &s_vols[id];
}

/* Volume sector -> device lba. Identity while the whole device is one partition (start = 0), but
 * kept so the partition offset stays visible (upstream writes cur_part_info.start + block). */
static br_u64 uboot_fat_lba_of(br_u32 block)
{
    return ((br_u64)s_part.start) + (br_u64)block;
}

/* ---- block layer (shim) ----
 * Upstream criterion: success = sectors transferred (== requested), failure < 0. Added here:
 * chunking by geometry.max_rw (upstream has no such rule) and out-of-range rejection (the whole
 * volume is one partition, so [start, start + size) is legal). A failing chunk returns -errno,
 * so upstream's `ret != nr_blocks` test still holds. */
static unsigned long uboot_fat_blk_rw(lbaint_t start, lbaint_t blkcnt, void *buf, int write)
{
    uboot_fat_vol_t *vol = s_cur_vol;
    br_u32 chunk;
    br_u64 lba;
    br_u32 left;
    br_u8 *p = (br_u8 *)buf;

    if (vol == BR_NULL) {
        br_log_error("uboot-fat: blk_%s with no volume bound", (write != 0) ? "write" : "read");
        return (unsigned long)-1;
    }
    if (blkcnt == 0u) {
        return 0u;
    }
    if (vol->ops == BR_NULL) {
        return (unsigned long)-1;
    }
    if ((br_u64)start >= vol->geom.n_sectors) {
        br_log_error("uboot-fat: %s lba %llu out of volume (n_sectors=%llu)",
                     (write != 0) ? "write" : "read", (unsigned long long)start,
                     (unsigned long long)vol->geom.n_sectors);
        return (unsigned long)-1;
    }
    lba = uboot_fat_lba_of((br_u32)start);
    if (((br_u64)start + (br_u64)blkcnt) > vol->geom.n_sectors) {
        br_log_error("uboot-fat: %s %llu sectors at lba %llu crosses volume end",
                     (write != 0) ? "write" : "read", (unsigned long long)blkcnt,
                     (unsigned long long)start);
        return (unsigned long)-1;
    }

    chunk = (vol->geom.max_rw != 0u) ? (br_u32)vol->geom.max_rw : (br_u32)blkcnt;
    if (chunk > blkcnt) {
        chunk = (br_u32)blkcnt;
    }
    left = (br_u32)blkcnt;
    while (left > 0u) {
        const br_u32 n = (left < chunk) ? left : chunk;
        int rc;

        if (write != 0) {
            rc = vol->ops->write(vol->priv, lba, p, (br_size_t)n);
        } else {
            rc = vol->ops->read(vol->priv, lba, p, (br_size_t)n);
        }
        /* bdev contract: success = sectors transferred; a short transfer counts as a device error
         * here (upstream decides via `ret != nr_blocks`). */
        if (rc != (int)n) {
            br_log_error("uboot-fat: %s lba %llu n=%u rc=%d",
                         (write != 0) ? "write" : "read", (unsigned long long)lba, n, rc);
            return (unsigned long)-1;
        }
        lba  += n;
        p    += (br_size_t)n * UB_FAT_SECTOR_SIZE;
        left -= n;
    }
    return (unsigned long)blkcnt;
}

unsigned long blk_dread(struct blk_desc *desc, lbaint_t start, lbaint_t blkcnt, void *buf)
{
    (void)desc;                     /* one "current volume" only; the descriptor is just shape */
    return uboot_fat_blk_rw(start, blkcnt, buf, 0);
}

unsigned long blk_dwrite(struct blk_desc *desc, lbaint_t start, lbaint_t blkcnt, const void *buf)
{
    (void)desc;
    return uboot_fat_blk_rw(start, blkcnt, (void *)(br_uintptr_t)buf, 1);
}

/* FAT type detection, same shape as upstream's static determine_legacy_fat_bits() (the Linux
 * fat_fill_super() algorithm). The FAT32 branch follows read_bootsectandvi(): fat_length == 0
 * && fat32_length != 0. Returns 0 for a malformed BPB so the mount criteria refuse it. */
static int uboot_fat_detect_bits(const boot_sector *bs)
{
    const u32 fat_start = bs->reserved;              /* already FAT2CPU'd by the caller */
    const u32 dir_start = fat_start + ((u32)bs->fats * bs->fat_length);
    const u32 rootdir_sectors = get_unaligned_le16(bs->dir_entries) *
                                sizeof(dir_entry) /
                                get_unaligned_le16(bs->sector_size);
    const u32 data_start = dir_start + rootdir_sectors;
    const u16 sectors = get_unaligned_le16(bs->sectors);
    const u32 total_sectors = (sectors != 0u) ? (u32)sectors : bs->total_sect;
    u32 total_clusters;
    u32 span;

    if ((bs->fat_length == 0u) && (bs->fat32_length != 0u)) {
        return 32;
    }
    if (bs->cluster_size == 0u) {
        return 0;                       /* malformed: let the mount criteria refuse it */
    }
    if (total_sectors <= data_start) {
        return 0;
    }
    span = total_sectors - data_start;
    total_clusters = span / bs->cluster_size;
    return (total_clusters > UB_FAT_MAX_FAT12) ? 16 : 12;
}

/* Read sector 0 and fill the observation fields from the vendored boot_sector; 0 / -errno. */
static int uboot_fat_vol_probe(uboot_fat_vol_t *vol)
{
    unsigned char sec[UB_FAT_SECTOR_SIZE];
    boot_sector bs;
    int rc;
    u16 sect_size;
    u32 fatlength;
    u32 total_sect;

    if ((vol->ops == BR_NULL) || (vol->geom.sector_size != UB_FAT_SECTOR_SIZE)) {
        return BR_ERR(EINVAL);
    }
    rc = vol->ops->read(vol->priv, 0u, sec, 1u);
    if (rc != 1) {
        br_log_error("uboot-fat: %s: failed to read boot sector rc=%d", vol->dev, rc);
        return (rc < 0) ? rc : BR_ERR(EIO);
    }

    memset(&bs, 0, sizeof(bs));
    memcpy(&bs, sec, sizeof(bs));

    /* As upstream read_bootsectandvi(): the struct holds raw on-disk bytes, so __u16/__u32 fields
     * need a FAT2CPU pass (identity on LE, see compat/fat_env.h) and the __u8[2] fields (sector
     * size / dir entries / sector count) are read with get_unaligned_le16. */
    bs.reserved    = FAT2CPU16(bs.reserved);
    bs.fat_length  = FAT2CPU16(bs.fat_length);
    bs.total_sect  = FAT2CPU32(bs.total_sect);

    sect_size = FAT2CPU16(get_unaligned_le16(bs.sector_size));
    if (sect_size != vol->geom.sector_size) {
        br_log_error("uboot-fat: %s: BPB sector size %u != device %u",
                     vol->dev, (unsigned)sect_size, (unsigned)vol->geom.sector_size);
        return BR_ERR(EINVAL);
    }
    if (bs.cluster_size == 0u) {
        br_log_error("uboot-fat: %s: BPB cluster size not set", vol->dev);
        return BR_ERR(EINVAL);
    }

    vol->sect_size = sect_size;
    vol->clust_size = bs.cluster_size;
    vol->fats = bs.fats;
    vol->fat_sect = bs.reserved;
    vol->fatsize = uboot_fat_detect_bits(&bs);
    if (vol->fatsize == 0) {
        br_log_error("uboot-fat: %s: BPB self-contradictory (cannot determine FAT type)", vol->dev);
        return BR_ERR(EINVAL);
    }

    if (vol->fatsize == 32) {
        fatlength = FAT2CPU32(bs.fat32_length);
        total_sect = bs.total_sect;
        vol->root_cluster = FAT2CPU32(bs.root_cluster);
        vol->rootdir_size = 0u;
    } else {
        fatlength = bs.fat_length;
        total_sect = FAT2CPU16(get_unaligned_le16(bs.sectors));
        if (total_sect == 0u) {
            total_sect = bs.total_sect;
        }
        vol->root_cluster = 0u;
        vol->rootdir_size = (FAT2CPU16(get_unaligned_le16(bs.dir_entries)) *
                             (u32)sizeof(dir_entry)) / sect_size;
    }
    if (total_sect == 0u) {
        total_sect = (u32)vol->geom.n_sectors;   /* same fallback as upstream */
    }
    vol->fatlength = fatlength;
    vol->total_sect = total_sect;
    vol->rootdir_sect = vol->fat_sect + (fatlength * vol->fats);

    if (vol->fatsize == 32) {
        /* upstream: data_begin = rootdir_sect - clust_size*2 (clusters start at 2) */
        vol->data_begin = vol->rootdir_sect - (vol->clust_size * 2u);
    } else {
        /* Non-FAT32 root dir is not on a cluster chain, so it can be a "negative cluster". */
        vol->data_begin = vol->rootdir_sect + vol->rootdir_size -
                          (vol->clust_size * 2u);
    }
    return 0;
}

/* ---- volume bind (where fat_set_blk_dev lives) ----
 * Fetch bdev geometry and check it (512 B sectors, non-zero sector count), fill the shim's struct
 * blk_desc/disk_partition (whole device = one partition), call upstream's own fat_set_blk_dev()
 * -- boot-sector magic and signature stay upstream's criteria -- then re-read the BPB for the
 * observation values. Returns 0 or -errno; failure leaves no half-bound state. */
int uboot_fat_vol_bind(uboot_fat_vol_t *vol)
{
    int rc;

    if (vol == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (vol->bound == BR_TRUE) {
        /* Already bound: make it current again. Upstream knows only the current volume (fat.c's
         * static cur_dev), so every entry must re-bind, or this volume would be read with the
         * previous volume's FAT parameters. */
        s_cur_vol = vol;
        s_desc.lba     = (lbaint_t)vol->geom.n_sectors;
        s_desc.blksz   = (unsigned long)vol->geom.sector_size;
        s_part.start   = 0u;
        s_part.size    = (lbaint_t)vol->geom.n_sectors;
        s_part.blksz   = (ulong)vol->geom.sector_size;
        /* Re-run fat_set_blk_dev() even when bound: it refreshes upstream's cur_dev/cur_part_info
         * (their only consumer) and re-verifies the boot sector magic, so swapped media is not
         * swallowed. One 512 B read per entry; failure leaves no half-bound state. */
        if (fat_set_blk_dev(&s_desc, &s_part) != 0) {
            br_log_error("uboot-fat: %s: rebind failed (invalid boot sector)", vol->dev);
            s_cur_vol = BR_NULL;
            return BR_ERR(EIO);
        }
        return 0;
    }

    vol->ops  = br_bdev_get(vol->dev, &vol->priv);
    vol->geom = (br_bdev_geom_t){ 0u, 0u, 0u, 0u, 0u };
    if ((vol->ops == BR_NULL) || (vol->priv == BR_NULL)) {
        br_log_error("uboot-fat: bdev %s not found", vol->dev);
        return BR_ERR(ENODEV);
    }
    rc = vol->ops->geometry(vol->priv, &vol->geom);
    if (rc != 0) {
        br_log_error("uboot-fat: geometry(%s) failed (%d)", vol->dev, rc);
        return rc;
    }
    if ((vol->geom.sector_size != UB_FAT_SECTOR_SIZE) || (vol->geom.n_sectors == 0u)) {
        br_log_error("uboot-fat: %s sector_size=%u n_sectors=%llu (512 B sectors required)",
                     vol->dev, (unsigned)vol->geom.sector_size,
                     (unsigned long long)vol->geom.n_sectors);
        return BR_ERR(EINVAL);
    }

    /* whole device as one partition: start = 0, size = device sector count */
    memset(&s_desc, 0, sizeof(s_desc));
    memset(&s_part, 0, sizeof(s_part));
    s_desc.uclass_id = 0;
    s_desc.devnum    = 0;
    s_desc.lba       = (lbaint_t)vol->geom.n_sectors;
    s_desc.blksz     = (unsigned long)vol->geom.sector_size;
    s_part.start     = 0u;
    s_part.size      = (lbaint_t)vol->geom.n_sectors;
    s_part.blksz     = (ulong)vol->geom.sector_size;

    s_cur_vol = vol;                    /* all subsequent I/O lands on this volume */
    if (fat_set_blk_dev(&s_desc, &s_part) != 0) {
        br_log_error("uboot-fat: %s: not a valid FAT boot sector", vol->dev);
        s_cur_vol = BR_NULL;
        return BR_ERR(EINVAL);
    }
    rc = uboot_fat_vol_probe(vol);
    if (rc != 0) {
        s_cur_vol = BR_NULL;
        return rc;
    }
    vol->bound = BR_TRUE;
    return 0;
}

/* Unbind the current volume (idempotent). The mount-failure rollback path also uses it. */
void uboot_fat_vol_unbind(void)
{
    if (s_cur_vol != BR_NULL) {
        s_cur_vol->bound = BR_FALSE;
    }
    s_cur_vol = BR_NULL;
}

/* (source device, mount point) -> volume id: claim a slot. The pair comes from the APP's mount
 * plan, not a plugin table; claiming the same (dev, mnt) twice reuses the slot (also blocking a
 * double mount), and no free slot is -ENOSPC, bounded by UB_FAT_VOL_MAX. */
int uboot_fat_vol_claim(const char *dev, const char *mnt, br_u32 *id)
{
    if ((dev == BR_NULL) || (mnt == BR_NULL) || (id == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* 1 same pair already claimed => reuse (idempotent) */
    for (br_u32 i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
        if ((s_vols[i].dev != BR_NULL) && (s_vols[i].mnt != BR_NULL) &&
            (uboot_fat_strcmp(s_vols[i].dev, dev) == 0) &&
            (uboot_fat_strcmp(s_vols[i].mnt, mnt) == 0)) {
            *id = i;
            return 0;
        }
    }
    /* 2 otherwise take a free slot */
    for (br_u32 i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
        if (s_vols[i].dev == BR_NULL) {
            s_vols[i].dev = dev;
            s_vols[i].mnt = mnt;
            *id = i;
            return 0;
        }
    }
    return BR_ERR(ENOSPC);
}

/* Volume observation for the mount criteria and TC-FAT-001; read-only, changes no state. */
int uboot_fat_vol_info(br_u32 id, uboot_fat_vol_info_t *out)
{
    uboot_fat_vol_t *v = uboot_fat_vol_get(id);

    if ((v == BR_NULL) || (out == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    out->dev                = v->dev;
    out->mnt                = v->mnt;
    out->sector_size        = (v->bound == BR_TRUE) ? v->sect_size : 0u;
    out->n_sectors          = (v->bound == BR_TRUE) ? (br_u64)v->total_sect : 0u;
    out->max_rw             = v->geom.max_rw;
    out->clust_size_sectors = (v->bound == BR_TRUE) ? v->clust_size : 0u;
    out->max_clust          = (v->bound == BR_TRUE && v->clust_size != 0u)
                              ? (v->total_sect / v->clust_size) : 0u;
    out->fatsize            = (v->bound == BR_TRUE) ? v->fatsize : 0;
    out->mounted            = v->bound;
    return 0;
}

/* ---- environment functions we implement ---- */

/* strdup for upstream's write path (no libc), bounded by UB_FAT_PATH_MAX (compat/fat_env.h).
 * Too long and out of memory both return NULL, which upstream's call sites turn into -ENOMEM. */
char *uboot_fat_strdup(const char *s)
{
    br_size_t n;
    char *p;

    if (s == BR_NULL) {
        return BR_NULL;
    }
    n = uboot_fat_strlen(s);
    if (n >= UB_FAT_STRDUP_CAP) {
        br_log_error("uboot-fat: path over %u bytes, refusing to copy", (unsigned)(UB_FAT_STRDUP_CAP - 1u));
        return BR_NULL;
    }
    p = (char *)br_malloc(n + 1u);
    if (p == BR_NULL) {
        return BR_NULL;
    }
    memcpy(p, s, n + 1u);
    return p;
}

/* Bounded sprintf (no libc), covering every specifier at upstream's four call sites (%.*s, %.3s,
 * %d, %02X, %%). Digits are generated in reverse, overflow discarded. Returns the sprintf
 * convention and always NUL-terminates (bare metal has no __builtin_sprintf). */
int uboot_fat_sprintf(char *dst, const char *fmt, ...)
{
    br_size_t n = 0u;
    br_size_t i = 0u;
    __builtin_va_list ap;

    if (dst == BR_NULL) {
        return 0;
    }
    __builtin_va_start(ap, fmt);
    while (fmt[i] != '\0') {
        int zero_pad = 0;
        int width = 0;
        int prec = -1;

        if (fmt[i] != '%') {
            if ((n + 1u) < UB_FAT_SPRINTF_MAX) {
                dst[n] = fmt[i];
            }
            n++;
            i++;
            continue;
        }
        i++;                                        /* skip '%' */
        if (fmt[i] == '0') {                        /* '0' flag supported only for `%02X` */
            zero_pad = 1;
            i++;
        }
        while ((fmt[i] >= '0') && (fmt[i] <= '9')) {
            width = (width * 10) + (int)(fmt[i] - '0');
            i++;
        }
        if (fmt[i] == '.') {
            i++;
            if (fmt[i] == '*') {                    /* `%.*s` (two upstream sites) */
                prec = __builtin_va_arg(ap, int);
                i++;
            } else {
                prec = 0;
                while ((fmt[i] >= '0') && (fmt[i] <= '9')) {
                    prec = (prec * 10) + (int)(fmt[i] - '0');
                    i++;
                }
            }
        }

        if (fmt[i] == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            br_size_t k = 0u;

            i++;
            if (s == BR_NULL) {
                s = "(null)";
            }
            while ((s[k] != '\0') && ((prec < 0) || (k < (br_size_t)prec))) {
                if ((n + 1u) < UB_FAT_SPRINTF_MAX) {
                    dst[n] = s[k];
                }
                n++;
                k++;
            }
        } else if ((fmt[i] == 'd') || (fmt[i] == 'u') || (fmt[i] == 'x') || (fmt[i] == 'X')) {
            const int as_signed = (fmt[i] == 'd');
            const int as_hex    = ((fmt[i] == 'x') || (fmt[i] == 'X'));
            const int upper     = (fmt[i] == 'X');
            const unsigned long long uv = as_signed
                ? (unsigned long long)(long long)__builtin_va_arg(ap, int)
                : (unsigned long long)__builtin_va_arg(ap, unsigned int);
            char tmp[24];
            br_size_t tn = 0u;

            i++;
            if (as_signed && ((long long)uv < 0)) {
                if ((n + 1u) < UB_FAT_SPRINTF_MAX) {
                    dst[n] = '-';
                }
                n++;
            }
            /* generate in reverse, then emit forward after padding to width ('0'/space fill) */
            {
                unsigned long long v = (as_signed && ((long long)uv < 0))
                                     ? (~uv + 1ull) : uv;
                const unsigned base = as_hex ? 16u : 10u;

                do {
                    const unsigned d = (unsigned)(v % base);
                    tmp[tn++] = (char)((d < 10u) ? ('0' + (char)d)
                                                : ((upper ? 'A' : 'a') + (char)(d - 10u)));
                    v /= base;
                } while ((v != 0u) && (tn < sizeof(tmp)));
            }
            for (int w = width; w > (int)tn; w--) {
                if ((n + 1u) < UB_FAT_SPRINTF_MAX) {
                    dst[n] = zero_pad ? '0' : ' ';
                }
                n++;
            }
            while (tn > 0u) {
                tn--;
                if ((n + 1u) < UB_FAT_SPRINTF_MAX) {
                    dst[n] = tmp[tn];
                }
                n++;
            }
        } else if (fmt[i] == '%') {
            if ((n + 1u) < UB_FAT_SPRINTF_MAX) {
                dst[n] = '%';
            }
            n++;
            i++;
        } else {
            /* Unknown specifier: emit it as-is rather than silently dropping part of a log/name. */
            if ((n + 1u) < UB_FAT_SPRINTF_MAX) {
                dst[n] = '%';
            }
            n++;
            if (fmt[i] != '\0') {
                if ((n + 1u) < UB_FAT_SPRINTF_MAX) {
                    dst[n] = fmt[i];
                }
                n++;
                i++;
            }
        }
    }
    __builtin_va_end(ap);
    dst[(n < (br_size_t)UB_FAT_SPRINTF_MAX) ? n : (UB_FAT_SPRINTF_MAX - 1u)] = '\0';
    return (int)n;
}

/* rand() for upstream set_name() (search start of the "~N" short-name suffix). Determinism keeps
 * the selftest reproducible; only the search order changes. LCG from Numerical Recipes. */
static unsigned int s_rand_state = 0x12345678u;

int uboot_fat_rand(void)
{
    s_rand_state = (s_rand_state * 1664525u) + 1013904223u;
    return (int)(s_rand_state >> 1);
}

/* ---- truncate (upstream public API only) ---- */

/*
 * Truncate/extend to `newsize`, built from upstream public write semantics (upstream has no
 * truncate entry). shrink/to zero: file_fat_write_at(path, newsize, NULL, 0, &act) sets the dirent
 * size and takes the "truncate the rest" or filesize == 0 branch; get_set_cluster(size == 0)
 * returns immediately, so the NULL buffer is never dereferenced. grow: append newsize - cur
 * explicit zero bytes, 512 B at a time (no hole extension upstream) -- TC-FAT-008 requires the new
 * region to read back as 0, not a previous file's leftovers. Returns 0 / -errno.
 */
int uboot_fat_truncate(const char *path, br_u64 newsize)
{
    loff_t cur = 0;
    loff_t act = 0;
    int rc;

    if ((path == BR_NULL) || (s_cur_vol == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    rc = fat_size(path, &cur);
    if (rc < 0) {
        return rc;
    }
    if (cur < 0) {
        return BR_ERR(EIO);
    }
    if ((br_u64)cur == newsize) {
        return 0;                        /* idempotent: length unchanged, do not touch the medium */
    }

    if (newsize < (br_u64)cur) {
        /* 1/2 shrink or to zero — upstream's own truncate branch */
        rc = file_fat_write_at(path, (loff_t)newsize, BR_NULL, 0, &act);
        return (rc < 0) ? rc : 0;
    }

    /* 3 grow — explicit zero fill (see function header) */
    {
        unsigned char zero[UB_FAT_ZERO_CHUNK];
        loff_t pos = cur;

        memset(zero, 0, sizeof(zero));
        while ((br_u64)pos < newsize) {
            br_u64 left = newsize - (br_u64)pos;
            loff_t chunk = (left > (br_u64)sizeof(zero)) ? (loff_t)sizeof(zero)
                                                        : (loff_t)left;

            act = 0;
            rc = file_fat_write_at(path, pos, zero, chunk, &act);
            if (rc < 0) {
                return rc;
            }
            if (act <= 0) {
                return BR_ERR(EIO);      /* nothing was written: do not pretend success */
            }
            pos += act;
        }
    }
    return 0;
}
