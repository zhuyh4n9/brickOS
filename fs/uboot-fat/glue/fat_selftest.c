/*
 * fs/uboot-fat - conformance cases (TC-FAT-*, 8 total).
 *
 * WORKAROUND(br-wa-test-001): TC-FAT-* are locally numbered -- the group is absent from the design
 * case table; registered with the numbering debt of the other families.
 *
 * The host preloads the three emmc FAT volumes in a post-build gate (README, prerequisite
 * artifacts); the expected files come from the constants in glue/fat_glue.h, shared with APP. This
 * suite never deletes or modifies those host files (tags 2/3 depend on them) and names its own
 * files with the tcN_ prefix. Except TC-FAT-001 (a plugin fact: FAT type and cluster count from the
 * BPB), every case goes through the public VFS surface, so the criterion is "the host's bytes read
 * back through VFS", not "an internal function returned 0". Returns the failure count (0 = green);
 * prints [FATCONF] PASS/FAIL <tag> <desc> per case and [FATCONF] SUMMARY pass=N fail=M.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "fat_glue.h"

int uboot_fat_selftest(void);

static br_u32 s_pass;
static br_u32 s_fail;

/* Per-branch diagnostics: one PASS/FAIL line per case cannot localize a failure, so each failing
 * branch adds "[FATCONF]   <tag> <step> got=<> want=<>" (failure paths only, SUMMARY unaffected). */
static void fat_diag(const char *tag, const char *step, long got, long want)
{
    br_log_info("[FATCONF]   %s %s got=%ld want=%ld", tag, step, got, want);
}

static void fat_diag_str(const char *tag, const char *step, const char *got, const char *want)
{
    br_log_info("[FATCONF]   %s %s got='%s' want='%s'", tag, step,
                (got != BR_NULL) ? got : "(null)", (want != BR_NULL) ? want : "(null)");
}

static void fat_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[FATCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[FATCONF] FAIL %s %s", tag, what);
    }
}

static br_bool fat_bytes_eq(const br_u8 *a, const br_u8 *b, br_size_t n)
{
    for (br_size_t i = 0u; i < n; i++) {
        if (a[i] != b[i]) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

/* Build the expected content of a "host-preloaded file": base is byte 0, then 0..9 repeating. */
static void fat_make_pattern(br_u8 *dst, br_size_t n, br_u8 base)
{
    if (n > 0u) {
        dst[0] = base;
    }
    for (br_size_t i = 1u; i < n; i++) {
        dst[i] = (br_u8)('0' + (br_u8)(i % 10u));
    }
}

/* Write a new file (creating it if absent). The caller supplies the criterion separately. */
static int fat_write_all(const char *path, const void *buf, br_size_t n)
{
    br_file_t *f = br_open(path, (br_u32)(BR_O_WRONLY | BR_O_CREAT | BR_O_TRUNC));
    br_s64 w;

    if (f == BR_NULL) {
        return BR_ERR(EIO);
    }
    w = br_file_write(f, buf, n);
    (void)br_file_close(f);
    if (w != (br_s64)n) {
        return BR_ERR(EIO);
    }
    return 0;
}

/* Read a file back and compare byte by byte with the expected host content. want_len == 0 means the
 * expected length is strlen(want). Reads in chunks and requires exactly len bytes plus EOF. */
static br_bool fat_read_verify(const char *path, const br_u8 *want, br_size_t want_len)
{
    br_u8 buf[64];
    br_stat_t st;
    br_size_t len = want_len;
    br_size_t got = 0u;
    br_file_t *f;

    if (br_stat(path, &st) != 0) {
        return BR_FALSE;
    }
    if (st.type != BR_INODE_FILE) {
        return BR_FALSE;
    }
    if (want_len == 0u) {
        len = uboot_fat_strlen((const char *)want);
    }
    if (st.size != (br_u64)len) {
        return BR_FALSE;
    }
    f = br_open(path, BR_O_RDONLY);
    if (f == BR_NULL) {
        return BR_FALSE;
    }
    while (got < len) {
        const br_s64 r = br_file_read(f, buf, sizeof(buf));

        if (r <= 0) {
            (void)br_file_close(f);
            return BR_FALSE;
        }
        if (((br_size_t)r > (len - got)) ||
            (fat_bytes_eq(buf, want + got, (br_size_t)r) == BR_FALSE)) {
            (void)br_file_close(f);
            return BR_FALSE;
        }
        got += (br_size_t)r;
    }
    {
        const br_s64 r = br_file_read(f, buf, sizeof(buf));

        (void)br_file_close(f);
        if (r != 0) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

static int fat_join(char *out, br_size_t cap, const char *dir, const char *name)
{
    return uboot_fat_join_path(dir, name, out, cap);
}

/* Find one name in a directory; on a hit fill *out and return 1, otherwise return 0. */
static int fat_find_dirent(const char *dir, const char *name, br_dirent_t *out)
{
    br_dir_t *d = br_opendir(dir);
    br_dirent_t de;
    int rc;

    if (d == BR_NULL) {
        return 0;
    }
    while ((rc = br_readdir(d, &de)) == 1) {
        if (uboot_fat_strcasecmp(de.name, name) == 0) {
            if (out != BR_NULL) {
                *out = de;
            }
            (void)br_closedir(d);
            return 1;
        }
    }
    (void)br_closedir(d);
    return 0;
}

static br_bool fat_mount_listed(const char *path)
{
    const br_u32 n = br_mount_count();

    for (br_u32 i = 0u; i < n; i++) {
        const char *p = br_mount_path_at(i);

        if ((p != BR_NULL) && (uboot_fat_strcmp(p, path) == 0)) {
            return BR_TRUE;
        }
    }
    return BR_FALSE;
}

int uboot_fat_selftest(void)
{
    static const char *const mnt[UB_FAT_VOL_MAX] = {
        UB_FAT_MNT12, UB_FAT_MNT16, UB_FAT_MNT32
    };
    static const int want_type[UB_FAT_VOL_MAX] = {
        UB_FAT_TYPE12, UB_FAT_TYPE16, UB_FAT_TYPE32
    };

    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[FATCONF] uboot-fat conformance (3 volumes / FAT12-16-32 / VFS round-trip)");

    /* TC-FAT-001: three mount points present, each volume's BPB type and cluster count match. */
    {
        uboot_fat_vol_info_t vi;
        br_bool ok = BR_TRUE;
        int vol_ok = 0;

        if (br_mount_count() < (br_u32)UB_FAT_VOL_MAX) {
            fat_diag("TC-FAT-001", "mount_count", (long)br_mount_count(),
                     (long)UB_FAT_VOL_MAX);
            ok = BR_FALSE;
        }
        for (br_u32 i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
            if (fat_mount_listed(mnt[i]) == BR_FALSE) {
                fat_diag_str("TC-FAT-001", "mount_point_missing", mnt[i], "(in mount table)");
                ok = BR_FALSE;
            }
            if (uboot_fat_mount_vol_info(mnt[i], &vi) != 0) {
                fat_diag_str("TC-FAT-001", "vol_info_failed", mnt[i], "(ok)");
                ok = BR_FALSE;
                continue;
            }
            br_log_info("[FATCONF]   %s dev=%s mnt=%s FAT%d cluster=%u sectors clusters=%u",
                        mnt[i], (vi.dev != BR_NULL) ? vi.dev : "?",
                        (vi.mnt != BR_NULL) ? vi.mnt : "?",
                        vi.fatsize, vi.clust_size_sectors, vi.max_clust);
            /* Criteria: device name, BPB-derived type, cluster count in that type's range. */
            if ((vi.mounted == BR_TRUE) && (vi.sector_size == UB_FAT_SECTOR_SIZE) &&
                (vi.clust_size_sectors > 0u) && (vi.fatsize == want_type[i])) {
                const br_bool in_range =
                    ((vi.fatsize == 12) && (vi.max_clust >= 1u) && (vi.max_clust <= 0xff4u)) ||
                    ((vi.fatsize == 16) && (vi.max_clust > 0xff4u) && (vi.max_clust <= 0xfff4u)) ||
                    ((vi.fatsize == 32) && (vi.max_clust > 0xfff4u));

                if ((in_range == BR_TRUE) &&
                    (uboot_fat_strcmp(vi.dev, (i == 0u) ? UB_FAT_DEV12 :
                                             (i == 1u) ? UB_FAT_DEV16 : UB_FAT_DEV32) == 0)) {
                    vol_ok++;
                } else {
                    fat_diag_str("TC-FAT-001", "vol_shape", vi.dev,
                                 (i == 0u) ? UB_FAT_DEV12 :
                                 (i == 1u) ? UB_FAT_DEV16 : UB_FAT_DEV32);
                    fat_diag("TC-FAT-001", "fatsize/max_clust",
                             (long)(vi.fatsize * 1000000 + (int)vi.max_clust),
                             (long)(want_type[i] * 1000000));
                }
            } else {
                fat_diag("TC-FAT-001", "sector/cluster_size",
                         (long)(vi.sector_size + vi.clust_size_sectors * 100000),
                         (long)(UB_FAT_SECTOR_SIZE));
            }
        }
        fat_conf((ok == BR_TRUE) && (vol_ok == (int)UB_FAT_VOL_MAX), "TC-FAT-001",
                 "three mount points present; each volume's BPB type (12/16/32) and cluster range consistent");
    }

    /* ---- TC-FAT-002: read back each volume's host-preloaded file and compare byte by byte ---- */
    {
        static const char *const want[UB_FAT_VOL_MAX] = {
            UB_FAT_C12_HELLO, UB_FAT_C16_HELLO, UB_FAT_C32_HELLO
        };
        br_bool ok = BR_TRUE;

        for (br_u32 i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
            char path[UB_FAT_PATH_MAX];

            if (fat_join(path, sizeof(path), mnt[i], "hello.txt") != 0) {
                ok = BR_FALSE;
                continue;
            }
            if (fat_read_verify(path, (const br_u8 *)want[i], 0u) == BR_FALSE) {
                fat_diag_str("TC-FAT-002", "content_mismatch", path, want[i]);
                ok = BR_FALSE;
            }
        }
        fat_conf(ok, "TC-FAT-002", "host-preloaded hello.txt of all three volumes reads back byte-identical");
    }

    /* TC-FAT-003: readdir each root: "./.." first, preloaded entries with correct type/size. */
    {
        static const struct {
            const char *mnt_path;
            br_u32      dir_size;
        } cases[UB_FAT_VOL_MAX] = {
            { UB_FAT_MNT12, UB_FAT_C12_DIR_A_BYTES },
            { UB_FAT_MNT16, UB_FAT_C16_DIR_A_BYTES },
            { UB_FAT_MNT32, UB_FAT_C32_DIR_A_BYTES },
        };
        br_bool ok = BR_TRUE;

        for (br_u32 i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
            br_dirent_t de;
            br_stat_t   st;

            /* "." and ".." must be the first two entries (same as fs/tmpfs) */
            {
                br_dir_t *d = br_opendir(cases[i].mnt_path);

                if (d == BR_NULL) {
                    fat_diag_str("TC-FAT-003", "opendir_failed", cases[i].mnt_path, "(ok)");
                    ok = BR_FALSE;
                    continue;
                }
                if ((br_readdir(d, &de) != 1) || (uboot_fat_strcmp(de.name, ".") != 0) ||
                    (de.type != BR_INODE_DIR)) {
                    fat_diag_str("TC-FAT-003", "dirent[0]", de.name, ".");
                    fat_diag("TC-FAT-003", "dirent[0].type", (long)de.type,
                             (long)BR_INODE_DIR);
                    ok = BR_FALSE;
                }
                if ((br_readdir(d, &de) != 1) || (uboot_fat_strcmp(de.name, "..") != 0) ||
                    (de.type != BR_INODE_DIR)) {
                    fat_diag_str("TC-FAT-003", "dirent[1]", de.name, "..");
                    fat_diag("TC-FAT-003", "dirent[1].type", (long)de.type,
                             (long)BR_INODE_DIR);
                    ok = BR_FALSE;
                }
                (void)br_closedir(d);
            }

            /* Name comparison is case-insensitive: FAT stores 8.3 uppercase plus two "lowercase
             * bits" that mkfs/mcopy may not set, so readdir reporting "HELLO.TXT" is legal. */
            if (fat_find_dirent(cases[i].mnt_path, "hello.txt", &de) == 0) {
                ok = BR_FALSE;
            } else if ((de.type != BR_INODE_FILE) ||
                       (de.name_len != (br_u32)uboot_fat_strlen(de.name)) ||
                       (uboot_fat_strcasecmp(de.name, "hello.txt") != 0)) {

                ok = BR_FALSE;
            }
            if (fat_find_dirent(cases[i].mnt_path, "dir", &de) == 0) {

                ok = BR_FALSE;
            } else if ((de.type != BR_INODE_DIR) ||
                       (uboot_fat_strcasecmp(de.name, "dir") != 0)) {

                ok = BR_FALSE;
            }
            {
                char abin[UB_FAT_PATH_MAX];

                if (fat_join(abin, sizeof(abin), cases[i].mnt_path, "dir/a.bin") != 0) {
                    fat_diag_str("TC-FAT-003", "join_failed", cases[i].mnt_path, "dir/a.bin");
                    ok = BR_FALSE;
                } else if (br_stat(abin, &st) != 0) {
                    fat_diag_str("TC-FAT-003", "stat_dir_a_bin_failed", abin, "(ok)");
                    ok = BR_FALSE;
                } else if ((st.type != BR_INODE_FILE) ||
                           (st.size != (br_u64)cases[i].dir_size)) {
                    fat_diag("TC-FAT-003", "dir/a.bin.size", (long)st.size,
                             (long)cases[i].dir_size);
                    ok = BR_FALSE;
                }
            }
        }
        fat_conf(ok, "TC-FAT-003",
                 "readdir's first two entries are ./..; hello.txt and dir/ have host-matching type and length");
    }

    /* ---- TC-FAT-004: /fat16 create → three-segment write → close → reopen and compare ---- */
    {
        static const char *const path = UB_FAT_MNT16 "/tc4_new.txt";
        const br_size_t total = 3u * 512u;          /* spans 3 clusters (cluster = 1 sector on a 16 MiB volume) */
        br_u8 content[3u * 512u];
        br_u8 back[3u * 512u];
        br_file_t *f;
        br_bool ok = BR_TRUE;

        (void)br_unlink(path);                      /* a previous run may have left it (the image persists) */
        for (br_size_t i = 0u; i < total; i++) {
            content[i] = (br_u8)(0x40u + (br_u8)(i % 0x3fu));
        }
        f = br_open(path, (br_u32)(BR_O_WRONLY | BR_O_CREAT | BR_O_TRUNC));
        if (f == BR_NULL) {
            fat_diag_str("TC-FAT-004", "open_for_write_failed", path, "(ok)");
            ok = BR_FALSE;
        } else {
            for (br_u32 seg = 0u; seg < 3u; seg++) {
                const br_s64 w = br_file_write(f, content + seg * 512u, 512u);

                if (w != 512) {
                    fat_diag("TC-FAT-004", "segment_write", (long)w, 512);
                    ok = BR_FALSE;
                }
            }
            (void)br_file_close(f);
        }
        f = br_open(path, BR_O_RDONLY);
        if (f == BR_NULL) {
            fat_diag_str("TC-FAT-004", "reopen_failed", path, "(ok)");
            ok = BR_FALSE;
        } else {
            const br_s64 r = br_file_read(f, back, sizeof(back));

            (void)br_file_close(f);
            if (r != (br_s64)total) {
                fat_diag("TC-FAT-004", "readback_len", (long)r, (long)total);
                ok = BR_FALSE;
            } else if (fat_bytes_eq(back, content, total) == BR_FALSE) {
                br_size_t bad = 0u;

                while ((bad < total) && (back[bad] == content[bad])) {
                    bad++;
                }
                fat_diag("TC-FAT-004", "first_mismatch_byte", (long)bad, (long)total);
                fat_diag("TC-FAT-004", "byte_got_vs_want",
                         (long)((bad < total) ? back[bad] : 0u),
                         (long)((bad < total) ? content[bad] : 0u));
                ok = BR_FALSE;
            }
        }
        fat_conf(ok, "TC-FAT-004", "new file written in three segments (1536 B) reads back byte-identical after reopen");
    }

    /* ---- TC-FAT-005: lseek(SEEK_END/SET) and EOF read returns 0 ---- */
    {
        static const char *const path = UB_FAT_MNT12 "/hello.txt";
        const br_size_t len = uboot_fat_strlen(UB_FAT_C12_HELLO);
        br_file_t *f;
        br_u8 one = 0u;
        br_bool ok = BR_TRUE;

        f = br_open(path, BR_O_RDONLY);
        if (f == BR_NULL) {
            fat_diag_str("TC-FAT-005", "open_failed", path, "(ok)");
            ok = BR_FALSE;
        } else {
            br_s64 p = br_file_lseek(f, 0, BR_SEEK_END);

            if (p != (br_s64)len) {
                fat_diag("TC-FAT-005", "SEEK_END", (long)p, (long)len);
                ok = BR_FALSE;
            }
            p = br_file_read(f, &one, 1u);
            if (p != 0) {                  /* EOF read = 0 (success) */
                fat_diag("TC-FAT-005", "eof_read", (long)p, 0);
                ok = BR_FALSE;
            }
            p = br_file_lseek(f, 3, BR_SEEK_SET);
            if (p != 3) {
                fat_diag("TC-FAT-005", "SEEK_SET(3)", (long)p, 3);
                ok = BR_FALSE;
            }
            p = br_file_read(f, &one, 1u);
            if (p != 1) {
                fat_diag("TC-FAT-005", "read_after_seek", (long)p, 1);
                ok = BR_FALSE;
            } else if (one != (br_u8)UB_FAT_C12_HELLO[3]) {
                fat_diag("TC-FAT-005", "byte_at_3", (long)one,
                         (long)(br_u8)UB_FAT_C12_HELLO[3]);
                ok = BR_FALSE;
            }
            p = br_file_lseek(f, -1, BR_SEEK_SET);
            if (p >= 0) {                  /* negative offset ⇒ -EINVAL */
                fat_diag("TC-FAT-005", "negative_seek", (long)p, (long)BR_ERR(EINVAL));
                ok = BR_FALSE;
            }
            (void)br_file_close(f);
        }
        fat_conf(ok, "TC-FAT-005", "SEEK_END/SET position correctly; EOF read returns 0; negative offset rejected");
    }

    /* ---- TC-FAT-006: /fat32 mkdir + create a file in it + stat validates size/type ---- */
    {
        static const char *const dir   = UB_FAT_MNT32 "/tc6_dir";
        static const char *const path  = UB_FAT_MNT32 "/tc6_dir/f.bin";
        const br_size_t len = 100u;
        br_u8 content[100];
        br_stat_t st;
        br_bool ok = BR_TRUE;

        (void)br_unlink(path);
        (void)br_rmdir(dir);
        fat_make_pattern(content, len, 'M');

        if (br_mkdir(dir) != 0) {

            ok = BR_FALSE;
        }
        if (br_stat(dir, &st) != 0) {

            ok = BR_FALSE;
        } else if (st.type != BR_INODE_DIR) {

            ok = BR_FALSE;
        }
        if (fat_write_all(path, content, len) != 0) {

            ok = BR_FALSE;
        }
        if (br_stat(path, &st) != 0) {

            ok = BR_FALSE;
        } else if ((st.type != BR_INODE_FILE) || (st.size != (br_u64)len)) {

            ok = BR_FALSE;
        } else if (fat_read_verify(path, content, len) == BR_FALSE) {

            ok = BR_FALSE;
        }
        fat_conf(ok, "TC-FAT-006", "/fat32 mkdir + create file + stat size/type match the content");
    }

    /* ---- TC-FAT-007: after rename the old name is -ENOENT and the new name has correct content ---- */
    {
        static const char *const oldp = UB_FAT_MNT16 "/tc7_a.txt";
        static const char *const newp = UB_FAT_MNT16 "/tc7_b.txt";
        br_u8 content[300];
        br_stat_t st;
        br_bool ok = BR_TRUE;

        (void)br_unlink(oldp);
        (void)br_unlink(newp);
        fat_make_pattern(content, sizeof(content), 'R');

        if (fat_write_all(oldp, content, sizeof(content)) != 0) {

            ok = BR_FALSE;
        }
        if (br_rename(oldp, newp) != 0) {

            ok = BR_FALSE;
        }
        if (br_stat(oldp, &st) != BR_ERR(ENOENT)) {

            ok = BR_FALSE;
        }
        if (fat_read_verify(newp, content, sizeof(content)) == BR_FALSE) {

            ok = BR_FALSE;
        }
        fat_conf(ok, "TC-FAT-007", "after rename the old name is -ENOENT and the new name is byte-identical");
    }

    /* ---- TC-FAT-008: unlink ⇒ -ENOENT; truncate shrink then size/content match; sync == 0 ---- */
    {
        static const char *const path = UB_FAT_MNT16 "/tc8_trunc.txt";
        br_u8 content[1000];
        br_u8 want[300];
        br_u8 back[1000];
        br_stat_t st;
        br_file_t *f;
        br_bool ok = BR_TRUE;

        (void)br_unlink(path);
        fat_make_pattern(content, sizeof(content), 'T');
        fat_make_pattern(want, sizeof(want), 'T');

        if (fat_write_all(path, content, sizeof(content)) != 0) {
            fat_diag_str("TC-FAT-008", "write_all_failed", path, "(ok)");
            ok = BR_FALSE;
        }
        if (br_stat(path, &st) != 0) {
            fat_diag_str("TC-FAT-008", "stat_1000_failed", path, "(ok)");
            ok = BR_FALSE;
        } else if (st.size != (br_u64)sizeof(content)) {
            fat_diag("TC-FAT-008", "size_before_trunc", (long)st.size, 1000);
            ok = BR_FALSE;
        }
        {
            const int rc = br_truncate(path, (br_u64)sizeof(want));

            if (rc != 0) {
                fat_diag("TC-FAT-008", "truncate_shrink_rc", (long)rc, 0);
                ok = BR_FALSE;
            }
        }
        if (br_stat(path, &st) != 0) {
            fat_diag_str("TC-FAT-008", "stat_after_shrink_failed", path, "(ok)");
            ok = BR_FALSE;
        } else if (st.size != (br_u64)sizeof(want)) {
            fat_diag("TC-FAT-008", "size_after_shrink", (long)st.size, 300);
            ok = BR_FALSE;
        } else {
            f = br_open(path, BR_O_RDONLY);
            if (f == BR_NULL) {
                ok = BR_FALSE;
            } else {
                const br_s64 r = br_file_read(f, back, sizeof(back));

                (void)br_file_close(f);
                /* The read-back must be **exactly** the new length: one extra byte means the cluster chain was not released */
                if (r != (br_s64)sizeof(want)) {
                    fat_diag("TC-FAT-008", "shrink_read_len", (long)r, 300);
                    ok = BR_FALSE;
                } else if (fat_bytes_eq(back, want, sizeof(want)) == BR_FALSE) {
                    fat_diag_str("TC-FAT-008", "shrink_content", path,
                                 "(first 300 B unchanged)");
                    ok = BR_FALSE;
                }
            }
        }
        /* Grow 300 -> 600: the new region must read back as 0, not previous leftovers. */
        if (br_truncate(path, 600u) == 0) {
            f = br_open(path, BR_O_RDONLY);
            if (f != BR_NULL) {
                const br_s64 r = br_file_read(f, back, sizeof(back));

                (void)br_file_close(f);
                if (r != 600) {
                    fat_diag("TC-FAT-008", "grow_read_len", (long)r, 600);
                    ok = BR_FALSE;
                }
                for (br_s64 i = 300; i < 600; i++) {
                    if (back[i] != 0u) {
                        fat_diag("TC-FAT-008", "grow_byte_not_zero", (long)i, 300);
                        ok = BR_FALSE;
                        break;
                    }
                }
            } else {
                ok = BR_FALSE;
            }
        } else {
            ok = BR_FALSE;
        }

        {
            const int rc = br_unlink(path);

            if (rc != 0) {
                fat_diag("TC-FAT-008", "unlink_rc", (long)rc, 0);
                ok = BR_FALSE;
            }
        }
        if (br_stat(path, &st) != BR_ERR(ENOENT)) {
            fat_diag_str("TC-FAT-008", "stat_after_unlink", path, "(ENOENT)");
            ok = BR_FALSE;
        }
        /* Volume sync must use the same path as file fsync (both share uboot_fat_flush_volume). */
        {
            const int rc = uboot_fat_test_sync(UB_FAT_MNT16);

            if (rc != 0) {
                fat_diag("TC-FAT-008", "fs_ops.sync_rc", (long)rc, 0);
                ok = BR_FALSE;
            }
        }
        {
            br_file_t *ff = br_open(UB_FAT_MNT16 "/hello.txt", BR_O_RDONLY);

            if (ff == BR_NULL) {
                fat_diag_str("TC-FAT-008", "open_for_fsync_failed",
                             UB_FAT_MNT16 "/hello.txt", "(ok)");
                ok = BR_FALSE;
            } else {
                const int rc = br_file_fsync(ff);

                if (rc != 0) {
                    fat_diag("TC-FAT-008", "file_fsync_rc", (long)rc, 0);
                    ok = BR_FALSE;
                }
                (void)br_file_close(ff);
            }
        }
        if (br_file_fsync(BR_NULL) != BR_ERR(EINVAL)) {
            fat_diag("TC-FAT-008", "fsync(NULL)",
                     (long)br_file_fsync(BR_NULL), (long)BR_ERR(EINVAL));
            ok = BR_FALSE;             /* a null handle is a bad argument (contract: f == NULL ⇒ -EINVAL) */
        }
        fat_conf(ok, "TC-FAT-008",
                 "truncate 300↔600 keeps size/content consistent; unlink ⇒ -ENOENT; volume sync and file fsync both == 0");
    }

    br_log_info("[FATCONF] SUMMARY pass=%u fail=%u total=%u",
                s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
