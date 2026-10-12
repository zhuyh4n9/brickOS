/*
 * adapter/uboot-erofs — selftest suite (TC-EROFS-001..008)
 *
 * Design basis: `docs/decisions/0010-plugin-selftest.md` (selftest separate from production).
 * Contract with br_plugin_manager_selftest(): return the **number of failures** (0 = all green); each case logs
 * `[EROFCONF] PASS/FAIL <tag> <desc>`, and the summary logs `[EROFCONF] SUMMARY pass=N fail=M`.
 *
 * WORKAROUND(br-wa-test-001): `TC-EROFS-*` is a **self-assigned numbering** — the whole group does not exist in
 * the design `6-01` case table (it is **new surface** of the 7-storage domain); registered alongside the other
 * families' numbering debt.
 *
 * ============================== Where the criteria come from ==============================
 *   1. **Public-surface driven**: uses `br_stat` / `br_open` / `br_file_read` / `br_file_lseek` /
 *      `br_opendir` / `br_readdir` / `br_unlink` / `br_mkdir` —— exactly what an APP can use.
 *      Only then is "EROFS is really wired into the VFS" actually tested (looking at the internal `erofs_ilookup` is not enough).
 *   2. **On-disk fact**: the criterion "magic must be at offset 1024" reads the **medium bytes** directly and compares
 *      (`erofs_dev_read()`), rather than self-certifying with our own parse result.
 *   3. **Generative content criteria**: the two compressed fixtures have recomputable formulas (see the "fixture constants"
 *      in erofs_internal.h), so this file has **no large literals** — 70000 bytes of content are computed as
 *      `"ABCDEFGH"[i % 8]`, and raw.bin from a linear congruential formula.
 *   4. **Assert directory entries by name, not by total count**: adding one file to the fixture must not turn a case red.
 *
 * ============================== One deliberate tradeoff ==============================
 *   TC-EROFS-007 calls `br_unlink()` / `br_mkdir()` — they do **not** modify the volume
 *   (the return value is just -EROFS, not a byte is written). So these "mutation" cases are safe on a read-only volume
 *   and need no cleanup.
 * ==========================================================================
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "internal.h"        /* upstream fs/erofs/internal.h (compiled as-is) */
#include "erofs_glue.h"      /* adapter-internal contract (declarations of this project) */

int uboot_erofs_selftest(void);

/* =====================================================================
 * Summary and printing
 * ===================================================================== */

static br_u32 s_pass;
static br_u32 s_fail;

static void er_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_pass++;
        br_log_info("[EROFCONF] PASS %s %s", tag, what);
    } else {
        s_fail++;
        br_log_info("[EROFCONF] FAIL %s %s", tag, what);
    }
}

/* =====================================================================
 * Small helpers (selftest's own; no dependency on runtime/posix)
 * ===================================================================== */

static br_size_t ef_strlen(const char *s)
{
    br_size_t n = 0u;

    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static br_bool ef_streq(const char *a, const char *b)
{
    return (uboot_erofs_strcmp(a, b) == 0) ? BR_TRUE : BR_FALSE;
}

/* Do all bytes of the buffer equal (seed + i) & 0xFF? */
static br_bool ef_check_seq(const u8 *p, br_size_t n, br_size_t base)
{
    for (br_size_t i = 0u; i < n; i++) {
        if (p[i] != (u8)((base + i) & 0xFFu)) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

/* Compressed fixture: byte i = "ABCDEFGH"[i % 8] */
static u8 ef_lz4_byte(br_size_t i)
{
    static const char pat[EROFS_FX_LZ4_PERIOD + 1u] = EROFS_FX_LZ4_PATTERN;

    return (u8)pat[i % (br_size_t)EROFS_FX_LZ4_PERIOD];
}

/* Compressed fixture: linear congruential formula for raw.bin (identical to the host script) */
static u8 ef_raw_byte(br_size_t i)
{
    const br_u32 v = (br_u32)(((br_u64)i * 1103515245u) + 12345u);

    return (u8)((v >> 16) & 0xFFu);
}

/*
 * Fetch one directory entry by name. `want_dir` = accept only directory entries (otherwise only non-directory entries).
 * Returns 1 = found, 0 = no such name, negative = errno.
 */
static int ef_find_dirent(const char *dir_path, const char *name, br_u32 *type_out,
                          br_u64 *size_out)
{
    br_dir_t   *d = br_opendir(dir_path);
    br_dirent_t de;
    int         rc = 0;

    if (d == BR_NULL) {
        return BR_ERR(ENOENT);
    }
    for (;;) {
        const int r = br_readdir(d, &de);

        if (r < 0) {
            rc = r;
            break;
        }
        if (r == 0) {
            break;                          /* exhausted */
        }
        if (ef_streq(de.name, name) == BR_TRUE) {
            *type_out = de.type;
            /* A directory entry carries no size (the contract's br_dirent_t has no such field) — caller does br_stat if needed */
            *size_out = 0u;
            rc = 1;
            break;
        }
    }
    (void)br_closedir(d);
    return rc;
}

/* Read a whole file (through the VFS) and compare against the generative criterion. `kind` selects the formula. */
static br_bool ef_read_whole(const char *path, br_u64 expect_size, br_u32 kind)
{
    br_file_t *f = br_open(path, BR_O_RDONLY);
    u8         buf[512];
    br_size_t  done = 0u;
    br_bool    ok = BR_TRUE;

    if (f == BR_NULL) {
        return BR_FALSE;
    }
    for (;;) {
        const br_s64 r = br_file_read(f, buf, (br_size_t)sizeof(buf));

        if (r < 0) {
            ok = BR_FALSE;
            break;
        }
        if (r == 0) {
            break;
        }
        for (br_s64 i = 0; i < r; i++) {
            const br_size_t pos = done + (br_size_t)i;
            br_bool good;

            if (kind == 0u) {
                good = (buf[i] == ef_lz4_byte(pos)) ? BR_TRUE : BR_FALSE;
            } else if (kind == 1u) {
                good = (buf[i] == ef_raw_byte(pos)) ? BR_TRUE : BR_FALSE;
            } else if (kind == 2u) {
                good = (buf[i] == (u8)(pos & 0xFFu)) ? BR_TRUE : BR_FALSE;
            } else {
                good = (buf[i] == (u8)EROFS_FX_HELLO_TEXT[pos]) ? BR_TRUE : BR_FALSE;
            }
            if (good == BR_FALSE) {
                br_log_error("uboot-erofs: %s byte %llu mismatch (got 0x%02x)",
                             path, (unsigned long long)pos, (unsigned)buf[i]);
                ok = BR_FALSE;
                break;
            }
        }
        if (ok == BR_FALSE) {
            break;
        }
        done += (br_size_t)r;
    }
    (void)br_file_close(f);

    if (ok == BR_FALSE) {
        return BR_FALSE;
    }
    if ((br_u64)done != expect_size) {
        br_log_error("uboot-erofs: %s length mismatch (got %llu, expected %llu)",
                     path, (unsigned long long)done, (unsigned long long)expect_size);
        return BR_FALSE;
    }
    return BR_TRUE;
}

/* =====================================================================
 * Test cases
 * ===================================================================== */

/* ---- TC-EROFS-001: mount table / superblock / root inode ---- */
static void ef_tc001(void)
{
    br_bool in_table = BR_FALSE;

    for (br_u32 i = 0u; i < br_mount_count(); i++) {
        const char *p = br_mount_path_at(i);

        if ((p != BR_NULL) && (ef_streq(p, BR_EROFS_MOUNT_PATH) == BR_TRUE)) {
            in_table = BR_TRUE;
            break;
        }
    }

    /* ★ On-disk fact: read the 4-byte magic at offset 1024 directly, no self-certifying with the parse result */
    u8 sb[16];
    br_u32 magic = 0u;
    const int rd = erofs_dev_read(0, sb, (u64)EROFS_SUPER_OFFSET, sizeof(sb));

    if (rd == 0) {
        magic = (br_u32)sb[0] | ((br_u32)sb[1] << 8) | ((br_u32)sb[2] << 16) |
                ((br_u32)sb[3] << 24);
    }

    /* Root inode: nid must really be a directory ("nid in range" alone is too weak) */
    struct erofs_inode root;
    root.nid = sbi.root_nid;
    const int irc = erofs_read_inode_from_disk(&root);
    const umode_t rmode = root.i_mode;      /* take it immediately: another read overwrites the inode in scratch */
    const erofs_nid_t rnid = root.nid;

    const br_bool ok =
        (in_table == BR_TRUE) &&
        (rd == 0) && (magic == EROFS_SUPER_MAGIC_V1) &&
        (sbi.blkszbits == EROFS_BLOCK_SIZE_BITS) &&
        (erofs_blksiz() == EROFS_MAX_BLOCK_SIZE) &&
        (sbi.total_blocks > 0u) &&
        (sbi.meta_blkaddr < sbi.total_blocks) &&
        (sbi.root_nid < (erofs_nid_t)sbi.inos * 8u) &&
        (irc == 0) && (S_ISDIR(rmode)) &&
        (rnid == sbi.root_nid) &&
        (uboot_erofs_dev.sector_size == 512u) &&
        (uboot_erofs_dev.n_sectors > 0u);

    er_conf(ok, "TC-EROFS-001",
            "mount table has /assets; on-disk magic at offset 1024 correct; blkszbits=12; root_nid is a directory");
}

/* ---- TC-EROFS-002: uncompressed (FLAT_INLINE) whole-file read ---- */
static void ef_tc002(void)
{
    br_stat_t st;
    const int src = br_stat("/assets/" EROFS_FX_HELLO_PATH, &st);

    /* Uncompressed flat criterion: layout is FLAT_INLINE / FLAT_PLAIN and content matches byte for byte */
    struct erofs_inode vi;
    vi.nid = 0;
    const int lrc = erofs_ilookup(EROFS_FX_HELLO_PATH, &vi);
    const unsigned char layout = vi.datalayout;
    const erofs_off_t   vsize  = vi.i_size;

    const br_bool ok =
        (src == 0) && (st.type == BR_INODE_FILE) && (st.size == (br_u64)EROFS_FX_HELLO_SIZE) &&
        (lrc == 0) &&
        (layout == EROFS_INODE_FLAT_INLINE || layout == EROFS_INODE_FLAT_PLAIN) &&
        (vsize == (erofs_off_t)EROFS_FX_HELLO_SIZE) &&
        (ef_read_whole("/assets/" EROFS_FX_HELLO_PATH, EROFS_FX_HELLO_SIZE, 3u) == BR_TRUE) &&
        (ef_strlen(EROFS_FX_HELLO_TEXT) == (br_size_t)EROFS_FX_HELLO_SIZE);

    er_conf(ok, "TC-EROFS-002",
            "hello.txt: 12 bytes \"erofs hello\\n\" byte for byte (FLAT_INLINE/FLAT_PLAIN path)");
}

/* ---- TC-EROFS-003: extent split at non-zero offset (mid-file read) ---- */
static void ef_tc003(void)
{
    br_file_t *f = br_open("/assets/" EROFS_FX_ABIN_PATH, BR_O_RDONLY);
    u8   buf[EROFS_FX_ABIN_MID_LEN];
    br_bool ok = BR_FALSE;

    if (f != BR_NULL) {
        const br_s64 sk = br_file_lseek(f, (br_s64)EROFS_FX_ABIN_MID_OFF, BR_SEEK_SET);
        const br_s64 rd = br_file_read(f, buf, (br_size_t)sizeof(buf));

        ok = (sk == (br_s64)EROFS_FX_ABIN_MID_OFF) &&
             (rd == (br_s64)EROFS_FX_ABIN_MID_LEN) &&
             (ef_check_seq(buf, (br_size_t)sizeof(buf), (br_size_t)EROFS_FX_ABIN_MID_OFF));
        (void)br_file_close(f);
    }
    er_conf(ok, "TC-EROFS-003",
            "dir/a.bin mid read (offset 1000, len 500) byte for byte = i&0xFF (extent split at non-zero offset)");
}

/* ---- TC-EROFS-004: compressed path + LZ4 decompression (70000 B of repeated "ABCDEFGH") ---- */
static void ef_tc004(void)
{
    struct erofs_inode vi;
    vi.nid = 0;
    const int lrc = erofs_ilookup(EROFS_FX_LZ4_PATH, &vi);
    const unsigned char layout = vi.datalayout;
    const erofs_off_t   vsize  = vi.i_size;

    const br_bool ok =
        (lrc == 0) &&
        (layout == EROFS_INODE_COMPRESSED_COMPACT || layout == EROFS_INODE_COMPRESSED_FULL) &&
        (vsize == (erofs_off_t)EROFS_FX_LZ4_SIZE) &&
        (ef_read_whole("/assets/" EROFS_FX_LZ4_PATH, EROFS_FX_LZ4_SIZE, 0u) == BR_TRUE);

    er_conf(ok, "TC-EROFS-004",
            "comp/lz4.txt: 70000 B byte for byte = \"ABCDEFGH\"[i%8] (compressed blocks + LZ4 decompression, with tail extent)");
}

/* ---- TC-EROFS-005: directory surface (readdir) asserted by name/type ---- */
static void ef_tc005(void)
{
    br_u32  t = 0u;
    br_u64  sz = 0u;
    br_stat_t st;
    struct erofs_inode vi;

    const int dot   = ef_find_dirent("/assets", ".", &t, &sz);
    const br_u32 dot_t = t;
    const int dotd  = ef_find_dirent("/assets", "..", &t, &sz);
    const br_u32 dotd_t = t;
    const int comp  = ef_find_dirent("/assets", "comp", &t, &sz);
    const br_u32 comp_t = t;
    const int dir   = ef_find_dirent("/assets", "dir", &t, &sz);
    const br_u32 dir_t = t;
    const int hello = ef_find_dirent("/assets", "hello.txt", &t, &sz);
    const br_u32 hello_t = t;
    const int raw   = ef_find_dirent("/assets", "raw.bin", &t, &sz);
    const br_u32 raw_t = t;
    const int deep  = ef_find_dirent("/assets/dir/sub", "deep.txt", &t, &sz);
    const br_u32 deep_t = t;
    const int statd = br_stat("/assets/dir", &st);

    vi.nid = 0;
    const int arc = erofs_ilookup(EROFS_FX_ABIN_PATH, &vi);
    const erofs_off_t asize = vi.i_size;
    const unsigned char alayout = vi.datalayout;

    vi.nid = 0;
    const int drc = erofs_ilookup(EROFS_FX_DEEP_PATH, &vi);
    const erofs_off_t dsize = vi.i_size;

    const br_bool ok =
        (dot == 1) && (dot_t == BR_INODE_DIR) &&
        (dotd == 1) && (dotd_t == BR_INODE_DIR) &&
        (comp == 1) && (comp_t == BR_INODE_DIR) &&
        (dir == 1) && (dir_t == BR_INODE_DIR) &&
        (hello == 1) && (hello_t == BR_INODE_FILE) &&
        (raw == 1) && (raw_t == BR_INODE_FILE) &&
        (deep == 1) && (deep_t == BR_INODE_FILE) &&
        (statd == 0) && (st.type == BR_INODE_DIR) &&
        (arc == 0) && (asize == (erofs_off_t)EROFS_FX_ABIN_SIZE) &&
        (alayout == EROFS_INODE_FLAT_PLAIN) &&
        (drc == 0) && (dsize == (erofs_off_t)EROFS_FX_DEEP_SIZE);

    er_conf(ok, "TC-EROFS-005",
            "readdir /assets and /assets/dir/sub: ./.. present, comp/dir are directories, hello.txt/raw.bin/deep.txt are files");
}

/* ---- TC-EROFS-006: nonexistent ⇒ BR_NULL / -ENOENT; br_stat type/size correct ---- */

/*
 * ★ Deliberately does **not** reuse one `br_stat_t` variable across calls: in the previous form
 *   `br_stat(a, &st); br_stat(b, &st); ...; (st.size == 12)` the last call overwrote the earlier
 *   result — so the criterion "always compared the last file's size" (measured: deep.txt's
 *   5 was compared against hello.txt's 12). This helper wraps "call + take snapshot immediately",
 *   so each path gets its own attributes.
 */
static int ef_stat_snapshot(const char *path, br_stat_t *out)
{
    br_stat_t st;

    (void)__builtin_memset(&st, 0, sizeof st);
    const int rc = br_stat(path, &st);
    *out = st;
    return rc;
}

static void ef_tc006(void)
{
    br_stat_t sd, sf, ss, sk;

    /* Public surface: nonexistent path ⇒ -ENOENT (checked via both br_open and br_stat) */
    br_file_t *nf = br_open("/assets/no-such-file", BR_O_RDONLY);
    const int  nst = ef_stat_snapshot("/assets/no-such-file", &sd);

    /* Type/size: one independent snapshot per path */
    const int dird = ef_stat_snapshot("/assets/dir", &sd);
    const int fild = ef_stat_snapshot("/assets/" EROFS_FX_HELLO_PATH, &sf);
    const int subd = ef_stat_snapshot("/assets/dir/sub", &ss);
    const int deep = ef_stat_snapshot("/assets/" EROFS_FX_DEEP_PATH, &sk);

    /* Internal surface: "no such name" must be distinct from "read failure" (upstream uses ERR_PTR, this file uses errno) */
    struct erofs_inode vi;
    vi.nid = 0;
    const int lrc = erofs_ilookup("dir/not-here.txt", &vi);

    const br_bool ok =
        (nf == BR_NULL) &&
        (nst == BR_ERR(ENOENT)) &&
        (dird == 0) && (sd.type == BR_INODE_DIR) &&
        (fild == 0) && (sf.type == BR_INODE_FILE) && (sf.size == (br_u64)EROFS_FX_HELLO_SIZE) &&
        (subd == 0) && (ss.type == BR_INODE_DIR) &&
        (deep == 0) && (sk.type == BR_INODE_FILE) && (sk.size == (br_u64)EROFS_FX_DEEP_SIZE) &&
        (lrc == BR_ERR(ENOENT));

    er_conf(ok, "TC-EROFS-006",
            "lookup nonexistent ⇒ BR_NULL (vfs gets -ENOENT); br_stat type and size correct for dirs/files");
}

/* ---- TC-EROFS-007: read-only enforcement ---- */
static void ef_tc007(void)
{
    br_file_t *w  = br_open("/assets/" EROFS_FX_HELLO_PATH, BR_O_WRONLY);
    br_file_t *rw = br_open("/assets/" EROFS_FX_HELLO_PATH, BR_O_RDWR);
    br_file_t *df = br_open("/assets", BR_O_RDONLY);      /* read-only open of a directory: should succeed */
    const int  un = br_unlink("/assets/" EROFS_FX_ABIN_PATH);
    const int  mk = br_mkdir("/assets/newdir");
    const int  rm = br_rmdir("/assets/dir");
    const int  rn = br_rename("/assets/" EROFS_FX_HELLO_PATH, "/assets/renamed.txt");
    const int  tr = br_truncate("/assets/" EROFS_FX_HELLO_PATH, 0u);
    const int  sl = br_symlink("target", "/assets/newlink");
    const int  lk = br_link("/assets/" EROFS_FX_HELLO_PATH, "/assets/hardlink.txt");

    /* A directory can indeed be opened read-only (don't make "read-only" mean "cannot open") */
    const br_bool dirok = (df != BR_NULL) ? BR_TRUE : BR_FALSE;
    if (df != BR_NULL) {
        (void)br_file_close(df);
    }

    const br_bool ok =
        (w == BR_NULL) && (rw == BR_NULL) && (dirok == BR_TRUE) &&
        (un == BR_ERR(EROFS)) && (mk == BR_ERR(EROFS)) &&
        (rm == BR_ERR(EROFS)) && (rn == BR_ERR(EROFS)) &&
        (tr == BR_ERR(EROFS)) &&
        (sl == BR_ERR(EROFS)) && (lk == BR_ERR(EROFS));

    er_conf(ok, "TC-EROFS-007",
            "write open ⇒ BR_NULL; unlink/mkdir/rmdir/rename/truncate/symlink/link ⇒ -EROFS");
}

/* ---- TC-EROFS-008: whole-file read across extents + size boundary (read == 0 after EOF) ---- */
static void ef_tc008(void)
{
    /* raw.bin is a compressed 8192 B (2 blocks) file ⇒ a whole-file read must cross extents; a.bin is a
     * 4096 B flat file ⇒ a whole-file read covers both "full block + block boundary" paths. */
    const br_bool raw_ok  = ef_read_whole("/assets/" EROFS_FX_RAW_PATH, EROFS_FX_RAW_SIZE, 1u);
    const br_bool abin_ok = ef_read_whole("/assets/" EROFS_FX_ABIN_PATH, EROFS_FX_ABIN_SIZE, 2u);

    /* size boundary: after lseek to EOF (and past EOF) read must return 0, **not** an error */
    br_file_t *f = br_open("/assets/" EROFS_FX_LZ4_PATH, BR_O_RDONLY);
    br_bool eof_ok = BR_FALSE;
    br_bool past_ok = BR_FALSE;

    if (f != BR_NULL) {
        u8 one = 0u;

        const br_s64 e = br_file_lseek(f, 0, BR_SEEK_END);
        const br_s64 r0 = br_file_read(f, &one, 1u);
        const br_s64 p = br_file_lseek(f, 4096, BR_SEEK_CUR);
        const br_s64 r1 = br_file_read(f, &one, 1u);

        eof_ok  = (e == (br_s64)EROFS_FX_LZ4_SIZE) && (r0 == 0);
        past_ok = (p == (br_s64)EROFS_FX_LZ4_SIZE + 4096) && (r1 == 0);
        (void)br_file_close(f);
    }

    const br_bool ok = (raw_ok == BR_TRUE) && (abin_ok == BR_TRUE) &&
                       (eof_ok == BR_TRUE) && (past_ok == BR_TRUE);

    er_conf(ok, "TC-EROFS-008",
            "raw.bin(8192)/a.bin(4096) whole-file byte for byte; read == 0 at SEEK_END and past EOF");
}

/* =====================================================================
 * Entry point
 * ===================================================================== */

int uboot_erofs_selftest(void)
{
    s_pass = 0u;
    s_fail = 0u;

    br_log_info("[EROFCONF] uboot-erofs conformance (mount / flat / compressed+LZ4 / dirent / read-only)");

    ef_tc001();
    ef_tc002();
    ef_tc003();
    ef_tc004();
    ef_tc005();
    ef_tc006();
    ef_tc007();
    ef_tc008();

    br_log_info("[EROFCONF] SUMMARY pass=%u fail=%u total=%u",
                (unsigned)s_pass, (unsigned)s_fail, (unsigned)(s_pass + s_fail));
    return (int)s_fail;
}
