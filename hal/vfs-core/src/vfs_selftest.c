
/*
 * vfs-core self-test suite: runs the TC-VFS-* cases through the public API of br/vfs/br_vfs.h
 * only (no FS name, no FS/device internals), so it scales with the composition. TC-VFS-002
 * inspects the match algorithm itself via src/vfs_internal.h accessors, which forward to the
 * production implementation. Returns the failure count; logs [VFSCONF] PASS/FAIL and SUMMARY.
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "vfs_internal.h"

/* Implements only vfs_core_selftest; lifecycle hooks live in the production files. */
int vfs_core_selftest(void);

static br_u32 s_pass;
static br_u32 s_fail;

static void pc(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[VFSCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[VFSCONF] FAIL %s %s", tag, what);
    }
}

static void conf_path(char *dst, br_size_t cap, const char *base, const char *name)
{
    br_size_t n = 0u;
    for (br_size_t i = 0u; base[i] != '\0' && n + 1u < cap; i++) {
        dst[n++] = base[i];
    }
    if (n > 0u && dst[n - 1u] != '/' && n + 1u < cap) {
        dst[n++] = '/';
    }
    for (br_size_t i = 0u; name[i] != '\0' && n + 1u < cap; i++) {
        dst[n++] = name[i];
    }
    dst[n] = '\0';
}

static int conf_count_dir(const char *path, br_u32 *total, br_u32 *files)
{
    br_dir_t *d = br_opendir(path);
    if (d == BR_NULL) {
        return BR_ERR(ENOENT);
    }
    br_u32 n = 0u;
    br_u32 nf = 0u;
    for (;;) {
        br_dirent_t de;
        const int rc = br_readdir(d, &de);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            (void)br_closedir(d);
            return rc;
        }
        n++;
        if (de.type == BR_INODE_FILE) {
            nf++;
        }
    }
    (void)br_closedir(d);
    if (total != BR_NULL) {
        *total = n;
    }
    if (files != BR_NULL) {
        *files = nf;
    }
    return BR_OK;
}

int vfs_core_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[VFSCONF] vfs-core conformance (mount table / walk chain / file face / dir face)");

/* ---- TC-VFS-001: mount table non-empty with a root mount ---- */
    br_u32 root_idx = (br_u32)-1;
    {
        br_bool have_root = BR_FALSE;
        br_bool paths_ok  = BR_TRUE;
        for (br_u32 i = 0u; i < br_mount_count(); i++) {
            const char *p = br_mount_path_at(i);
            if (p == BR_NULL || p[0] != '/') {
                paths_ok = BR_FALSE;
                break;
            }
            if (br_vfs_internal_str_eq(p, "/") == BR_TRUE) {
                have_root = BR_TRUE;
            }
        }
        pc((br_mount_count() > 0u) && have_root && paths_ok &&
           (br_mount_path_at(br_mount_count()) == BR_NULL),
           "TC-VFS-001", "mount table non-empty / has root mount '/' / all paths absolute / out-of-range path lookup => NULL");

/* Locate the root mount; if absent, stop here (else cascading FAILs). */
        for (br_u32 i = 0u; i < br_mount_count(); i++) {
            const char *p = br_mount_path_at(i);
            if (p != BR_NULL && br_vfs_internal_str_eq(p, "/") == BR_TRUE) {
                root_idx = i;
                break;
            }
        }
    }
    if (root_idx == (br_u32)-1) {
        br_log_info("[VFSCONF] note: no root mount => all rootfs-dependent cases not applicable (not falsely green: FAIL recorded)");
        br_log_info("[VFSCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
        return (int)s_fail;
    }
    const char *base = "/";     /* the '/' mount was asserted above */

/* ---- TC-VFS-002: longest-prefix match + duplicate mount => -EEXIST ---- */
    {
        br_bool ok = BR_TRUE;
/* Each mount point's path must hit itself via longest-prefix match (internal accessors). */
        br_u32 n = 0u;
        const vfs_mount_internal_t *ms = br_vfs_internal_mounts(&n);
        for (br_u32 i = 0u; i < n; i++) {
            if (br_vfs_internal_mount_match(ms[i].path) != i) {
                ok = BR_FALSE;
            }
        }
        const int r = br_mount_register("/", br_vfs_internal_mounts(BR_NULL)[root_idx].ops,
                                        br_vfs_internal_mounts(BR_NULL)[root_idx].priv);
        if (r != BR_ERR(EEXIST)) {
            ok = BR_FALSE;
        }
        if (br_mount_register("/x", BR_NULL, BR_NULL) != BR_ERR(EINVAL) ||
            br_mount_register(BR_NULL, br_vfs_internal_mounts(BR_NULL)[root_idx].ops, BR_NULL) != BR_ERR(EINVAL) ||
            br_mount_register("relative", br_vfs_internal_mounts(BR_NULL)[root_idx].ops, BR_NULL) != BR_ERR(EINVAL)) {
            ok = BR_FALSE;
        }
        pc(ok, "TC-VFS-002", "longest-prefix match (own path hits itself); duplicate mount -EEXIST; bad args -EINVAL");
    }

/* ---- Private temp directory (does not rely on tmpfs-prebuilt /dev /data /tmp) ---- */
    char dir[BR_VFS_PATH_MAX_INTERNAL];
    char file[BR_VFS_PATH_MAX_INTERNAL];
    char file2[BR_VFS_PATH_MAX_INTERNAL];
    conf_path(dir,   (br_size_t)sizeof(dir),   base, "vfsconf.d");
    conf_path(file,  (br_size_t)sizeof(file),  dir,  "a.bin");
    conf_path(file2, (br_size_t)sizeof(file2), dir,  "b.bin");

    (void)br_unlink(file);          /* clear leftovers from the previous run (failure is fine: may not exist) */
    (void)br_unlink(file2);
    (void)br_unlink(dir);
    (void)br_rmdir(dir);

/* ---- TC-VFS-003: mkdir / idempotent conflict / file ops on a directory ---- */
    {
        br_bool ok = (br_mkdir(dir) == BR_OK) && (br_mkdir(dir) == BR_ERR(EEXIST));
        br_stat_t st;
        ok = ok && (br_stat(dir, &st) == BR_OK) && (st.type == BR_INODE_DIR);
        int err = BR_OK;
        br_file_t *f = br_open_err(dir, BR_O_WRONLY, &err);
        ok = ok && (f == BR_NULL) && (err == BR_ERR(EISDIR));
        int err2 = BR_OK;
        br_file_t *fa = br_open_err(file, BR_O_CREAT | BR_O_RDWR, &err2);
        ok = ok && (fa != BR_NULL);
        if (fa != BR_NULL) {
            (void)br_file_close(fa);
            char deep[BR_VFS_PATH_MAX_INTERNAL];
            conf_path(deep, (br_size_t)sizeof(deep), file, "x");
            int err3 = BR_OK;
            br_file_t *fd = br_open_err(deep, BR_O_RDONLY, &err3);
            ok = ok && (fd == BR_NULL) && (err3 == BR_ERR(ENOTDIR));
        }
        pc(ok, "TC-VFS-003", "mkdir/repeat -EEXIST; stat reports DIR; write-open dir -EISDIR; file as dir -ENOTDIR");
    }

/* ---- TC-VFS-004: file roundtrip (create -> write x3 -> lseek -> read -> stat) ---- */
    {
        static const char p1[] = "brickOS ";
        static const char p2[] = "vfs ";
        static const char p3[] = "roundtrip";
        const br_size_t  want = (br_size_t)(sizeof(p1) - 1u + sizeof(p2) - 1u + sizeof(p3) - 1u);

        int err = BR_OK;
        br_file_t *f = br_open_err(file, BR_O_CREAT | BR_O_RDWR | BR_O_TRUNC, &err);
        br_bool ok = (f != BR_NULL);
        if (ok == BR_TRUE) {
            ok = (br_file_write(f, p1, sizeof(p1) - 1u) == (br_s64)(sizeof(p1) - 1u)) &&
                 (br_file_write(f, p2, sizeof(p2) - 1u) == (br_s64)(sizeof(p2) - 1u)) &&
                 (br_file_write(f, p3, sizeof(p3) - 1u) == (br_s64)(sizeof(p3) - 1u));
            ok = ok && (br_file_lseek(f, 0, BR_SEEK_END) == (br_s64)want);
            ok = ok && (br_file_lseek(f, 0, BR_SEEK_SET) == 0);
            ok = ok && (br_file_offset(f) == 0);

            char   back[32];
            const br_s64 n = br_file_read(f, back, (br_size_t)sizeof(back));
            ok = ok && (n == (br_s64)want);
            if (n == (br_s64)want) {
                static const char whole[] = "brickOS vfs roundtrip";
                ok = ok && br_vfs_internal_mem_eq(back, whole, want);
            }
            ok = ok && (br_file_read(f, back, 1u) == 0);
            pc(ok, "TC-VFS-004", "file roundtrip: three writes / SEEK_END / SEEK_SET / read-back byte-identical / EOF returns 0");

            ok = (br_file_close(f) == BR_OK);
            br_stat_t st;
            ok = ok && (br_stat(file, &st) == BR_OK) &&
                 (st.type == BR_INODE_FILE) && (st.size == (br_u64)want);
            pc(ok, "TC-VFS-005", "close returns 0; stat type/length match the writes");
        } else {
            pc(BR_FALSE, "TC-VFS-004", "file roundtrip: open failed (see err on previous line)");
            pc(BR_FALSE, "TC-VFS-005", "close/stat: precondition failed");
        }
    }

/* ---- TC-VFS-006: negative open semantics (O_EXCL / missing / null args / relative path) ---- */
    {
        int e1 = BR_OK, e2 = BR_OK, e3 = BR_OK, e4 = BR_OK;
        br_file_t *f1 = br_open_err(file, BR_O_CREAT | BR_O_EXCL | BR_O_RDWR, &e1);
        br_file_t *f2 = br_open_err("/vfsconf_no_such_file", BR_O_RDONLY, &e2);
        br_file_t *f3 = br_open_err(BR_NULL, BR_O_RDONLY, &e3);
        br_file_t *f4 = br_open_err("relative/path", BR_O_RDONLY, &e4);
        const br_bool ok = (f1 == BR_NULL) && (e1 == BR_ERR(EEXIST)) &&
                           (f2 == BR_NULL) && (e2 == BR_ERR(ENOENT)) &&
                           (f3 == BR_NULL) && (e3 == BR_ERR(EINVAL)) &&
                           (f4 == BR_NULL) && (e4 == BR_ERR(EINVAL));
        pc(ok, "TC-VFS-006", "O_CREAT|O_EXCL existing -EEXIST; missing -ENOENT; NULL/relative path -EINVAL");
    }

/* ---- TC-VFS-007: directory iteration (fixed-length entries) ---- */
    {
        br_u32 total = 0u, files = 0u;
        const int rc = conf_count_dir(dir, &total, &files);
        br_bool ok = (rc == BR_OK) && (total == 3u) && (files == 1u);
        pc(ok, "TC-VFS-007", "readdir: '.' + '..' + 1 file = 3 entries, 1 of them FILE type");
    }

/* ---- TC-VFS-008: rename + truncate ---- */
    {
        br_bool ok = (br_rename(file, file2) == BR_OK);
        br_stat_t st;
        ok = ok && (br_stat(file2, &st) == BR_OK) && (st.size > 0u);
        ok = ok && (br_stat(file, &st) == BR_ERR(ENOENT));

        ok = ok && (br_truncate(file2, 7u) == BR_OK) &&
             (br_stat(file2, &st) == BR_OK) && (st.size == 7u);
        int err = BR_OK;
        br_file_t *f = br_open_err(file2, BR_O_RDONLY, &err);
        if (ok == BR_TRUE && f != BR_NULL) {
            char back[8];
            const br_s64 n = br_file_read(f, back, (br_size_t)sizeof(back));
            ok = (n == 7) && br_vfs_internal_mem_eq(back, "brickOS", 7u);
        } else {
            ok = BR_FALSE;
        }
        if (f != BR_NULL) {
            (void)br_file_close(f);
        }
        int et = BR_OK;
        br_file_t *ft = br_open_err(file2, BR_O_WRONLY | BR_O_TRUNC, &et);
        ok = ok && (ft != BR_NULL);
        if (ft != BR_NULL) {
            (void)br_file_close(ft);
        }
        ok = ok && (br_stat(file2, &st) == BR_OK) && (st.size == 0u);
        pc(ok, "TC-VFS-008", "rename: old name gone / new name present; truncate(7) keeps content; O_TRUNC zeroes");
    }

/* ---- TC-VFS-009: lseek boundaries + handle accessors ---- */
    {
        int err = BR_OK;
        br_file_t *f = br_open_err(file2, BR_O_RDWR, &err);
        br_bool ok = (f != BR_NULL);
        if (f != BR_NULL) {
            ok = ok && (br_file_lseek(f, -1, BR_SEEK_SET) == BR_ERR(EINVAL));
            ok = ok && (br_file_lseek(f, 0, BR_SEEK_END) == 0);
            ok = ok && (br_file_lseek(f, 1, 99 /* invalid whence */) == BR_ERR(EINVAL));
            ok = ok && (br_file_inode(f) != BR_NULL) &&
                 (br_file_inode(f)->type == BR_INODE_FILE) &&
                 ((br_file_flags(f) & BR_O_ACCMODE) == BR_O_RDWR);
            ok = ok && (br_file_close(f) == BR_OK);
        }
        pc(ok, "TC-VFS-009", "lseek: negative offset -EINVAL / bad whence -EINVAL; accessors match open flags");
    }

/* ---- TC-VFS-010: unlink / rmdir (including negatives) ---- */
    {
        br_bool ok = (br_unlink(file2) == BR_OK) &&
                     (br_unlink(file2) == BR_ERR(ENOENT)) &&
                     (br_stat(file2, &(br_stat_t){0}) == BR_ERR(ENOENT)) &&
                     (br_rmdir(dir) == BR_OK) &&
                     (br_rmdir(dir) == BR_ERR(ENOENT));
        pc(ok, "TC-VFS-010", "unlink/rmdir succeed and are idempotent (repeat => -ENOENT)");
    }

/* ---- TC-VFS-011: null handle/args robustness (no crash, contract error codes) ---- */
    {
        char byte = 0;
        const br_bool ok =
            (br_file_read(BR_NULL, &byte, 1u) == BR_ERR(EINVAL)) &&
            (br_file_write(BR_NULL, &byte, 1u) == BR_ERR(EINVAL)) &&
            (br_file_lseek(BR_NULL, 0, BR_SEEK_SET) == BR_ERR(EINVAL)) &&
            (br_file_ioctl(BR_NULL, 0u, BR_NULL) == BR_ERR(EINVAL)) &&
            (br_file_fsync(BR_NULL) == BR_ERR(EINVAL)) &&
            (br_file_close(BR_NULL) == BR_OK) &&
            (br_readdir(BR_NULL, BR_NULL) == BR_ERR(EINVAL)) &&
            (br_closedir(BR_NULL) == BR_OK) &&
            (br_opendir("/vfsconf_no_such_dir") == BR_NULL) &&
            (br_stat(BR_NULL, &(br_stat_t){0}) == BR_ERR(EINVAL)) &&
            (br_mkdir(BR_NULL) == BR_ERR(EINVAL)) &&
            (br_truncate(BR_NULL, 0u) == BR_ERR(EINVAL));
        pc(ok, "TC-VFS-011", "null handle/null path: return -EINVAL; close(NULL)/closedir(NULL) idempotent 0");
    }

/* ---- TC-VFS-012: mount points cannot be rmdir/unlink (no unmount before v3) ---- */
    {
        br_bool ok = BR_TRUE;
        for (br_u32 i = 0u; i < br_mount_count(); i++) {
            const char *p = br_mount_path_at(i);
            if (p == BR_NULL || br_vfs_internal_str_eq(p, "/") == BR_TRUE) {
                continue;               /* root mount has no "/.." parent; skip it separately */
            }
            if (br_rmdir(p) != BR_ERR(EBUSY) || br_unlink(p) != BR_ERR(EBUSY)) {
                ok = BR_FALSE;
            }
        }
        pc(ok, "TC-VFS-012", "mount point itself cannot be rmdir/unlink => -EBUSY (no unmount before v3)");
    }

/* ---- TC-VFS-013: non-root mount (/dev): readdir enumerable, stat ok, open only 0/-ENOTSUP ---- */
    {
        const char *dev = BR_NULL;
        for (br_u32 i = 0u; i < br_mount_count(); i++) {
            const char *p = br_mount_path_at(i);
            if (p != BR_NULL && br_vfs_internal_str_eq(p, "/") == BR_FALSE) {
                dev = p;
                break;
            }
        }
        if (dev == BR_NULL) {
            br_log_info("[VFSCONF] note: no non-root mount in this composition => TC-VFS-013 not applicable");
        } else {
            br_u32 total = 0u, files = 0u;
            br_bool ok = (conf_count_dir(dev, &total, &files) == BR_OK) && (total >= 2u);
            br_dir_t *d = br_opendir(dev);
            br_u32 checked = 0u;
            if (d != BR_NULL) {
                for (;;) {
                    br_dirent_t de;
                    const int rc = br_readdir(d, &de);
                    if (rc <= 0) {
                        break;
                    }
                    if (br_vfs_internal_str_eq(de.name, ".") == BR_TRUE || br_vfs_internal_str_eq(de.name, "..") == BR_TRUE) {
                        continue;
                    }
                    char p[BR_VFS_PATH_MAX_INTERNAL];
                    conf_path(p, (br_size_t)sizeof(p), dev, de.name);

                    br_stat_t st;
                    ok = ok && (br_stat(p, &st) == BR_OK);

                    int err = BR_OK;
                    br_file_t *f = br_open_err(p, BR_O_RDONLY | BR_O_NONBLOCK, &err);
                    if (f != BR_NULL) {
                        ok = ok && (br_file_close(f) == BR_OK);     /* has a file face: open/close paired */
                    } else {
/* No file face (v1 raw flash) must be -ENOTSUP; any other code means broken node construction. */
                        ok = ok && (err == BR_ERR(ENOTSUP));
                    }
                    checked++;
                }
                (void)br_closedir(d);
            } else {
                ok = BR_FALSE;
            }
            br_log_info("[VFSCONF] %s has %u concrete device nodes (open: success = has file face / -ENOTSUP = list-only)",
                        dev, checked);
            pc(ok, "TC-VFS-013", "non-root mount: readdir enumerable, every node stat succeeds and open returns only 0/-ENOTSUP");
        }
    }

/* ---- Link family: symlink expansion lives in vfs-core, hard-link data lives in the FS ---- */
    char ldir[BR_VFS_PATH_MAX_INTERNAL];
    char tgt[BR_VFS_PATH_MAX_INTERNAL];
    char lnk[BR_VFS_PATH_MAX_INTERNAL];
    char hard[BR_VFS_PATH_MAX_INTERNAL];
    conf_path(ldir, (br_size_t)sizeof(ldir), base, "vfsconf.lnk.d");
    conf_path(tgt,  (br_size_t)sizeof(tgt),  ldir, "target.bin");
    conf_path(lnk,  (br_size_t)sizeof(lnk),  ldir, "link1");
    conf_path(hard, (br_size_t)sizeof(hard), ldir, "hard1");

    (void)br_unlink(lnk);
    (void)br_unlink(hard);
    (void)br_unlink(tgt);
    (void)br_rmdir(ldir);
    (void)br_mkdir(ldir);

/* ---- TC-VFS-014: symlink creation + lstat sees the link + stat follows (incl. dangling) ---- */
    {
        br_stat_t lst;
        br_stat_t fst;
        const int e1 = br_symlink("target.bin", lnk);        /* target does not exist yet: dangling is valid */
        const int e2 = br_symlink("target.bin", lnk);        /* again => -EEXIST */
        const int e3 = br_symlink(BR_NULL, lnk);
        const int e4 = br_symlink("", lnk);

        const br_bool l_ok = (br_lstat(lnk, &lst) == BR_OK) &&
                             (lst.type == BR_INODE_SYMLINK) &&
                             (lst.size == 10u) &&                    /* "target.bin" = 10 chars */
                             ((lst.valid & BR_STAT_NLINK) != 0u) && (lst.nlink == 1u);
        const br_bool dangling = (br_stat(lnk, &fst) == BR_ERR(ENOENT));

        int err = BR_OK;
        br_file_t *tf = br_open_err(tgt, BR_O_CREAT | BR_O_RDWR | BR_O_TRUNC, &err);
        br_bool follow_ok = BR_FALSE;
        if (tf != BR_NULL) {
            (void)br_file_write(tf, "hello", 5u);
            (void)br_file_close(tf);
            follow_ok = (br_stat(lnk, &fst) == BR_OK) && (fst.type == BR_INODE_FILE) &&
                        (fst.size == 5u);
        }
        pc((e1 == BR_OK) && (e2 == BR_ERR(EEXIST)) && (e3 == BR_ERR(EINVAL)) &&
           (e4 == BR_ERR(EINVAL)) && l_ok && dangling && follow_ok,
           "TC-VFS-014", "symlink: may dangle / repeat -EEXIST / empty target -EINVAL; lstat sees link, stat follows");
    }

/* ---- TC-VFS-015: readlink boundaries ---- */
    {
        char        small[4];
        char        full[BR_SYMLINK_MAX + 2u];
        const int   n_small = br_readlink(lnk, small, (br_size_t)sizeof(small));
        const int   n_full  = br_readlink(lnk, full, (br_size_t)sizeof(full));
        const int   n_null  = br_readlink(lnk, BR_NULL, 0u);     /* length query only */
        const int   n_file  = br_readlink(tgt, full, (br_size_t)sizeof(full));
        const int   n_none  = br_readlink("/vfsconf_no_such_link", full, (br_size_t)sizeof(full));

        br_bool ok = (n_small == 10) && (small[3] == '\0') &&     /* truncated to cap-1 + NUL */
                     (n_full == 10) && (n_null == 10) && (n_file == BR_ERR(EINVAL)) &&
                     (n_none == BR_ERR(ENOENT)) && (full[0] == 't') && (full[9] == 'n');
        for (br_size_t i = 0u; i < 10u && ok == BR_TRUE; i++) {
            static const char want[] = "target.bin";
            if (full[i] != want[i]) {
                ok = BR_FALSE;
            }
        }
        pc(ok, "TC-VFS-015", "readlink: truncation still reports full length / null buf reports length / non-link -EINVAL / missing -ENOENT");
    }

/* ---- TC-VFS-016: read/write/truncate through a link land on the target ---- */
    {
        char      buf[16];
        br_bool   ok = BR_TRUE;
        br_stat_t st;

        int err = BR_OK;
        br_file_t *lf = br_open_err(lnk, BR_O_RDONLY, &err);
        if (lf != BR_NULL) {
            const br_s64 r = br_file_read(lf, buf, (br_size_t)sizeof(buf));
            ok = ok && (r == 5) && (buf[0] == 'h');
            (void)br_file_close(lf);
        } else {
            ok = BR_FALSE;
        }
        br_file_t *wf = br_open_err(lnk, BR_O_WRONLY | BR_O_TRUNC, &err);
        if (wf != BR_NULL) {
            (void)br_file_write(wf, "world!", 6u);
            (void)br_file_close(wf);
        } else {
            ok = BR_FALSE;
        }
        ok = ok && (br_stat(tgt, &st) == BR_OK) && (st.size == 6u);

        br_file_t *df = br_open_err(tgt, BR_O_RDONLY, &err);   /* read back via the target name */
        if (df != BR_NULL) {
            const br_s64 r = br_file_read(df, buf, (br_size_t)sizeof(buf));
            ok = ok && (r == 6) && (buf[0] == 'w') && (buf[5] == '!');
            (void)br_file_close(df);
        } else {
            ok = BR_FALSE;
        }
        pc(ok, "TC-VFS-016", "read/write/truncate through a link land on the target (link is not a copy)");
    }

/* ---- TC-VFS-017: absolute target and relative target containing '..' ---- */
    {
        char abs_link[BR_VFS_PATH_MAX_INTERNAL];
        char rel_link[BR_VFS_PATH_MAX_INTERNAL];
        conf_path(abs_link, (br_size_t)sizeof(abs_link), ldir, "abs1");
        conf_path(rel_link, (br_size_t)sizeof(rel_link), ldir, "rel1");

        (void)br_unlink(abs_link);
        (void)br_unlink(rel_link);

        const int e1 = br_symlink(tgt, abs_link);              /* absolute: matches mount table on restart */
        const int e2 = br_symlink("../vfsconf.lnk.d/target.bin", rel_link);

        br_stat_t st1;
        br_stat_t st2;
        br_bool ok = (e1 == BR_OK) && (e2 == BR_OK) &&
                     (br_stat(abs_link, &st1) == BR_OK) && (st1.type == BR_INODE_FILE) &&
                     (br_stat(rel_link, &st2) == BR_OK) && (st2.size == 6u);

        char longtgt[BR_SYMLINK_MAX + 8u];
        for (br_size_t i = 0u; i < sizeof(longtgt) - 1u; i++) {
            longtgt[i] = 'x';
        }
        longtgt[sizeof(longtgt) - 1u] = '\0';
        ok = ok && (br_symlink(longtgt, lnk) == BR_ERR(ENAMETOOLONG));

        (void)br_unlink(abs_link);
        (void)br_unlink(rel_link);
        pc(ok, "TC-VFS-017", "absolute target / relative target with '..' both follow; overlong target -ENAMETOOLONG");
    }

/* ---- TC-VFS-018: O_NOFOLLOW / loops => -ELOOP ---- */
    {
        char loop_a[BR_VFS_PATH_MAX_INTERNAL];
        char loop_b[BR_VFS_PATH_MAX_INTERNAL];
        conf_path(loop_a, (br_size_t)sizeof(loop_a), ldir, "loop_a");
        conf_path(loop_b, (br_size_t)sizeof(loop_b), ldir, "loop_b");
        (void)br_unlink(loop_a);
        (void)br_unlink(loop_b);

        int err = BR_OK;
        br_file_t *nf = br_open_err(lnk, BR_O_RDONLY | BR_O_NOFOLLOW, &err);
        const br_bool nofollow_ok = (nf == BR_NULL) && (err == BR_ERR(ELOOP));

        err = BR_OK;
        br_file_t *ff = br_open_err(lnk, BR_O_RDONLY, &err);     /* without NOFOLLOW: follows */
        const br_bool follow_ok = (ff != BR_NULL);
        if (ff != BR_NULL) {
            (void)br_file_close(ff);
        }

/* 2-cycle a -> b -> a: stat/open must return -ELOOP and must not hang. */
        br_bool loop_ok = (br_symlink("loop_b", loop_a) == BR_OK) &&
                          (br_symlink("loop_a", loop_b) == BR_OK);
        br_stat_t lst;
        loop_ok = loop_ok && (br_stat(loop_a, &lst) == BR_ERR(ELOOP));
        loop_ok = loop_ok && (br_lstat(loop_a, &lst) == BR_OK) &&
                  (lst.type == BR_INODE_SYMLINK);                /* lstat does not look at cycles */

        char self[BR_VFS_PATH_MAX_INTERNAL];
        conf_path(self, (br_size_t)sizeof(self), ldir, "self1");
        (void)br_unlink(self);
        loop_ok = loop_ok && (br_symlink("self1", self) == BR_OK) &&
                  (br_stat(self, &lst) == BR_ERR(ELOOP));

        (void)br_unlink(loop_a);
        (void)br_unlink(loop_b);
        (void)br_unlink(self);
        pc(nofollow_ok && follow_ok && loop_ok, "TC-VFS-018",
           "O_NOFOLLOW does not expand the final component => -ELOOP; 2-cycle/self-ref => -ELOOP (lstat still sees the link)");
    }

/* ---- TC-VFS-019: hard link = same data body; directory -EPERM; cross-mount -EXDEV ---- */
    {
        br_stat_t st_t;
        br_stat_t st_h;
        br_bool ok = (br_link(tgt, hard) == BR_OK) &&
                     (br_link(tgt, hard) == BR_ERR(EEXIST)) &&
                     (br_lstat(tgt, &st_t) == BR_OK) && (st_t.nlink == 2u) &&
                     (br_lstat(hard, &st_h) == BR_OK) && (st_h.nlink == 2u);

        char      buf[16];
        int       err = BR_OK;

/* Write via hard1, read back from target: truly one data body. */
        br_file_t *hf = br_open_err(hard, BR_O_RDWR, &err);
        if (hf != BR_NULL) {
            (void)br_file_write(hf, "ABC", 3u);
            (void)br_file_close(hf);
        } else {
            ok = BR_FALSE;
        }
        br_file_t *tf = br_open_err(tgt, BR_O_RDONLY, &err);
        if (tf != BR_NULL) {
            const br_s64 r = br_file_read(tf, buf, 3u);
            ok = ok && (r == 3) && (buf[0] == 'A') && (buf[2] == 'C');
            (void)br_file_close(tf);
        } else {
            ok = BR_FALSE;
        }

/* Deleting one name keeps the data (nlink 2 -> 1); the other name stays readable. */
        ok = ok && (br_unlink(tgt) == BR_OK) && (br_stat(hard, &st_h) == BR_OK) &&
             (st_h.nlink == 1u) && (st_h.size == 6u);
        tf = br_open_err(hard, BR_O_RDONLY, &err);
        if (tf != BR_NULL) {
            const br_s64 r = br_file_read(tf, buf, 6u);
            ok = ok && (r == 6) && (buf[0] == 'A');
            (void)br_file_close(tf);
        } else {
            ok = BR_FALSE;
        }

/* Directories cannot be hard-linked => -EPERM. */
        ok = ok && (br_link(ldir, lnk) == BR_ERR(EPERM));

/* Cross-mount => -EXDEV, using a node in the first non-root mount as target. */
        {
            const char *other = BR_NULL;
            for (br_u32 i = 0u; i < br_mount_count(); i++) {
                const char *mp = br_mount_path_at(i);
                if (mp != BR_NULL && br_vfs_internal_str_eq(mp, "/") == BR_FALSE) {
                    other = mp;
                    break;
                }
            }
            if (other == BR_NULL) {
                br_log_info("[VFSCONF] note: no second mount => TC-VFS-019 -EXDEV branch only verified as 'not applicable'");
            } else {
                char deep[BR_VFS_PATH_MAX_INTERNAL];
                conf_path(deep, (br_size_t)sizeof(deep), other, "uart0");
                const int r = br_link(deep, hard);
/* Target may not exist (no uart0 there) => only -ENOENT or -EXDEV. */
                ok = ok && ((r == BR_ERR(EXDEV)) || (r == BR_ERR(ENOENT)));
                if (r == BR_ERR(EXDEV)) {
                    br_log_info("[VFSCONF] note: cross-mount hard link => -EXDEV (%s -> %s)", deep, hard);
                }
            }
        }
        pc(ok, "TC-VFS-019", "hard links share the data body / nlink counting / deleting one name keeps data / directory -EPERM / cross-mount -EXDEV");
    }

/* ---- TC-VFS-020: unlink/rename act on the name; links count as directory entries ---- */
    {
        char f20[BR_VFS_PATH_MAX_INTERNAL];
        char sym20[BR_VFS_PATH_MAX_INTERNAL];
        char hard20[BR_VFS_PATH_MAX_INTERNAL];
        char ren20[BR_VFS_PATH_MAX_INTERNAL];
        conf_path(f20,    (br_size_t)sizeof(f20),    ldir, "f20.bin");
        conf_path(sym20,  (br_size_t)sizeof(sym20),  ldir, "sym20");
        conf_path(hard20, (br_size_t)sizeof(hard20), ldir, "hard20");
        conf_path(ren20,  (br_size_t)sizeof(ren20),  ldir, "ren20");

        (void)br_unlink(f20);
        (void)br_unlink(sym20);
        (void)br_unlink(hard20);
        (void)br_unlink(ren20);

        int  err = BR_OK;
        br_bool s[9];
        for (br_u32 k = 0u; k < 9u; k++) {
            s[k] = BR_FALSE;
        }

        br_file_t *f = br_open_err(f20, BR_O_CREAT | BR_O_RDWR | BR_O_TRUNC, &err);
        if (f != BR_NULL) {
            (void)br_file_write(f, "x", 1u);
            (void)br_file_close(f);
            s[0] = BR_TRUE;
        }

        br_stat_t st;
/* Following works; then unlink the link itself, target unaffected. */
        s[1] = (br_symlink("f20.bin", sym20) == BR_OK) &&
               (br_stat(sym20, &st) == BR_OK) && (st.type == BR_INODE_FILE);
        s[2] = (br_unlink(sym20) == BR_OK);
        s[3] = (br_lstat(sym20, &(br_stat_t){0}) == BR_ERR(ENOENT)) &&
               (br_stat(f20, &st) == BR_OK) && (st.size == 1u);

/* rename changes the name only: data and nlink unchanged. */
        s[4] = (br_link(f20, hard20) == BR_OK);
        s[5] = (br_stat(hard20, &st) == BR_OK) && (st.nlink == 2u);
        s[6] = (br_rename(hard20, ren20) == BR_OK) &&
               (br_stat(ren20, &st) == BR_OK) && (st.nlink == 2u) &&
               (br_lstat(hard20, &(br_stat_t){0}) == BR_ERR(ENOENT));

/* Links count as directory entries: non-empty directory => rmdir refuses. */
        s[7] = (br_rmdir(ldir) == BR_ERR(ENOTEMPTY));

/* Cleanup: ldir still holds link1 and hard1 from earlier cases. */
        s[8] = (br_unlink(ren20) == BR_OK) && (br_unlink(f20) == BR_OK) &&
               (br_unlink(lnk) == BR_OK) && (br_unlink(hard) == BR_OK) &&
               (br_rmdir(ldir) == BR_OK);

        pc(s[0] && s[1] && s[2] && s[3] && s[4] && s[5] && s[6] && s[7] && s[8],
           "TC-VFS-020", "unlink/rename act on the name; links count as directory entries (rmdir on non-empty => -ENOTEMPTY)");
    }

    br_log_info("[VFSCONF] SUMMARY pass=%u fail=%u total=%u", s_pass, s_fail, s_pass + s_fail);
    return (int)s_fail;
}
