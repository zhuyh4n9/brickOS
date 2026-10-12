/*
 * adapter/uboot-fat — block-layer shim + volume table + BPB observation + the few environment
 * functions we implement (project code)
 *
 * ============================== where this file sits in the layering ==============================
 * Upstream `fs/fat/fat_write.c` (which includes `fat.c`) is **one compilation unit**; all its demands
 * on the environment are satisfied in `compat/fat_env.h`; this file adds the three things that must
 * have a concrete entity on the environment side:
 *   1 `blk_dread/blk_dwrite`: the landing of upstream `disk_read/disk_write` — wired to
 *      `br_bdev_ops_t.read/write`, with chunking by device geometry (max_rw) + out-of-range rejection;
 *   2 volume table and binding: upstream's "current device" is `static struct blk_desc *cur_dev`; this
 *      file turns it into a "current volume" (`s_cur_vol`), and the bind action goes through **upstream's
 *      own** `fat_set_blk_dev()` (whole device as one partition => `start = 0`, `size = n_sectors`) —
 *      so not a single upstream line changes;
 *   3 small utilities declared in the environment header but **implementable only by this plugin**:
 *      `uboot_fat_strdup`/`uboot_fat_sprintf`/`uboot_fat_rand`, plus the truncation public API assembly
 *      `uboot_fat_truncate`.
 *
 * ============================== why glue also parses the BPB ==============================
 * Upstream's `get_fs_info()`/`determine_legacy_fat_bits()` are **static** (private to the single
 * compilation unit) with no external observation point. But this plugin's mount criteria (TC-FAT-001 +
 * mount geometry checks) must answer "is this volume FAT12/16/32, do the cluster counts fall in the
 * defined ranges, is the data area inside the volume". So glue uses the vendored `include/fat.h`
 * `struct boot_sector` (**still the single source of truth** for layout) to parse **read-only** once —
 * it only feeds criteria and logs and **takes no part in any I/O decision** (all I/O is upstream's, and
 * upstream re-parses the BPB on every API call). This is "observation", not "a second implementation":
 *   - the decision algorithm is the same shape as upstream `determine_legacy_fat_bits` (compared line by
 *     line); the provenance is noted in the function header;
 *   - if the observation disagrees with upstream, the symptom is a **red mount-time criterion** (not
 *     "wrong reads/writes"), because reads and writes do not trust it.
 * ==========================================================================
 */
#include "fat_glue.h"

/* On-disk structures (boot_sector/dir_entry/MAX_LFN_SLOT/...): compat's `<fat.h>` forwarding shell
 * => content comes from the vendor `include/fat.h`; we copy not one field. */
#include <fat.h>

/* ==================================================================== volume table and current volume
 *
 * Upstream is "global current device + current partition" (`cur_dev`/`cur_part_info`). This file keeps
 * the **same shape**: a "current volume" pointer plus the `struct blk_desc`/`disk_partition` built from
 * it. Mounting is a one-shot single-threaded action of this plugin (start phase), and selftest runs on
 * the same thread => no locking (the same v1 static assumption as cdev/bdev; the real concurrency gap
 * is registered in WORKAROUNDS.md as br-wa-boot-002).
 */

static uboot_fat_vol_t  s_vols[UB_FAT_VOL_MAX];
static uboot_fat_vol_t *s_cur_vol;

/* "whole device is one partition" descriptor + partition entry (refreshed from volume geometry per bind) */
static struct blk_desc       s_desc;
static struct disk_partition s_part;

uboot_fat_vol_t *uboot_fat_vol_get(br_u32 id)
{
    if (id >= (br_u32)UB_FAT_VOL_MAX) {
        return BR_NULL;
    }
    return &s_vols[id];
}

/*
 * Volume sector number -> device lba. Whole device as one partition (start = 0) => **identity**, but
 * this layer is kept so that "partition offset" stays explicitly visible in the code (upstream writes
 * `cur_part_info.start + block`).
 */
static br_u64 uboot_fat_lba_of(br_u32 block)
{
    return ((br_u64)s_part.start) + (br_u64)block;
}

/* ==================================================================== block layer (shim)
 *
 * Upstream criterion: success = number of sectors transferred (== requested), failure < 0. This file
 * adds two things here:
 *   1 chunk by `geometry.max_rw` (a single I/O never exceeds the bdev limit; upstream has no such rule);
 *   2 out-of-range rejection (whole volume is one partition => the legal lba range is [start, start + size)).
 * Any chunk failing returns a negative errno => upstream's `ret != nr_blocks` criterion still holds.
 */
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
        /* bdev contract: success = number of sectors transferred (**short reads allowed**). A short one
         * is treated as a device error — upstream decides via `ret != nr_blocks`; this file collapses
         * that at the chunking level. */
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
    (void)desc;                     /* this file has only one "current volume"; the descriptor is just shape */
    return uboot_fat_blk_rw(start, blkcnt, buf, 0);
}

unsigned long blk_dwrite(struct blk_desc *desc, lbaint_t start, lbaint_t blkcnt, const void *buf)
{
    (void)desc;
    return uboot_fat_blk_rw(start, blkcnt, (void *)(br_uintptr_t)buf, 1);
}

/* ==================================================================== BPB observation */

/*
 * FAT type detection. **Same shape as upstream `fs/fat/fat.c`'s `determine_legacy_fat_bits()`**
 * (upstream's is static: based on the Linux `fat_fill_super()` algorithm; reimplementing it line by line
 * here is what lets the **mount criteria** obtain the type — see "why glue also parses the BPB" in the
 * file header).
 * The 32-bit branch additionally follows upstream `read_bootsectandvi()`: `fat_length == 0 && fat32_length != 0`.
 */
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

/* Read the boot sector (sector 0) and fill observation values from the vendored `boot_sector` fields.
 * Returns 0 / negative errno. */
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

    /* Same as upstream `read_bootsectandvi()`: the struct holds **raw on-disk bytes**, so __u16/__u32
     * fields need a FAT2CPU pass (identity on little-endian targets, see compat/fat_env.h's #error);
     * __u8[2] fields (sector size / dir entries / sector count) are read as LE, using
     * get_unaligned_le16 as upstream does. */
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
        /* the non-FAT32 root directory is not on a cluster chain, so it may land in a "negative cluster"
         * (upstream's comment says so explicitly) */
        vol->data_begin = vol->rootdir_sect + vol->rootdir_size -
                          (vol->clust_size * 2u);
    }
    return 0;
}

/* ==================================================================== volume bind (where fat_set_blk_dev lives) */

/*
 * Upstream `fat_set_blk_dev(dev_desc, info)`: records the current device + verifies the boot sector
 * magic 0x55AA and the filesystem signature. This file's `uboot_fat_vol_bind(vol)`:
 *   1 fetch bdev geometry and verify (512 B sectors + non-zero sector count);
 *   2 fill the shim's `struct blk_desc`/`disk_partition` from the geometry (whole device as one partition);
 *   3 call **upstream's own** `fat_set_blk_dev()` — boot-sector verification at mount time belongs to
 *     upstream (we do not change its criteria);
 *   4 read the BPB again to fill observation values (for criteria/logs, see the file header).
 * Returns 0 or a negative errno; on failure no half-bound state remains.
 */
int uboot_fat_vol_bind(uboot_fat_vol_t *vol)
{
    int rc;

    if (vol == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (vol->bound == BR_TRUE) {
        /* Already bound: just make it the current volume (equivalent to upstream fat_set_blk_dev's
         * "switch device" semantics).
         * * Key to multi-volume mounting: upstream only knows the "current volume" (fat.c's static
         *   cur_dev), so glue must bind once at **every entry**, otherwise it would read this volume
         *   with the previous volume's FAT parameters. */
        s_cur_vol = vol;
        s_desc.lba     = (lbaint_t)vol->geom.n_sectors;
        s_desc.blksz   = (unsigned long)vol->geom.sector_size;
        s_part.start   = 0u;
        s_part.size    = (lbaint_t)vol->geom.n_sectors;
        s_part.blksz   = (ulong)vol->geom.sector_size;
        /* * Even if already bound, **still** run upstream's `fat_set_blk_dev()`: it refreshes upstream's
         *   two statics (`cur_dev`/`cur_part_info`, this file being their only consumer) and re-verifies
         *   the boot sector magic — the cost is one 512 B read per entry, in exchange for not silently
         *   swallowing "the volume was swapped / media changed". Failure => no half-bound state. */
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

    /* whole device as one partition: start = 0, size = device sector count (same shape as u-boot's part 0 with no partition table) */
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

/*
 * (source device, mount point) -> volume id  — **claim a volume slot**.
 *
 * * ADR-0030: the correspondence is no longer hard-coded in the plugin's table, but given by the **APP's
 *   mount plan**: the APP calls `mount("emmc0", "/fat12", "fat", 0, NULL)`, making (emmc0, /fat12) a pair.
 *   Claiming the same (dev, mnt) twice => reuse the same slot (idempotent; also blocks "mounting the same
 *   volume twice"); out of slots => -ENOSPC (this file's upper bound on concurrently mounted volumes =
 *   UB_FAT_VOL_MAX).
 *
 * * This change eliminates a **two-sources-of-truth** spot: "mount point <-> device" used to be written
 *   both in the plugin's table and in the manifest's [[mount]]; changing one did not error, it only
 *   silently mounted the wrong device (that debt is settled by ADR-0030).
 */
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

/* Factual observation of a volume (for mount criteria + selftest TC-FAT-001): read-only, changes no state. */
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

/* ==================================================================== the environment functions we implement */

/*
 * `strdup` (the upstream write path uses it to copy paths; no libc). Bound see compat/fat_env.h: this
 * plugin's VFS surface limits paths to `UB_FAT_PATH_MAX`. Too long / out of memory both return NULL —
 * upstream's call sites answer NULL with `-ENOMEM` (each checked), so the failure surface matches libc.
 */
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

/*
 * Bounded `sprintf` (no libc; every conversion specifier at upstream's four call sites is covered:
 * `%.*s` / `%.3s` / `%d` / `%02X` / `%%`). Digits are generated in reverse order, overflow discarded;
 * returns "number of chars that should have been written" (sprintf convention), always NUL-terminated.
 * * Why not `__builtin_sprintf`: bare metal has none, and core's `br_log_write` only outputs without
 *   writing back into the buffer => a tiny formatter must live here (the same trade-off as
 *   adapter/uboot-erofs's criterion surface).
 */
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
            /* generate in reverse, then emit forward after padding to width (only '0'/space fill) */
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
            /* unknown conversion specifier: emit it as-is (do not swallow silently, lest "a log/name lost
             * a piece" become a weird symptom) */
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

/*
 * `rand()` (upstream `set_name()` uses it to pick the search start for the "~N" short-name suffix).
 * u-boot's `rand()` has no corresponding vendored original (see NOTICE), and **determinism** is an
 * advantage here: the candidate space is inherently "try 1 upward until no conflict", so the start does
 * not affect the contract of "which name is found" (only the search order), yet it makes the same
 * on-disk state give the same result => selftest is reproducible. The LCG constants come from Numerical
 * Recipes (implementation-independent; only good dispersion is needed).
 */
static unsigned int s_rand_state = 0x12345678u;

int uboot_fat_rand(void)
{
    s_rand_state = (s_rand_state * 1664525u) + 1013904223u;
    return (int)(s_rand_state >> 1);
}

/* ==================================================================== truncate (upstream public API only) */

/*
 * Truncate/extend to `newsize`. Upstream has **no** truncate entry, but its own public write semantics
 * can be assembled into three cases (each checked against the upstream source; see the measured
 * conclusions in ADR-0026 §5):
 *
 *   1 **shrink**: `file_fat_write_at(path, newsize, NULL, 0, &act)` — upstream first sets the directory
 *      entry size to `pos + size` (= newsize), then `set_contents()` takes the "truncate the rest"
 *      branch: `clear_fatent()` returns the tail cluster chain and marks the last cluster EOC.
 *      `get_set_cluster(size == 0)` returns immediately => **the NULL buffer is never dereferenced**
 *      (this is the whole reason "passing NULL is safe").
 *   2 **to zero**: the same call (`newsize == 0`) takes upstream's dedicated `filesize == 0` branch:
 *      clear the whole cluster chain + set the first cluster to 0 + size = 0.
 *   3 **grow**: upstream has no "hole extension" semantics (`set_contents`'s `assert(!pos)`/
 *      `assert(pos == cur_pos)` are dead code, and its cluster-walking path reports "something wrong"
 *      when "pos falls beyond the end of the chain") => extension can only **write zero bytes
 *      explicitly**: append `newsize - cur` zeros from the current length (chunked, 512 B each).
 *      What TC-FAT-008 requires is exactly "the new region reads back as 0" (not leftovers of a previous
 *      file), so this implementation is synonymous with the criterion — we did **not** relax the
 *      criterion, we merely wrote the "grow" branch in a form upstream actually accepts.
 *
 * Return value: 0 / negative errno.
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
