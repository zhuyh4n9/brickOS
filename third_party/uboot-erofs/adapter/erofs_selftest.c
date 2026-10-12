/*
 * third_party/uboot-erofs — 自检套件(TC-EROFS-001..008)
 *
 * 设计依据: `docs/decisions/0010-plugin-selftest.md`(自检与生产分离)。
 * 与 br_plugin_manager_selftest() 的契约: 返回**失败项数**(0 = 全绿); 每条打
 * `[EROFCONF] PASS/FAIL <tag> <desc>`, 收尾打 `[EROFCONF] SUMMARY pass=N fail=M`。
 *
 * WORKAROUND(br-wa-test-001): `TC-EROFS-*` 是**自编号** —— 设计 `6-01` 的用例表里
 * 整组不存在(属 7-storage 域的**新增面**); 与其余各族的编号债同一处登记。
 *
 * ============================== 判据从哪来 ==============================
 *   1. **公开面驱动**: 走 `br_stat` / `br_open` / `br_file_read` / `br_file_lseek` /
 *      `br_opendir` / `br_readdir` / `br_unlink` / `br_mkdir` —— 也就是 APP 能用的那一套。
 *      这样"EROFS 真的接进了 VFS"这件事才有判据(只看内部的 `erofs_ilookup` 不够)。
 *   2. **盘上事实**: 「magic 必须在偏移 1024」这条判据**直接读介质字节**再比对
 *      (`uboot_erofs_dev_read()`), 不拿我们自己解析出来的结果自证。
 *   3. **生成式内容判据**: 夹具里的两个压缩件分别有可重算的式子(见 erofs_internal.h
 *      的"夹具常量"), 所以本文件里**没有任何大块字面量** —— 70000 字节的内容用
 *      `"ABCDEFGH"[i % 8]` 现算, raw.bin 用线性同余式现算。
 *   4. **按名字断言目录条目, 不数总数**: 夹具加一个文件不该让用例变红。
 *
 * ============================== 一个刻意的取舍 ==============================
 *   TC-EROFS-007 会调 `br_unlink()` / `br_mkdir()` —— 它们**不会**改动卷
 *   (返回值就是 -EROFS, 一个字节都不写)。所以这些"变更"用例在只读卷上是安全的,
 *   也不需要清理。
 * ==========================================================================
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>

#include "erofs_internal.h"

int uboot_erofs_selftest(void);

/* =====================================================================
 * 汇总与打印
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
 * 小工具(自检自己的; 不依赖 runtime/posix)
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

/* 字节缓冲的所有字节都等于 (seed + i) & 0xFF 这条式子? */
static br_bool ef_check_seq(const u8 *p, br_size_t n, br_size_t base)
{
    for (br_size_t i = 0u; i < n; i++) {
        if (p[i] != (u8)((base + i) & 0xFFu)) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

/* 压缩夹具: 第 i 字节 = "ABCDEFGH"[i % 8] */
static u8 ef_lz4_byte(br_size_t i)
{
    static const char pat[EROFS_FX_LZ4_PERIOD + 1u] = EROFS_FX_LZ4_PATTERN;

    return (u8)pat[i % (br_size_t)EROFS_FX_LZ4_PERIOD];
}

/* 压缩夹具: raw.bin 的线性同余式(与宿主脚本逐字一致) */
static u8 ef_raw_byte(br_size_t i)
{
    const br_u32 v = (br_u32)(((br_u64)i * 1103515245u) + 12345u);

    return (u8)((v >> 16) & 0xFFu);
}

/*
 * 按名字取一条目录项。`want_dir` = 只接受目录项(否则只接受非目录项)。
 * 返回 1 = 找到, 0 = 没有这个名字, 负数 = errno。
 */
static int ef_find_dirent(const char *dir_path, const char *name, br_u32 *type_out,
                          br_u64 *size_out)
{
    br_dir_t   *d = br_opendir(dir_path);
    br_dirent_t de;
    int         rc = 0;

    if (d == BR_NULL) {
        return BR_ERR(BR_ENOENT);
    }
    for (;;) {
        const int r = br_readdir(d, &de);

        if (r < 0) {
            rc = r;
            break;
        }
        if (r == 0) {
            break;                          /* 穷尽 */
        }
        if (ef_streq(de.name, name) == BR_TRUE) {
            *type_out = de.type;
            /* 目录项不带大小(契约的 br_dirent_t 没有这个字段)—— 调用方按需 br_stat */
            *size_out = 0u;
            rc = 1;
            break;
        }
    }
    (void)br_closedir(d);
    return rc;
}

/* 读整个文件(经 VFS)并与生成式判据比对。`kind` 决定用哪条式子。 */
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
                br_log_error("uboot-erofs: %s 第 %llu 字节不符(实得 0x%02x)",
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
        br_log_error("uboot-erofs: %s 长度对不上(实得 %llu, 期望 %llu)",
                     path, (unsigned long long)done, (unsigned long long)expect_size);
        return BR_FALSE;
    }
    return BR_TRUE;
}

/* =====================================================================
 * 用例
 * ===================================================================== */

/* ---- TC-EROFS-001: 挂载表 / 超级块 / 根 inode ---- */
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

    /* ★ 盘上事实: 直接读偏移 1024 的 4 字节 magic, 不拿解析结果自证 */
    u8 sb[16];
    br_u32 magic = 0u;
    const int rd = uboot_erofs_dev_read(0, sb, (u64)EROFS_SUPER_OFFSET, sizeof(sb));

    if (rd == 0) {
        magic = (br_u32)sb[0] | ((br_u32)sb[1] << 8) | ((br_u32)sb[2] << 16) |
                ((br_u32)sb[3] << 24);
    }

    /* 根 inode: nid 必须真的是一个目录(只查"nid 在范围内"太弱) */
    struct erofs_inode root;
    root.nid = uboot_erofs_sbi.root_nid;
    const int irc = erofs_read_inode_from_disk(&root);
    const umode_t rmode = root.i_mode;      /* 立刻取走: 别的读会盖掉 scratch 里的 inode */
    const erofs_nid_t rnid = root.nid;

    const br_bool ok =
        (in_table == BR_TRUE) &&
        (rd == 0) && (magic == EROFS_SUPER_MAGIC_V1) &&
        (uboot_erofs_sbi.blkszbits == EROFS_BLOCK_SIZE_BITS) &&
        (erofs_blksiz() == EROFS_MAX_BLOCK_SIZE) &&
        (uboot_erofs_sbi.total_blocks > 0u) &&
        (uboot_erofs_sbi.meta_blkaddr < uboot_erofs_sbi.total_blocks) &&
        (uboot_erofs_sbi.root_nid < (erofs_nid_t)uboot_erofs_sbi.inos * 8u) &&
        (irc == 0) && (S_ISDIR(rmode)) &&
        (rnid == uboot_erofs_sbi.root_nid) &&
        (uboot_erofs_dev.sector_size == 512u) &&
        (uboot_erofs_dev.n_sectors > 0u);

    er_conf(ok, "TC-EROFS-001",
            "挂载表含 /assets; 盘上偏移 1024 的 magic 正确; blkszbits=12; root_nid 是目录");
}

/* ---- TC-EROFS-002: 未压缩(FLAT_INLINE)整文件读 ---- */
static void ef_tc002(void)
{
    br_stat_t st;
    const int src = br_stat("/assets/" EROFS_FX_HELLO_PATH, &st);

    /* 未压缩 flat 的判据: 布局是 FLAT_INLINE / FLAT_PLAIN 之一 + 内容逐字节一致 */
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
            "hello.txt: 12 字节 \"erofs hello\\n\" 逐字节一致(FLAT_INLINE/FLAT_PLAIN 路径)");
}

/* ---- TC-EROFS-003: 非零 offset 的 extent 切分(中段读) ---- */
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
            "dir/a.bin 中段读(offset 1000, len 500)逐字节 = i&0xFF(非零 offset 的 extent 切分)");
}

/* ---- TC-EROFS-004: 压缩路径 + LZ4 解压(70000 B 的 "ABCDEFGH" 重复) ---- */
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
            "comp/lz4.txt: 70000 B 逐字节 = \"ABCDEFGH\"[i%8](压缩块 + LZ4 解压, 含尾 extent)");
}

/* ---- TC-EROFS-005: 目录面(readdir)按名字/类型断言 ---- */
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
            "readdir /assets 与 /assets/dir/sub: ./.. 在, comp/dir 是目录, hello.txt/raw.bin/deep.txt 是文件");
}

/* ---- TC-EROFS-006: 不存在 ⇒ BR_NULL / -ENOENT; br_stat 的类型/大小正确 ---- */

/*
 * ★ 这里刻意**不**复用同一个 `br_stat_t` 变量横跨多次调用: 上一版的写法
 *   `br_stat(a, &st); br_stat(b, &st); ...; (st.size == 12)` 里, 最后一次调用会把
 *   前一次的结果盖掉 —— 判据于是"总是拿最后一个文件的 size 比" (实测: deep.txt 的
 *   5 被拿去和 hello.txt 的 12 比)。把"调用 + 立刻取快照"收进这个小函数,
 *   每个路径各拿一份自己的属性。
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

    /* 公开面: 不存在的路径 ⇒ -ENOENT(用 br_open 与 br_stat 两条路各验一次) */
    br_file_t *nf = br_open("/assets/no-such-file", BR_O_RDONLY);
    const int  nst = ef_stat_snapshot("/assets/no-such-file", &sd);

    /* 类型/大小: 每个路径一份独立快照 */
    const int dird = ef_stat_snapshot("/assets/dir", &sd);
    const int fild = ef_stat_snapshot("/assets/" EROFS_FX_HELLO_PATH, &sf);
    const int subd = ef_stat_snapshot("/assets/dir/sub", &ss);
    const int deep = ef_stat_snapshot("/assets/" EROFS_FX_DEEP_PATH, &sk);

    /* 内部面: "没这个名字"必须与"读失败"分开(上游用 ERR_PTR, 本件用 errno) */
    struct erofs_inode vi;
    vi.nid = 0;
    const int lrc = erofs_ilookup("dir/not-here.txt", &vi);

    const br_bool ok =
        (nf == BR_NULL) &&
        (nst == BR_ERR(BR_ENOENT)) &&
        (dird == 0) && (sd.type == BR_INODE_DIR) &&
        (fild == 0) && (sf.type == BR_INODE_FILE) && (sf.size == (br_u64)EROFS_FX_HELLO_SIZE) &&
        (subd == 0) && (ss.type == BR_INODE_DIR) &&
        (deep == 0) && (sk.type == BR_INODE_FILE) && (sk.size == (br_u64)EROFS_FX_DEEP_SIZE) &&
        (lrc == BR_ERR(BR_ENOENT));

    er_conf(ok, "TC-EROFS-006",
            "lookup 不存在 ⇒ BR_NULL(vfs 得 -ENOENT); br_stat 对目录/文件类型与大小正确");
}

/* ---- TC-EROFS-007: 只读执法 ---- */
static void ef_tc007(void)
{
    br_file_t *w  = br_open("/assets/" EROFS_FX_HELLO_PATH, BR_O_WRONLY);
    br_file_t *rw = br_open("/assets/" EROFS_FX_HELLO_PATH, BR_O_RDWR);
    br_file_t *df = br_open("/assets", BR_O_RDONLY);      /* 目录只读打开: 应当成功 */
    const int  un = br_unlink("/assets/" EROFS_FX_ABIN_PATH);
    const int  mk = br_mkdir("/assets/newdir");
    const int  rm = br_rmdir("/assets/dir");
    const int  rn = br_rename("/assets/" EROFS_FX_HELLO_PATH, "/assets/renamed.txt");
    const int  tr = br_truncate("/assets/" EROFS_FX_HELLO_PATH, 0u);
    const int  sl = br_symlink("target", "/assets/newlink");
    const int  lk = br_link("/assets/" EROFS_FX_HELLO_PATH, "/assets/hardlink.txt");

    /* 目录确实能只读打开(别把"只读"做成了"打不开") */
    const br_bool dirok = (df != BR_NULL) ? BR_TRUE : BR_FALSE;
    if (df != BR_NULL) {
        (void)br_file_close(df);
    }

    const br_bool ok =
        (w == BR_NULL) && (rw == BR_NULL) && (dirok == BR_TRUE) &&
        (un == BR_ERR(BR_EROFS)) && (mk == BR_ERR(BR_EROFS)) &&
        (rm == BR_ERR(BR_EROFS)) && (rn == BR_ERR(BR_EROFS)) &&
        (tr == BR_ERR(BR_EROFS)) &&
        (sl == BR_ERR(BR_EROFS)) && (lk == BR_ERR(BR_EROFS));

    er_conf(ok, "TC-EROFS-007",
            "写打开 ⇒ BR_NULL; unlink/mkdir/rmdir/rename/truncate/symlink/link ⇒ -EROFS");
}

/* ---- TC-EROFS-008: 跨 extent 整文件读 + size 边界(EOF 后 read == 0) ---- */
static void ef_tc008(void)
{
    /* raw.bin 是 8192 B(2 个块)的压缩件 ⇒ 整文件读必然跨 extent; a.bin 是 4096 B 的
     * 平面件 ⇒ 整文件读覆盖"整块 + 块边界"两条路径。 */
    const br_bool raw_ok  = ef_read_whole("/assets/" EROFS_FX_RAW_PATH, EROFS_FX_RAW_SIZE, 1u);
    const br_bool abin_ok = ef_read_whole("/assets/" EROFS_FX_ABIN_PATH, EROFS_FX_ABIN_SIZE, 2u);

    /* size 边界: lseek 到 EOF(与越过 EOF)之后 read 必须返回 0,**不是**错误 */
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
            "raw.bin(8192)/a.bin(4096) 整文件逐字节一致; SEEK_END 与越过 EOF 后 read == 0");
}

/* =====================================================================
 * 入口
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
