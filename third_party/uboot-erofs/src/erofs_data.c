/*
 * third_party/uboot-erofs — 读路径与平面映射(移植自 u-boot `fs/erofs/data.c`, GPL-2.0+)
 *
 * ============================== 上游对照 ==============================
 *   上游文件 : fs/erofs/data.c(359 行)
 *   移植范围 : 全部读路径 —— 平面映射(FLAT_PLAIN / FLAT_INLINE)、chunk-based 映射、
 *              设备映射、未压缩读、压缩读、`erofs_pread()` 入口。
 *   逐函数对照:
 *     erofs_map_blocks_flatmode() → 保留(未压缩 flat: 主体 + inline 尾)
 *     erofs_map_blocks()          → 保留(含 chunk-based 的 block map / chunk index)
 *     erofs_map_dev()             → 保留(单卷下是恒等; 见"偏离 2")
 *     erofs_read_one_data()       → 保留
 *     erofs_read_raw_data()       → 保留
 *     z_erofs_read_one_data()     → 保留
 *     z_erofs_read_data()         → 保留(压缩路径的**多 extent 切分**在这里)
 *     erofs_pread()               → 保留(按 datalayout 分派)
 *   `erofs_getxattr()`/`erofs_listxattr()` 只在上游 internal.h 里**声明**过(定义在
 *   xattr.c, u-boot 根本不编译它) ⇒ 本件不移植, 也不留声明。
 *
 * ============================== 有意偏离 ==============================
 *   1. **栈上 4 KiB 缓冲 → 文件域静态**(`s_map`, 内含 4 KiB `mpage`)。上游在
 *      `erofs_read_raw_data()`/`z_erofs_read_data()` 里都是
 *      `struct erofs_map_blocks map = {...}` —— 那个结构体内嵌 4 KiB `mpage`,
 *      放栈上就等于把 4 KiB 栈全用掉(见 erofs_internal.h 的"栈纪律")。
 *      两个函数用的是**同一张**静态工作台: 它们从 `erofs_pread()` 分派, 互不递归调用。
 *   2. **`erofs_map_dev()` 的 fragment 分支**: 上游的 `sbi.devs` 多设备表已裁掉,
 *      于是"按设备号找映射窗口"的循环不存在 ⇒ 单卷下 `m_deviceid` 必须为 0,
 *      否则 `-ENODEV`(盘上说这块数据在别的设备上, 而本件只有一个卷)。
 *   3. **读缓冲的越界执法**: 上游信任盘上的块地址(`erofs_dev_read()` 越界会由
 *      u-boot 的 blk 层拒绝)。本件在 `erofs_read_one_data()` 里**显式**再查一遍
 *      (见 uboot_erofs_range_ok), 因为坏 nid 读出的垃圾如果流进 inode 解析, 症状会
 *      远离根因。这一条与 erofs_vfs.c 的块层 shim(uboot_erofs_dev_read)的越界检查是同一条防线的两个断面。
 *   4. **`raw` 缓冲用 core 堆**: 上游 `realloc(raw, bufsize)` 在压缩路径上按 extent 的
 *      **物理长度**增长。本件的 `br_realloc` 是同一语义(ADR-0014)。最大 32 KiB。
 *   5. 错误码: 裸名字(`EIO`/`ENOTSUP`/`EFSCORRUPTED`/…)由 core 的
 *      `<br/core/br_errno.h>` 提供(ADR-0023)⇒ 上游的 `-EFSCORRUPTED` 原样成立
 *      (`EUCLEAN` = 117, 见 `erofs_compat.h`)。
 *   6. **chunk-based 的空洞**: 上游对"未映射"的 extent 填 0。本件照做(见
 *      `erofs_read_raw_data()` 的两个 memset 分支)。
 * ==========================================================================
 */
#include "erofs_internal.h"

/*
 * ★ 偏离 1: 平面/压缩两个读路径共用的映射工作台(文件域静态, 不占栈)。
 * `index` 初值 = UINT_MAX: `z_erofs_reload_indexes()` 靠它判断"索引块还在不在缓冲里"
 * (见 erofs_zmap.c —— 本件把那个缓存标记移到了**录制器**上, 这里的值只是初值)。
 */
static struct erofs_map_blocks s_map;

/*
 * 偏离 3: 每次真正下发块读之前, 确认 (offset, len) 落在介质内。
 * 为什么不只靠 bdev 的越界检查: 那一条只在"跨出介质"时报错, 报出来的是 -EIO 而没有
 * "哪个 inode/哪个 extent"的上下文; 这里补一层, 日志里能看到出错的字节区间。
 */
static bool uboot_erofs_range_ok(u64 offset, size_t len)
{
    /* EROFS 的块号是 u32, 块大小 ≤ 4 KiB ⇒ 偏移上界远小于 2^63; 用无符号加法不会绕回。
     * 但 `total_blocks` 来自盘上, 所以仍按加法查一遍。 */
    const u64 fs_bytes = uboot_erofs_sbi.total_blocks * (u64)erofs_blksiz();

    if (offset > fs_bytes) {
        return false;
    }
    if (len > (fs_bytes - offset)) {
        return false;
    }
    return true;
}

static int erofs_map_blocks_flatmode(struct erofs_inode *inode,
                                     struct erofs_map_blocks *map,
                                     int flags)
{
    int err = 0;
    erofs_blk_t nblocks, lastblk;
    u64 offset = map->m_la;
    struct erofs_inode *vi = inode;
    bool tailendpacking = (vi->datalayout == EROFS_INODE_FLAT_INLINE);

    (void)flags;    /* 上游同样不读它(签名先行) */

    nblocks = (erofs_blk_t)BLK_ROUND_UP(inode->i_size);
    lastblk = nblocks - (erofs_blk_t)(tailendpacking ? 1u : 0u);

    /* there is no hole in flatmode */
    map->m_flags = EROFS_MAP_MAPPED;

    if (offset < erofs_pos(lastblk)) {
        map->m_pa = erofs_pos(vi->u.i_blkaddr) + map->m_la;
        map->m_plen = erofs_pos(lastblk) - offset;
    } else if (tailendpacking) {
        /* 2 - inode inline B: inode, [xattrs], inline last blk... */
        map->m_pa = iloc(vi->nid) + vi->inode_isize +
            vi->xattr_isize + erofs_blkoff(map->m_la);
        map->m_plen = inode->i_size - offset;

        /* inline data should be located in the same meta block */
        if (erofs_blkoff(map->m_pa) + map->m_plen > erofs_blksiz()) {
            erofs_err("inline data cross block boundary @ nid %llu",
                      (unsigned long long)vi->nid);
            DBG_BUGON(1);
            err = BR_ERR(EFSCORRUPTED);
            goto err_out;
        }

        map->m_flags |= EROFS_MAP_META;
    } else {
        erofs_err("internal error @ nid: %llu (size %llu), m_la 0x%llx",
                  (unsigned long long)vi->nid, (unsigned long long)inode->i_size,
                  (unsigned long long)map->m_la);
        DBG_BUGON(1);
        err = BR_ERR(BR_EIO);
        goto err_out;
    }

    map->m_llen = map->m_plen;
err_out:
    return err;
}

int erofs_map_blocks(struct erofs_inode *inode,
                     struct erofs_map_blocks *map, int flags)
{
    struct erofs_inode *vi = inode;
    struct erofs_inode_chunk_index *idx;
    u64 chunknr;
    unsigned int unit;
    erofs_off_t pos;
    int err = 0;

    map->m_deviceid = 0;
    if (map->m_la >= inode->i_size) {
        /* leave out-of-bound access unmapped */
        map->m_flags = 0;
        map->m_plen = 0;
        goto out;
    }

    if (vi->datalayout != EROFS_INODE_CHUNK_BASED) {
        return erofs_map_blocks_flatmode(inode, map, flags);
    }

    /* 偏离 1: chunk index 的那一块读进共享 scratch(上游是栈上的 u8 buf[4096])。
     * 一次性用完即弃 —— 紧随其后的解析只碰 buf 的前几个字节。 */
    u8 *buf = uboot_erofs_scratch();

    if (vi->u.chunkformat & EROFS_CHUNK_FORMAT_INDEXES) {
        unit = sizeof(*idx);                    /* chunk index */
    } else {
        unit = EROFS_BLOCK_MAP_ENTRY_SIZE;      /* block map */
    }

    chunknr = map->m_la >> vi->u.chunkbits;
    pos = round_up((u64)iloc(vi->nid) + vi->inode_isize + vi->xattr_isize, unit) +
        (erofs_off_t)(unit * chunknr);

    err = erofs_blk_read(buf, erofs_blknr(pos), 1);
    if (err < 0) {
        return BR_ERR(BR_EIO);
    }

    map->m_la = chunknr << vi->u.chunkbits;
    map->m_plen = min_t(erofs_off_t, (erofs_off_t)1 << vi->u.chunkbits,
                        (erofs_off_t)round_up(inode->i_size - map->m_la, erofs_blksiz()));

    /* handle block map */
    if (!(vi->u.chunkformat & EROFS_CHUNK_FORMAT_INDEXES)) {
        __le32 *blkaddr = (__le32 *)(void *)(buf + erofs_blkoff(pos));

        if (le32_to_cpu(*blkaddr) == (u32)EROFS_NULL_ADDR) {
            map->m_flags = 0;
        } else {
            map->m_pa = erofs_pos(le32_to_cpu(*blkaddr));
            map->m_flags = EROFS_MAP_MAPPED;
        }
        goto out;
    }
    /* parse chunk indexes */
    idx = (struct erofs_inode_chunk_index *)(void *)(buf + erofs_blkoff(pos));
    switch (le32_to_cpu(idx->blkaddr)) {
    case (u32)EROFS_NULL_ADDR:
        map->m_flags = 0;
        break;
    default:
        /* 偏离 2: 单卷 ⇒ device_id 只能是 0(上游这里要 & sbi.device_id_mask)。 */
        map->m_deviceid = (unsigned short)le16_to_cpu(idx->device_id);
        map->m_pa = erofs_pos(le32_to_cpu(idx->blkaddr));
        map->m_flags = EROFS_MAP_MAPPED;
        break;
    }
out:
    map->m_llen = map->m_plen;
    return err;
}

int erofs_map_dev(struct erofs_map_dev *map)
{
    /* 偏离 2: 上游在多设备表里找"这块 pa 落在哪台设备", 并把 pa 折算成那台设备的
     * 局部偏移。本件只有一个卷, 于是: 设备号非 0 就是坏镜像。 */
    if (map->m_deviceid != 0u) {
        erofs_err("multi-device map (device_id=%u) 不被支持", (unsigned)map->m_deviceid);
        return BR_ERR(BR_ENODEV);
    }
    return 0;
}

int erofs_read_one_data(struct erofs_map_blocks *map, char *buffer, u64 offset,
                        size_t len)
{
    struct erofs_map_dev mdev;
    int ret;

    mdev = (struct erofs_map_dev) {
        .m_deviceid = map->m_deviceid,
        .m_pa = map->m_pa,
    };
    ret = erofs_map_dev(&mdev);
    if (ret != 0) {
        return ret;
    }

    if (!uboot_erofs_range_ok(mdev.m_pa + offset, len)) {
        erofs_err("read out of media: pa=0x%llx off=%llu len=%llu",
                  (unsigned long long)mdev.m_pa, (unsigned long long)offset,
                  (unsigned long long)len);
        return BR_ERR(BR_EIO);
    }

    ret = erofs_dev_read((int)mdev.m_deviceid, buffer, mdev.m_pa + offset, len);
    if (ret < 0) {
        return BR_ERR(BR_EIO);
    }
    return 0;
}

static int erofs_read_raw_data(struct erofs_inode *inode, char *buffer,
                               erofs_off_t size, erofs_off_t offset)
{
    /* 偏离 1: 用共享静态工作台(上游是栈上的 struct erofs_map_blocks)。 */
    struct erofs_map_blocks *map = &s_map;
    int ret;
    erofs_off_t ptr = offset;

    map->index = (erofs_blk_t)-1;
    while (ptr < offset + size) {
        char *const estart = buffer + ptr - offset;
        erofs_off_t eend, moff = 0;

        map->m_la = ptr;
        ret = erofs_map_blocks(inode, map, 0);
        if (ret != 0) {
            return ret;
        }

        DBG_BUGON(map->m_plen != map->m_llen);

        /* trim extent */
        eend = min(offset + size, map->m_la + map->m_llen);
        DBG_BUGON(ptr < map->m_la);

        if (!(map->m_flags & EROFS_MAP_MAPPED)) {
            if (!map->m_llen) {
                /* reached EOF */
                memset(estart, 0, (br_size_t)(offset + size - ptr));
                ptr = offset + size;
                continue;
            }
            memset(estart, 0, (br_size_t)(eend - ptr));
            ptr = eend;
            continue;
        }

        if (ptr > map->m_la) {
            moff = ptr - map->m_la;
            map->m_la = ptr;
        }

        ret = erofs_read_one_data(map, estart, (u64)moff, (size_t)(eend - map->m_la));
        if (ret != 0) {
            return ret;
        }
        ptr = eend;
    }
    return 0;
}

int z_erofs_read_one_data(struct erofs_inode *inode,
                          struct erofs_map_blocks *map, char *raw, char *buffer,
                          erofs_off_t skip, erofs_off_t length, bool trimmed)
{
    struct erofs_map_dev mdev;
    int ret = 0;

    if (map->m_flags & EROFS_MAP_FRAGMENT) {
        struct erofs_inode packed_inode = { .nid = uboot_erofs_sbi.packed_nid };

        ret = erofs_read_inode_from_disk(&packed_inode);
        if (ret != 0) {
            erofs_err("failed to read packed inode from disk: %d", ret);
            return ret;
        }

        return erofs_pread(&packed_inode, buffer, length - skip,
                           inode->fragmentoff + skip);
    }

    /* no device id here, thus it will always succeed */
    mdev = (struct erofs_map_dev) {
        .m_pa = map->m_pa,
    };
    ret = erofs_map_dev(&mdev);
    if (ret != 0) {
        DBG_BUGON(1);
        return ret;
    }

    if (!uboot_erofs_range_ok(mdev.m_pa, (size_t)map->m_plen)) {
        erofs_err("compressed extent out of media: pa=0x%llx plen=%llu",
                  (unsigned long long)mdev.m_pa, (unsigned long long)map->m_plen);
        return BR_ERR(BR_EIO);
    }

    ret = erofs_dev_read((int)mdev.m_deviceid, raw, mdev.m_pa, (size_t)map->m_plen);
    if (ret < 0) {
        return ret;
    }

    ret = z_erofs_decompress(&(struct z_erofs_decompress_req) {
            .in = raw,
            .out = buffer,
            .decodedskip = (unsigned int)skip,
            .interlaced_offset =
                (map->m_algorithmformat == Z_EROFS_COMPRESSION_INTERLACED) ?
                    (unsigned int)erofs_blkoff(map->m_la) : 0u,
            .inputsize = (unsigned int)map->m_plen,
            .decodedlength = (unsigned int)length,
            .alg = (unsigned int)map->m_algorithmformat,
            .partial_decoding = (trimmed == true) ? true :
                (!(map->m_flags & EROFS_MAP_FULL_MAPPED) ||
                 (map->m_flags & EROFS_MAP_PARTIAL_REF)),
             });
    if (ret < 0) {
        return ret;
    }
    return 0;
}

static int z_erofs_read_data(struct erofs_inode *inode, char *buffer,
                             erofs_off_t size, erofs_off_t offset)
{
    erofs_off_t end, length, skip;
    /* 偏离 1: 共享静态工作台 + 堆上的 raw(上游分别是栈上的 map 与 realloc 的 raw)。 */
    struct erofs_map_blocks *map = &s_map;
    bool trimmed;
    unsigned int bufsize = 0;
    char *raw = BR_NULL;
    int ret = 0;

    map->index = (erofs_blk_t)-1;
    end = offset + size;
    while (end > offset) {
        map->m_la = end - 1;

        ret = z_erofs_map_blocks_iter(inode, map, 0);
        if (ret != 0) {
            break;
        }

        /*
         * trim to the needed size if the returned extent is quite
         * larger than requested, and set up partial flag as well.
         */
        if (end < map->m_la + map->m_llen) {
            length = end - map->m_la;
            trimmed = true;
        } else {
            DBG_BUGON(end != map->m_la + map->m_llen);
            length = map->m_llen;
            trimmed = false;
        }

        if (map->m_la < offset) {
            skip = offset - map->m_la;
            end = offset;
        } else {
            skip = 0;
            end = map->m_la;
        }

        if (!(map->m_flags & EROFS_MAP_MAPPED)) {
            memset(buffer + end - offset, 0, (br_size_t)(length - skip));
            end = map->m_la;
            continue;
        }

        if (map->m_plen > bufsize) {
            char *tmp;

            bufsize = (unsigned int)map->m_plen;
            tmp = (char *)br_realloc(raw, (br_size_t)bufsize);
            if (tmp == BR_NULL) {
                ret = BR_ERR(BR_ENOMEM);
                break;
            }
            raw = tmp;
        }

        ret = z_erofs_read_one_data(inode, map, raw,
                                    buffer + end - offset, skip, length,
                                    trimmed);
        if (ret < 0) {
            break;
        }
    }
    if (raw != BR_NULL) {
        br_free(raw);
    }
    return (ret < 0) ? ret : 0;
}

int erofs_pread(struct erofs_inode *inode, char *buf,
                erofs_off_t count, erofs_off_t offset)
{
    /* 零长读: 直接成功。上游也走到 read_raw_data 才由 while 条件收敛; 本件在入口挡下
     * 是因为 `erofs_pread(vi, NULL, 0, off)` 在自检里会出现(空文件/EOF 边界)。 */
    if (count == 0u) {
        return 0;
    }
    if (buf == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }

    switch (inode->datalayout) {
    case EROFS_INODE_FLAT_PLAIN:
    case EROFS_INODE_FLAT_INLINE:
    case EROFS_INODE_CHUNK_BASED:
        return erofs_read_raw_data(inode, buf, count, offset);
    case EROFS_INODE_COMPRESSED_FULL:
    case EROFS_INODE_COMPRESSED_COMPACT:
        return z_erofs_read_data(inode, buf, count, offset);
    default:
        break;
    }
    return BR_ERR(BR_EINVAL);
}
