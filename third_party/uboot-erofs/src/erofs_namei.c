/*
 * third_party/uboot-erofs — inode 解析与路径走查(移植自 u-boot `fs/erofs/namei.c`, GPL-2.0+)
 *
 * ============================== 上游对照 ==============================
 *   上游文件 : fs/erofs/namei.c(266 行)
 *   移植范围 : `erofs_read_inode_from_disk()`(盘上 inode → 内存 inode)、
 *              `find_target_dirent()`(块内 dirent 查找)、`erofs_namei()`(逐级下推)、
 *              `link_path_walk()` + `erofs_ilookup()`(路径风格入口)。
 *   逐函数对照:
 *     erofs_new_decode_dev()/makedev() → **裁掉**(设备号本件不用, 见"偏离 1")
 *     erofs_read_inode_from_disk()     → 保留(两个布局: COMPACT 32 B / EXTENDED 64 B)
 *     find_target_dirent()             → 保留, 但返回值改成"错误码 + 出参"(见"偏离 2")
 *     struct nameidata / erofs_namei() → 保留(它只装一个 nid)
 *     link_path_walk()                 → 保留(路径走查)
 *     erofs_ilookup()                  → 保留(本插件 VFS 胶水的**主入口**)
 *
 * ============================== 有意偏离 ==============================
 *   1. **设备号解码裁掉**: 上游对 S_IFCHR/S_IFBLK 会把盘上的 32 位 `rdev` 解成
 *      Linux 的 `dev_t` 并放进 `vi->u.i_rdev`。本插件的 VFS 层只认
 *      DIR/FILE/SYMLINK(其余类型在目录面上"跳过并计数"), 没有任何读者消费 `i_rdev`,
 *      所以那两个 case 收成 `vi->u.i_rdev = 0`(字段保留: 未知类型仍需落到一个合法分支)。
 *   2. **错误指针 → 出参**: 上游 `find_target_dirent()` 用 `ERR_PTR()` 把"损坏"与
 *      "未命中"分开(`IS_ERR`/`PTR_ERR`)。本件不用 errno 指针(项目纪律), 改成
 *      `int *err_out` 出参 + `bool` 返回值。★ 这一点很要紧: "名字不存在"是 `false` +
 *      `*err_out == 0`, 而 `erofs_ilookup()` 要把它变成 `-ENOENT`(不是 -EIO)。
 *   3. **栈上的 4 KiB 缓冲 → 共享静态 scratch**: 上游 `erofs_namei()` 里是
 *      `char buf[EROFS_MAX_BLOCK_SIZE]`。内核对 APP 线程的栈是 4 KiB 级, 那样写会
 *      直接压穿(见 erofs_internal.h 的"栈纪律")。`erofs_read_inode_from_disk()` 的
 *      路径上加了一段"scratch 用过就重读 inode" —— 上游没有这个问题, 因为它的
 *      `buf` 在**自己的**栈帧里。这段工作区注释里写清了不变量。
 *   4. `erofs_err(... nid | 0ULL)` 里的 `| 0ULL`: 上游用它把 nid 提升成 64 位好配
 *      `%llu`。本件的 `PRIu64` 宏自带 `unsigned long long` 强转, 于是去掉那些 `| 0ULL`
 *      噪声(语义不变)。
 *   5. `strnlen()` 是本件 `erofs_compat.h` 里的 `uboot_erofs_strnlen`(无 libc)。
 * ==========================================================================
 */
#include "erofs_internal.h"

int erofs_read_inode_from_disk(struct erofs_inode *vi)
{
    int ret, ifmt;
    u8 *buf = uboot_erofs_scratch();
    struct erofs_inode_compact *dic;
    struct erofs_inode_extended *die;
    const erofs_off_t inode_loc = iloc(vi->nid);

    ret = erofs_dev_read(0, buf, inode_loc, sizeof(*dic));
    if (ret < 0) {
        return BR_ERR(BR_EIO);
    }

    dic = (struct erofs_inode_compact *)(void *)buf;
    ifmt = le16_to_cpu(dic->i_format);

    vi->datalayout = (unsigned char)erofs_inode_datalayout((unsigned int)ifmt);
    if (vi->datalayout >= EROFS_INODE_DATALAYOUT_MAX) {
        erofs_err("unsupported datalayout %u of nid %llu",
                  (unsigned)vi->datalayout, (unsigned long long)vi->nid);
        return BR_ERR(BR_ENOTSUP);
    }
    switch (erofs_inode_version((unsigned int)ifmt)) {
    case EROFS_INODE_LAYOUT_EXTENDED:
        vi->inode_isize = sizeof(struct erofs_inode_extended);

        ret = erofs_dev_read(0, buf + sizeof(*dic),
                             inode_loc + sizeof(*dic),
                             sizeof(*die) - sizeof(*dic));
        if (ret < 0) {
            return BR_ERR(BR_EIO);
        }

        die = (struct erofs_inode_extended *)(void *)buf;
        vi->xattr_isize = erofs_xattr_ibody_size(die->i_xattr_icount);
        vi->i_mode = le16_to_cpu(die->i_mode);

        switch (vi->i_mode & (umode_t)S_IFMT) {
        case S_IFREG:
        case S_IFDIR:
        case S_IFLNK:
            vi->u.i_blkaddr = le32_to_cpu(die->i_u.raw_blkaddr);
            break;
        case S_IFCHR:
        case S_IFBLK:
            /* 偏离 1: 设备号解码裁掉 */
            vi->u.i_rdev = 0;
            break;
        case S_IFIFO:
        case S_IFSOCK:
            vi->u.i_rdev = 0;
            break;
        default:
            goto bogusimode;
        }

        vi->i_uid   = le32_to_cpu(die->i_uid);
        vi->i_gid   = le32_to_cpu(die->i_gid);
        vi->i_nlink = le32_to_cpu(die->i_nlink);

        vi->i_mtime      = le64_to_cpu(die->i_mtime);
        vi->i_mtime_nsec = le32_to_cpu(die->i_mtime_nsec);
        vi->i_size       = le64_to_cpu(die->i_size);
        if (vi->datalayout == EROFS_INODE_CHUNK_BASED) {
            /* fill chunked inode summary info */
            vi->u.chunkformat = le16_to_cpu(die->i_u.c.format);
        }
        break;
    case EROFS_INODE_LAYOUT_COMPACT:
        vi->inode_isize = sizeof(struct erofs_inode_compact);
        vi->xattr_isize = erofs_xattr_ibody_size(dic->i_xattr_icount);
        vi->i_mode = le16_to_cpu(dic->i_mode);

        switch (vi->i_mode & (umode_t)S_IFMT) {
        case S_IFREG:
        case S_IFDIR:
        case S_IFLNK:
            vi->u.i_blkaddr = le32_to_cpu(dic->i_u.raw_blkaddr);
            break;
        case S_IFCHR:
        case S_IFBLK:
            vi->u.i_rdev = 0;       /* 偏离 1 */
            break;
        case S_IFIFO:
        case S_IFSOCK:
            vi->u.i_rdev = 0;
            break;
        default:
            goto bogusimode;
        }

        vi->i_uid   = le16_to_cpu(dic->i_uid);
        vi->i_gid   = le16_to_cpu(dic->i_gid);
        vi->i_nlink = le16_to_cpu(dic->i_nlink);

        vi->i_mtime      = uboot_erofs_sbi.build_time;
        vi->i_mtime_nsec = uboot_erofs_sbi.build_time_nsec;

        vi->i_size = le32_to_cpu(dic->i_size);
        if (vi->datalayout == EROFS_INODE_CHUNK_BASED) {
            vi->u.chunkformat = le16_to_cpu(dic->i_u.c.format);
        }
        break;
    default:
        erofs_err("unsupported on-disk inode version %u of nid %llu",
                  erofs_inode_version((unsigned int)ifmt), (unsigned long long)vi->nid);
        return BR_ERR(BR_ENOTSUP);
    }

    /*
     * ★ 不变量(本件特有): 到这里为止, 若盘上是 EXTENDED 布局, scratch 里躺着
     *   完整 64 字节 inode; COMPACT 布局则只有前 32 字节有效。上面两个分支**都不再
     *   读盘**, 所以调用方在返回后可以放心接着用 scratch 的内容 —— 唯一的例外正是
     *   接下来这段: 压缩 inode 的 `z_erofs_fill_inode()` 不读盘(只写字段), 但
     *   `z_erofs_map_blocks_iter()` 一旦调用就会**用 scratch 读映射索引**。因此本函数
     *   **不**在这里回读 inode: 压缩 inode 的映射头/索引解析发生在 zmap 里, 那里
     *   每次都从盘上重新读它需要的东西(见 erofs_zmap.c 的 z_erofs_fill_inode_lazy)。
     */
    vi->flags = 0;
    if (vi->datalayout == EROFS_INODE_CHUNK_BASED) {
        if (vi->u.chunkformat & ~(unsigned)EROFS_CHUNK_FORMAT_ALL) {
            erofs_err("unsupported chunk format %x of nid %llu",
                      (unsigned)vi->u.chunkformat, (unsigned long long)vi->nid);
            return BR_ERR(BR_ENOTSUP);
        }
        vi->u.chunkbits = (unsigned char)(uboot_erofs_sbi.blkszbits +
                          (vi->u.chunkformat & EROFS_CHUNK_FORMAT_BLKBITS_MASK));
    } else if (erofs_inode_is_data_compressed(vi->datalayout)) {
        /* 上游: `if (erofs_blksiz() != EROFS_MAX_BLOCK_SIZE) return -EOPNOTSUPP;`
         * 本件同样只支持 4 KiB 块的压缩(映射迭代按整块索引算, 见 zmap.c)。 */
        if (erofs_blksiz() != EROFS_MAX_BLOCK_SIZE) {
            erofs_err("compressed inode 需要 %u 字节块(实得 %u)",
                      (unsigned)EROFS_MAX_BLOCK_SIZE, (unsigned)erofs_blksiz());
            return BR_ERR(BR_ENOTSUP);
        }
        return z_erofs_fill_inode(vi);
    }
    return 0;
bogusimode:
    erofs_err("bogus i_mode (%o) @ nid %llu", (unsigned)vi->i_mode,
              (unsigned long long)vi->nid);
    return BR_ERR(EFSCORRUPTED);   /* 与上游逐字一致(EUCLEAN = 117, 见 erofs_compat.h) */
}

/*
 * 在一个目录块里找名字。
 * 偏离 2: 上游返回 `struct erofs_dirent *`, 用 `ERR_PTR(-EFSCORRUPTED)` 表达"块损坏"。
 * 本件用 `*err_out` 出参: 0 = 没找到(合法), 非 0 = 损坏/失败。
 */
static bool find_target_dirent(erofs_nid_t pnid,
                               void *dentry_blk,
                               const char *name, unsigned int len,
                               unsigned int nameoff,
                               unsigned int maxsize,
                               struct erofs_dirent **de_out,
                               int *err_out)
{
    struct erofs_dirent *de = (struct erofs_dirent *)dentry_blk;
    const struct erofs_dirent *end = (const struct erofs_dirent *)
        ((u8 *)dentry_blk + nameoff);

    *de_out  = BR_NULL;
    *err_out = 0;

    while (de < end) {
        const char *de_name;
        unsigned int de_namelen;

        nameoff = le16_to_cpu(de->nameoff);
        de_name = (char *)dentry_blk + nameoff;

        /* the last dirent in the block? */
        if (de + 1 >= end) {
            de_namelen = (unsigned int)strnlen(de_name, maxsize - nameoff);
        } else {
            de_namelen = (unsigned int)le16_to_cpu(de[1].nameoff) - nameoff;
        }

        /* a corrupted entry is found */
        if (nameoff + de_namelen > maxsize ||
            de_namelen > EROFS_NAME_LEN) {
            erofs_err("bogus dirent @ nid %llu", (unsigned long long)pnid);
            DBG_BUGON(1);
            *err_out = BR_ERR(EFSCORRUPTED);
            return false;
        }

        if (len == de_namelen && !memcmp(de_name, name, de_namelen)) {
            *de_out = de;
            return true;
        }
        ++de;
    }
    return false;
}

struct nameidata {
    erofs_nid_t  nid;
    unsigned int ftype;
};

/* ★ 偏离(可见性): 上游的 `erofs_namei()` 是全局符号(u-boot 的 fs 层直接调它)。
 * 本件只有 `link_path_walk()` 一个调用者 ⇒ 收成 static —— 本插件不导出任何符号,
 * 镜像的符号空间里只该有 `uboot_erofs_*` 那几个钩子。 */
static int erofs_namei(struct nameidata *nd, const char *name, unsigned int len)
{
    erofs_nid_t nid = nd->nid;
    int ret;
    struct erofs_inode vi = { .nid = nid };
    erofs_off_t offset;

    ret = erofs_read_inode_from_disk(&vi);
    if (ret != 0) {
        return ret;
    }
    if (!S_ISDIR(vi.i_mode)) {
        /* 上游没有这一条(它只在 link_path_walk 里逐级走, 中间级必然是目录)。
         * 本件保留它: 自检/直接调用者可能把一个文件当目录走查, 那时 -ENOTDIR 比
         * 一块垃圾 dirent 解析要诚实得多。 */
        return BR_ERR(BR_ENOTDIR);
    }

    offset = 0;
    while (offset < vi.i_size) {
        /* 每一块都要重新取 scratch —— `erofs_read_inode_from_disk()` 也用同一份。 */
        u8 *buf = uboot_erofs_scratch();
        const erofs_off_t maxsize = min_t(erofs_off_t,
                                          vi.i_size - offset, (erofs_off_t)erofs_blksiz());
        struct erofs_dirent *de;
        unsigned int nameoff;
        struct erofs_dirent *found = BR_NULL;
        int ferr = 0;

        ret = erofs_pread(&vi, (char *)buf, maxsize, offset);
        if (ret != 0) {
            return (ret < 0) ? ret : BR_ERR(BR_EIO);
        }

        de = (struct erofs_dirent *)(void *)buf;
        nameoff = (unsigned int)le16_to_cpu(de->nameoff);
        if (nameoff < sizeof(struct erofs_dirent) ||
            nameoff >= erofs_blksiz()) {
            erofs_err("invalid de[0].nameoff %u @ nid %llu",
                      nameoff, (unsigned long long)nid);
            return BR_ERR(EFSCORRUPTED);
        }

        (void)find_target_dirent(nid, buf, name, len, nameoff,
                                 (unsigned int)maxsize, &found, &ferr);
        if (ferr != 0) {
            return ferr;
        }
        if (found != BR_NULL) {
            nd->nid = le64_to_cpu(found->nid);
            return 0;
        }
        offset += maxsize;
    }
    return BR_ERR(BR_ENOENT);
}

static int link_path_walk(const char *name, struct nameidata *nd)
{
    nd->nid = uboot_erofs_sbi.root_nid;

    while (*name == '/') {
        name++;
    }

    /* At this point we know we have a real path component. */
    while (*name != '\0') {
        const char *p = name;
        int ret;

        do {
            ++p;
        } while (*p != '\0' && *p != '/');

        DBG_BUGON(p <= name);
        ret = erofs_namei(nd, name, (unsigned int)(p - name));
        if (ret != 0) {
            return ret;
        }

        name = p;
        /* Skip until no more slashes. */
        for (name = p; *name == '/'; ++name) {
        }
    }
    return 0;
}

int erofs_ilookup(const char *path, struct erofs_inode *vi)
{
    int ret;
    struct nameidata nd;

    ret = link_path_walk(path, &nd);
    if (ret != 0) {
        return ret;
    }

    vi->nid = nd.nid;
    return erofs_read_inode_from_disk(vi);
}
