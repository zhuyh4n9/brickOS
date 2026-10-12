// SPDX-License-Identifier: GPL-2.0+
/*
 * third_party/uboot-erofs — 压缩映射迭代(移植自 u-boot `fs/erofs/zmap.c`, GPL-2.0+)
 *
 * ============================== 上游对照 ==============================
 *   上游文件 : fs/erofs/zmap.c(698 行)
 *   移植范围 : **全部**压缩映射迭代 —— `z_erofs_fill_inode()` /
 *              `z_erofs_fill_inode_lazy()`(映射头)、`legacy_load_cluster_from_disk()`
 *              (非紧凑索引)、`unpack_compacted_index()`(紧凑 4B/2B 索引)、
 *              `z_erofs_extent_lookback()`、`z_erofs_get_extent_compressedlen()`、
 *              `z_erofs_get_extent_decompressedlen()`、`z_erofs_do_map_blocks()`、
 *              `z_erofs_map_blocks_iter()`(入口)。
 *   ★ 一条分支都没有再裁: BIG_PCLUSTER / COMPACTED_2B / INTERLACED / ZTAILPACKING /
 *     FRAGMENT 都在(见 README "裁剪口径" —— 上游把这些写成 `erofs_sb_has_*()` **运行期**
 *     查询, 本来就不是 `IS_ENABLED()` 的编译期开关, 所以它们原样可用)。
 *     本件只把**算法**收窄到 LZ4: 非 LZ4 的 `z_algorithmtype` 在 decompress 处报
 *     `-ENOTSUP`(见 erofs_decompress.c), 映射迭代本身对算法无感。
 *
 * ============================== 有意偏离 ==============================
 *   1. **索引块的缓存判据从 `map->index` 改成 `m->index`**(本件最重要的一处改动)。
 *      上游 `z_erofs_reload_indexes()` 用 `map->index == eblk` 判断"这块索引还在不在
 *      `map->mpage` 里"。上游能把 `mpage` 放在栈上(每次 `erofs_read_raw_data()` 重新
 *      构造一个 map), 所以"缓存"天然跟着一次读走。**本件把 `mpage` 提成了文件域静态**
 *      (栈只有 4 KiB 级, 见 erofs_internal.h 的"栈纪律"), 于是 `map->index` 会在两次
 *      映射之间残留 —— 若照抄上游判据, 会把"上一次映射留下的索引块"当成"这一次已经
 *      加载好的"。
 *      做法: 缓存标记挂在**录制器**上(`struct z_erofs_maprecorder` 新增 `index` 字段),
 *      并且用一个文件域标志 `s_z_scratch_valid` 记录"共享 scratch 里的内容是否还是映射
 *      自己写下的":
 *        - `z_erofs_map_blocks_iter()` 进入时**无条件**置为"失效"(从头开始, 谁也不信);
 *        - 每次映射自己读进 scratch 之前/之后把标志置为"有效";
 *        - 任何**别的**消费者碰共享 scratch(本文件里是 `z_erofs_fill_inode_lazy()` 的
 *          映射头读与 `z_erofs_get_extent_decompressedlen()` 的 lookahead 读)都把它
 *          置回"失效";
 *        - 录制器下一次要读索引时, 若标志失效则丢弃缓存、真的重新读。
 *      结论: 缓存**只**在"一段连续的映射计算内"生效, 跨调用一定重读 —— 正确性优先于
 *      省下的那几次块读(上游的缓存本来也只是同一段计算内的复读)。
 *   2. **栈上的 `char buf[...]` → 共享 scratch**: `z_erofs_fill_inode_lazy()` 里上游是
 *      `char buf[sizeof(struct z_erofs_map_header)]`(8 B) —— 这个其实放栈上完全没问题;
 *      本件统一走 scratch 只是为了"读盘只有一条路径", 代价见偏离 1。
 *      另有 `struct erofs_map_blocks map = { .index = UINT_MAX }` 两处(内嵌 4 KiB):
 *      本件**保留**该局部结构(它的 `mpage` 只是没人用的空地 —— 索引块读的是共享
 *      scratch), 因为 `z_erofs_do_map_blocks()` 需要它自己的 `m_la/m_llen/m_flags` 工作区。
 *   3. **`DBG_BUGON(err < 0 && err != -ENOMEM)`**: 上游在
 *      `z_erofs_map_blocks_iter()` 出口用它把"读路径不该出现的错误"打成停机。本件把它
 *      换成**运行期日志**(见 erofs_compat.h 的 DBG_BUGON), 并在同一出口回传原错误码。
 *   4. 错误码: 裸名字由 core 的 `<br/core/br_errno.h>` 提供(ADR-0023), 于是**上游的
 *      `-EFSCORRUPTED` 一处都没改**(`EUCLEAN` = 117; 见 erofs_compat.h)。
 *   5. `erofs_dbg()` 的格式串里那些 `| 0ULL` / `PRIu64` 噪声: 本件的 `PRIu64` 宏自带
 *      `unsigned long long` 强转 ⇒ 去掉 `| 0ULL`(语义不变)。
 * ==========================================================================
 */
#include "erofs_internal.h"

static int z_erofs_do_map_blocks(struct erofs_inode *vi,
                                 struct erofs_map_blocks *map,
                                 int flags);

/*
 * ★ 偏离 1: 共享 scratch 的内容是否还是"映射自己写下的索引块"。
 * 含义与维护点见文件头。它不是"scratch 里有没有东西", 而是"能不能当索引缓存用"。
 */
static bool s_z_scratch_valid;

int z_erofs_fill_inode(struct erofs_inode *vi)
{
    if (!erofs_sb_has_big_pcluster() &&
        !erofs_sb_has_ztailpacking() && !erofs_sb_has_fragments() &&
        vi->datalayout == EROFS_INODE_COMPRESSED_FULL) {
        vi->z_advise = 0;
        vi->z_algorithmtype[0] = 0;
        vi->z_algorithmtype[1] = 0;
        vi->z_logical_clusterbits = uboot_erofs_sbi.blkszbits;

        vi->flags |= EROFS_I_Z_INITED;
    }
    return 0;
}

static int z_erofs_fill_inode_lazy(struct erofs_inode *vi)
{
    int ret;
    erofs_off_t pos;
    struct z_erofs_map_header *h;
    u8 *buf;

    if (vi->flags & EROFS_I_Z_INITED) {
        return 0;
    }

    pos = round_up(iloc(vi->nid) + vi->inode_isize + vi->xattr_isize, 8u);
    buf = uboot_erofs_scratch();
    ret = erofs_dev_read(0, buf, pos, sizeof(struct z_erofs_map_header));
    if (ret < 0) {
        return BR_ERR(BR_EIO);
    }

    h = (struct z_erofs_map_header *)(void *)buf;
    /*
     * if the highest bit of the 8-byte map header is set, the whole file
     * is stored in the packed inode. The rest bits keeps z_fragmentoff.
     */
    if (h->h_clusterbits >> Z_EROFS_FRAGMENT_INODE_BIT) {
        vi->z_advise = Z_EROFS_ADVISE_FRAGMENT_PCLUSTER;
        vi->fragmentoff = le64_to_cpu(*(__le64 *)(void *)h) ^ (1ULL << 63);
        vi->z_tailextent_headlcn = 0;
        goto out;
    }

    vi->z_advise = le16_to_cpu(h->h_advise);
    vi->z_algorithmtype[0] = h->h_algorithmtype & 15u;
    vi->z_algorithmtype[1] = h->h_algorithmtype >> 4;

    if (vi->z_algorithmtype[0] >= Z_EROFS_COMPRESSION_MAX) {
        erofs_err("unknown compression format %u for nid %llu",
                  (unsigned)vi->z_algorithmtype[0], (unsigned long long)vi->nid);
        return BR_ERR(BR_ENOTSUP);
    }

    vi->z_logical_clusterbits = (u8)(uboot_erofs_sbi.blkszbits + (h->h_clusterbits & 7u));
    if (vi->datalayout == EROFS_INODE_COMPRESSED_COMPACT &&
        !(vi->z_advise & Z_EROFS_ADVISE_BIG_PCLUSTER_1) ^
        !(vi->z_advise & Z_EROFS_ADVISE_BIG_PCLUSTER_2)) {
        erofs_err("big pcluster head1/2 of compact indexes should be consistent for nid %llu",
                  (unsigned long long)vi->nid);
        return BR_ERR(EFSCORRUPTED);
    }

    if (vi->z_advise & Z_EROFS_ADVISE_INLINE_PCLUSTER) {
        struct erofs_map_blocks map = { .index = (erofs_blk_t)-1 };

        vi->idata_size = le16_to_cpu(h->h_idata_size);
        ret = z_erofs_do_map_blocks(vi, &map, EROFS_GET_BLOCKS_FINDTAIL);
        if (!map.m_plen ||
            erofs_blkoff(map.m_pa) + map.m_plen > erofs_blksiz()) {
            erofs_err("invalid tail-packing pclustersize %llu",
                      (unsigned long long)map.m_plen);
            return BR_ERR(EFSCORRUPTED);
        }
        if (ret < 0) {
            return ret;
        }
    }
    if ((vi->z_advise & Z_EROFS_ADVISE_FRAGMENT_PCLUSTER) &&
        !(h->h_clusterbits >> Z_EROFS_FRAGMENT_INODE_BIT)) {
        struct erofs_map_blocks map = { .index = (erofs_blk_t)-1 };

        vi->fragmentoff = le32_to_cpu(h->h_fragmentoff);
        ret = z_erofs_do_map_blocks(vi, &map, EROFS_GET_BLOCKS_FINDTAIL);
        if (ret < 0) {
            return ret;
        }
    }
out:
    /* ★ 偏离 1: 映射头(以及上面两处 FINDTAIL 的索引读)都借用过共享 scratch ⇒
     *   通知后面的索引加载"缓存不可信"。 */
    s_z_scratch_valid = false;
    vi->flags |= EROFS_I_Z_INITED;
    return 0;
}

struct z_erofs_maprecorder {
    struct erofs_inode *inode;
    struct erofs_map_blocks *map;

    unsigned long lcn;
    /* compression extent information gathered */
    u8  type, headtype;
    u16 clusterofs;
    u16 delta[2];
    erofs_blk_t pblk, compressedblks;
    erofs_off_t nextpackoff;
    bool partialref;

    /* ★ 偏离 1: 索引块缓存标记(上游用的是 map->index —— 见文件头)。 */
    erofs_blk_t index;
};

static int z_erofs_reload_indexes(struct z_erofs_maprecorder *m,
                                  erofs_blk_t eblk)
{
    int ret;

    /* ★ 偏离 1: 两个条件缺一不可 —— "这块已经读过" **且** "scratch 里还是它"。 */
    if (s_z_scratch_valid && (m->index == eblk)) {
        return 0;
    }

    /* ★ 偏离 1 的**唯一读点**: 索引块必须读进 `uboot_erofs_scratch()` —— 两个解析器
     *   (`legacy_load_cluster_from_disk` / `compact_load_cluster_from_disk`)都从
     *   scratch 取字节。上游这里是 `map->mpage`, 本件把 mpage 让给了共享 scratch,
     *   所以读点与解析点必须同一个缓冲: 读进 `map->mpage` 而解析 scratch ⇒ 解析到
     *   的是上一块残留(压缩镜像走这条路)。
     *   (`struct erofs_map_blocks.mpage` 因此在本件里没有读者, 见 erofs_internal.h。) */
    ret = erofs_blk_read(uboot_erofs_scratch(), eblk, 1);
    if (ret < 0) {
        return BR_ERR(BR_EIO);
    }

    m->index = eblk;
    s_z_scratch_valid = true;
    return 0;
}

static int legacy_load_cluster_from_disk(struct z_erofs_maprecorder *m,
                                         unsigned long lcn)
{
    struct erofs_inode *const vi = m->inode;
    const erofs_off_t ibase = iloc(vi->nid);
    const erofs_off_t pos = Z_EROFS_FULL_INDEX_ALIGN(ibase +
            vi->inode_isize + vi->xattr_isize) +
        (erofs_off_t)lcn * sizeof(struct z_erofs_lcluster_index);
    struct z_erofs_lcluster_index *di;
    unsigned int advise, type;
    int err;

    err = z_erofs_reload_indexes(m, erofs_blknr(pos));
    if (err != 0) {
        return err;
    }

    m->nextpackoff = pos + sizeof(struct z_erofs_lcluster_index);
    m->lcn = lcn;
    /* ★ 偏离 1: 上游这里是 `m->kaddr + erofs_blkoff(pos)`, 而 `m->kaddr = map->mpage`
     *   (内嵌在栈上的 map 里)。本件的索引块读进**共享 scratch**(见
     *   erofs_internal.h 的"栈纪律"), 所以承载者换成 scratch —— 语义完全一样:
     *   `z_erofs_reload_indexes()` 刚把这一块读进 scratch。 */
    di = (struct z_erofs_lcluster_index *)(void *)
        (uboot_erofs_scratch() + erofs_blkoff(pos));

    advise = le16_to_cpu(di->di_advise);
    type = (advise >> Z_EROFS_LI_LCLUSTER_TYPE_BIT) &
        ((1u << Z_EROFS_LI_LCLUSTER_TYPE_BITS) - 1u);
    switch (type) {
    case Z_EROFS_LCLUSTER_TYPE_NONHEAD:
        m->clusterofs = (u16)(1u << vi->z_logical_clusterbits);
        m->delta[0] = le16_to_cpu(di->di_u.delta[0]);
        if (m->delta[0] & Z_EROFS_LI_D0_CBLKCNT) {
            if (!(vi->z_advise & Z_EROFS_ADVISE_BIG_PCLUSTER_1)) {
                DBG_BUGON(1);
                return BR_ERR(EFSCORRUPTED);
            }
            m->compressedblks = (u16)(m->delta[0] & ~(u16)Z_EROFS_LI_D0_CBLKCNT);
            m->delta[0] = 1;
        }
        m->delta[1] = le16_to_cpu(di->di_u.delta[1]);
        break;
    case Z_EROFS_LCLUSTER_TYPE_PLAIN:
    case Z_EROFS_LCLUSTER_TYPE_HEAD1:
        if (advise & Z_EROFS_LI_PARTIAL_REF) {
            m->partialref = true;
        }
        m->clusterofs = le16_to_cpu(di->di_clusterofs);
        m->pblk = le32_to_cpu(di->di_u.blkaddr);
        break;
    default:
        DBG_BUGON(1);
        return BR_ERR(BR_ENOTSUP);
    }
    m->type = (u8)type;
    return 0;
}

static unsigned int decode_compactedbits(unsigned int lobits,
                                         unsigned int lomask,
                                         const u8 *in, unsigned int pos, u8 *type)
{
    const unsigned int v = get_unaligned_le32(in + pos / 8u) >> (pos & 7u);
    const unsigned int lo = v & lomask;

    *type = (u8)((v >> lobits) & 3u);
    return lo;
}

static int get_compacted_la_distance(unsigned int lclusterbits,
                                     unsigned int encodebits,
                                     unsigned int vcnt, const u8 *in, int i)
{
    const unsigned int lomask = (1u << lclusterbits) - 1u;
    unsigned int lo, d1 = 0;
    u8 type;

    DBG_BUGON(i >= (int)vcnt);

    do {
        lo = decode_compactedbits(lclusterbits, lomask,
                                  in, encodebits * (unsigned int)i, &type);

        if (type != Z_EROFS_LCLUSTER_TYPE_NONHEAD) {
            return (int)d1;
        }
        ++d1;
    } while (++i < (int)vcnt);

    /* vcnt - 1 (Z_EROFS_LCLUSTER_TYPE_NONHEAD) item */
    if (!(lo & Z_EROFS_LI_D0_CBLKCNT)) {
        d1 += lo - 1u;
    }
    return (int)d1;
}

static int unpack_compacted_index(struct z_erofs_maprecorder *m,
                                  unsigned int amortizedshift,
                                  erofs_off_t pos, bool lookahead)
{
    struct erofs_inode *const vi = m->inode;
    const unsigned int lclusterbits = vi->z_logical_clusterbits;
    const unsigned int lomask = (1u << lclusterbits) - 1u;
    unsigned int vcnt, base, lo, encodebits, nblk, eofs;
    int i;
    const u8 *in;
    u8 type;
    bool big_pcluster;

    if (1u << amortizedshift == 4u) {
        vcnt = 2;
    } else if (1u << amortizedshift == 2u && lclusterbits == 12u) {
        vcnt = 16;
    } else {
        return BR_ERR(BR_ENOTSUP);
    }

    /* it doesn't equal to round_up(..) */
    m->nextpackoff = round_down(pos, vcnt << amortizedshift) +
        (vcnt << amortizedshift);
    big_pcluster = (vi->z_advise & Z_EROFS_ADVISE_BIG_PCLUSTER_1) ? true : false;
    encodebits = ((vcnt << amortizedshift) - sizeof(__le32)) * 8u / vcnt;
    eofs = (unsigned int)erofs_blkoff(pos);
    base = (unsigned int)round_down(eofs, vcnt << amortizedshift);
    /* ★ 偏离 1: 索引块在共享 scratch 里(上游是 map->mpage)。 */
    in = uboot_erofs_scratch() + base;

    i = (int)((eofs - base) >> amortizedshift);

    lo = decode_compactedbits(lclusterbits, lomask,
                              in, encodebits * (unsigned int)i, &type);
    m->type = type;
    if (type == Z_EROFS_LCLUSTER_TYPE_NONHEAD) {
        m->clusterofs = (u16)(1u << lclusterbits);

        /* figure out lookahead_distance: delta[1] if needed */
        if (lookahead) {
            m->delta[1] = (u16)get_compacted_la_distance(lclusterbits,
                                                         encodebits, vcnt, in, i);
        }
        if (lo & Z_EROFS_LI_D0_CBLKCNT) {
            if (!big_pcluster) {
                DBG_BUGON(1);
                return BR_ERR(EFSCORRUPTED);
            }
            m->compressedblks = lo & ~(unsigned)Z_EROFS_LI_D0_CBLKCNT;
            m->delta[0] = 1;
            return 0;
        } else if (i + 1 != (int)vcnt) {
            m->delta[0] = (u16)lo;
            return 0;
        }
        /*
         * since the last lcluster in the pack is special,
         * of which lo saves delta[1] rather than delta[0].
         * Hence, get delta[0] by the previous lcluster indirectly.
         */
        lo = decode_compactedbits(lclusterbits, lomask,
                                  in, encodebits * (unsigned int)(i - 1), &type);
        if (type != Z_EROFS_LCLUSTER_TYPE_NONHEAD) {
            lo = 0;
        } else if (lo & Z_EROFS_LI_D0_CBLKCNT) {
            lo = 1;
        }
        m->delta[0] = (u16)(lo + 1u);
        return 0;
    }
    m->clusterofs = (u16)lo;
    m->delta[0] = 0;
    /* figout out blkaddr (pblk) for HEAD lclusters */
    if (!big_pcluster) {
        nblk = 1;
        while (i > 0) {
            --i;
            lo = decode_compactedbits(lclusterbits, lomask,
                                      in, encodebits * (unsigned int)i, &type);
            if (type == Z_EROFS_LCLUSTER_TYPE_NONHEAD) {
                i -= (int)lo;
            }

            if (i >= 0) {
                ++nblk;
            }
        }
    } else {
        nblk = 0;
        while (i > 0) {
            --i;
            lo = decode_compactedbits(lclusterbits, lomask,
                                      in, encodebits * (unsigned int)i, &type);
            if (type == Z_EROFS_LCLUSTER_TYPE_NONHEAD) {
                if (lo & Z_EROFS_LI_D0_CBLKCNT) {
                    --i;
                    nblk += lo & ~(unsigned)Z_EROFS_LI_D0_CBLKCNT;
                    continue;
                }
                if (lo <= 1u) {
                    DBG_BUGON(1);
                    /* --i; ++nblk;	continue; */
                    return BR_ERR(EFSCORRUPTED);
                }
                i -= (int)(lo - 2u);
                continue;
            }
            ++nblk;
        }
    }
    in += (vcnt << amortizedshift) - sizeof(__le32);
    m->pblk = le32_to_cpu(*(const __le32 *)(const void *)in) + nblk;
    return 0;
}

static int compacted_load_cluster_from_disk(struct z_erofs_maprecorder *m,
                                            unsigned long lcn, bool lookahead)
{
    struct erofs_inode *const vi = m->inode;
    const unsigned int lclusterbits = vi->z_logical_clusterbits;
    const erofs_off_t ebase = round_up(iloc(vi->nid) + vi->inode_isize +
                                       vi->xattr_isize, 8u) +
        sizeof(struct z_erofs_map_header);
    const unsigned int totalidx = (unsigned int)BLK_ROUND_UP(vi->i_size);
    unsigned int compacted_4b_initial, compacted_2b;
    unsigned int amortizedshift;
    erofs_off_t pos;
    int err;

    if (lclusterbits != 12u) {
        return BR_ERR(BR_ENOTSUP);
    }

    if (lcn >= totalidx) {
        return BR_ERR(BR_EINVAL);
    }

    m->lcn = lcn;
    /* used to align to 32-byte (compacted_2b) alignment */
    compacted_4b_initial = (32u - (unsigned int)(ebase % 32)) / 4u;
    if (compacted_4b_initial == 32u / 4u) {
        compacted_4b_initial = 0;
    }

    if ((vi->z_advise & Z_EROFS_ADVISE_COMPACTED_2B) &&
        compacted_4b_initial < totalidx) {
        compacted_2b = (unsigned int)rounddown(totalidx - compacted_4b_initial, 16u);
    } else {
        compacted_2b = 0;
    }

    pos = ebase;
    if (lcn < compacted_4b_initial) {
        amortizedshift = 2;
        goto out;
    }
    pos += (erofs_off_t)compacted_4b_initial * 4u;
    lcn -= compacted_4b_initial;

    if (lcn < compacted_2b) {
        amortizedshift = 1;
        goto out;
    }
    pos += (erofs_off_t)compacted_2b * 2u;
    lcn -= compacted_2b;
    amortizedshift = 2;
out:
    pos += (erofs_off_t)lcn * (1u << amortizedshift);
    err = z_erofs_reload_indexes(m, erofs_blknr(pos));
    if (err != 0) {
        return err;
    }
    return unpack_compacted_index(m, amortizedshift, pos, lookahead);
}

static int z_erofs_load_cluster_from_disk(struct z_erofs_maprecorder *m,
                                          unsigned int lcn, bool lookahead)
{
    const unsigned int datamode = m->inode->datalayout;

    if (datamode == EROFS_INODE_COMPRESSED_FULL) {
        return legacy_load_cluster_from_disk(m, lcn);
    }

    if (datamode == EROFS_INODE_COMPRESSED_COMPACT) {
        return compacted_load_cluster_from_disk(m, lcn, lookahead);
    }

    return BR_ERR(BR_EINVAL);
}

static int z_erofs_extent_lookback(struct z_erofs_maprecorder *m,
                                   unsigned int lookback_distance)
{
    struct erofs_inode *const vi = m->inode;
    struct erofs_map_blocks *const map = m->map;
    const unsigned int lclusterbits = vi->z_logical_clusterbits;
    unsigned long lcn = m->lcn;
    int err;

    if (lcn < lookback_distance) {
        erofs_err("bogus lookback distance @ nid %llu", (unsigned long long)vi->nid);
        DBG_BUGON(1);
        return BR_ERR(EFSCORRUPTED);
    }

    /* load extent head logical cluster if needed */
    lcn -= lookback_distance;
    err = z_erofs_load_cluster_from_disk(m, (unsigned int)lcn, false);
    if (err != 0) {
        return err;
    }

    switch (m->type) {
    case Z_EROFS_LCLUSTER_TYPE_NONHEAD:
        if (!m->delta[0]) {
            erofs_err("invalid lookback distance 0 @ nid %llu", (unsigned long long)vi->nid);
            DBG_BUGON(1);
            return BR_ERR(EFSCORRUPTED);
        }
        return z_erofs_extent_lookback(m, m->delta[0]);
    case Z_EROFS_LCLUSTER_TYPE_PLAIN:
    case Z_EROFS_LCLUSTER_TYPE_HEAD1:
        m->headtype = m->type;
        map->m_la = ((erofs_off_t)lcn << lclusterbits) | m->clusterofs;
        break;
    default:
        erofs_err("unknown type %u @ lcn %lu of nid %llu",
                  (unsigned)m->type, lcn, (unsigned long long)vi->nid);
        DBG_BUGON(1);
        return BR_ERR(BR_ENOTSUP);
    }
    return 0;
}

static int z_erofs_get_extent_compressedlen(struct z_erofs_maprecorder *m,
                                            unsigned int initial_lcn)
{
    struct erofs_inode *const vi = m->inode;
    struct erofs_map_blocks *const map = m->map;
    const unsigned int lclusterbits = vi->z_logical_clusterbits;
    unsigned long lcn;
    int err;

    DBG_BUGON(m->type != Z_EROFS_LCLUSTER_TYPE_PLAIN &&
              m->type != Z_EROFS_LCLUSTER_TYPE_HEAD1);

    if (m->headtype == Z_EROFS_LCLUSTER_TYPE_PLAIN ||
        !(vi->z_advise & Z_EROFS_ADVISE_BIG_PCLUSTER_1)) {
        map->m_plen = (u64)1 << lclusterbits;
        return 0;
    }

    lcn = m->lcn + 1u;
    if (m->compressedblks) {
        goto out;
    }

    err = z_erofs_load_cluster_from_disk(m, (unsigned int)lcn, false);
    if (err != 0) {
        return err;
    }

    /*
     * If the 1st NONHEAD lcluster has already been handled initially w/o
     * valid compressedblks, which means at least it mustn't be CBLKCNT, or
     * an internal implemenatation error is detected.
     *
     * The following code can also handle it properly anyway, but let's
     * BUG_ON in the debugging mode only for developers to notice that.
     */
    DBG_BUGON(lcn == initial_lcn &&
              m->type == Z_EROFS_LCLUSTER_TYPE_NONHEAD);

    switch (m->type) {
    case Z_EROFS_LCLUSTER_TYPE_PLAIN:
    case Z_EROFS_LCLUSTER_TYPE_HEAD1:
        /*
         * if the 1st NONHEAD lcluster is actually PLAIN or HEAD type
         * rather than CBLKCNT, it's a 1 lcluster-sized pcluster.
         */
        m->compressedblks = (erofs_blk_t)(1u << (lclusterbits - uboot_erofs_sbi.blkszbits));
        break;
    case Z_EROFS_LCLUSTER_TYPE_NONHEAD:
        if (m->delta[0] != 1) {
            goto err_bonus_cblkcnt;
        }
        if (m->compressedblks) {
            break;
        }
        /* fallthrough */
    default:
        erofs_err("cannot found CBLKCNT @ lcn %lu of nid %llu",
                  lcn, (unsigned long long)vi->nid);
        DBG_BUGON(1);
        return BR_ERR(EFSCORRUPTED);
    }
out:
    map->m_plen = (u64)m->compressedblks << uboot_erofs_sbi.blkszbits;
    return 0;
err_bonus_cblkcnt:
    erofs_err("bogus CBLKCNT @ lcn %lu of nid %llu", lcn, (unsigned long long)vi->nid);
    DBG_BUGON(1);
    return BR_ERR(EFSCORRUPTED);
}

static int z_erofs_get_extent_decompressedlen(struct z_erofs_maprecorder *m)
{
    struct erofs_inode *const vi = m->inode;
    struct erofs_map_blocks *map = m->map;
    unsigned int lclusterbits = vi->z_logical_clusterbits;
    u64 lcn = m->lcn, headlcn = map->m_la >> lclusterbits;
    int err;

    do {
        /* handle the last EOF pcluster (no next HEAD lcluster) */
        if ((lcn << lclusterbits) >= vi->i_size) {
            map->m_llen = vi->i_size - map->m_la;
            return 0;
        }

        err = z_erofs_load_cluster_from_disk(m, (unsigned int)lcn, true);
        if (err != 0) {
            return err;
        }

        if (m->type == Z_EROFS_LCLUSTER_TYPE_NONHEAD) {
            DBG_BUGON(!m->delta[1] &&
                      m->clusterofs != 1u << lclusterbits);
        } else if (m->type == Z_EROFS_LCLUSTER_TYPE_PLAIN ||
                   m->type == Z_EROFS_LCLUSTER_TYPE_HEAD1) {
            /* go on until the next HEAD lcluster */
            if (lcn != headlcn) {
                break;
            }
            m->delta[1] = 1;
        } else {
            erofs_err("unknown type %u @ lcn %llu of nid %llu",
                      (unsigned)m->type, (unsigned long long)lcn,
                      (unsigned long long)vi->nid);
            DBG_BUGON(1);
            return BR_ERR(BR_ENOTSUP);
        }
        lcn += m->delta[1];
    } while (m->delta[1]);

    map->m_llen = ((u64)lcn << lclusterbits) + m->clusterofs - map->m_la;
    return 0;
}

static int z_erofs_do_map_blocks(struct erofs_inode *vi,
                                 struct erofs_map_blocks *map,
                                 int flags)
{
    bool ztailpacking = (vi->z_advise & Z_EROFS_ADVISE_INLINE_PCLUSTER) ? true : false;
    bool fragment = (vi->z_advise & Z_EROFS_ADVISE_FRAGMENT_PCLUSTER) ? true : false;
    struct z_erofs_maprecorder m = {
        .inode = vi,
        .map = map,
        /* ★ 偏离 1: 缓存标记的初值 —— "没读过" */
        .index = (erofs_blk_t)-1,
    };
    int err = 0;
    unsigned int lclusterbits, endoff;
    unsigned long initial_lcn;
    unsigned long long ofs, end;

    lclusterbits = vi->z_logical_clusterbits;
    ofs = (flags & EROFS_GET_BLOCKS_FINDTAIL) ? vi->i_size - 1u : map->m_la;
    initial_lcn = (unsigned long)(ofs >> lclusterbits);
    endoff = (unsigned int)(ofs & ((1u << lclusterbits) - 1u));

    err = z_erofs_load_cluster_from_disk(&m, (unsigned int)initial_lcn, false);
    if (err != 0) {
        goto out;
    }

    if (ztailpacking && (flags & EROFS_GET_BLOCKS_FINDTAIL)) {
        vi->z_idataoff = m.nextpackoff;
    }

    map->m_flags = EROFS_MAP_MAPPED | EROFS_MAP_ENCODED;
    end = ((unsigned long long)m.lcn + 1ULL) << lclusterbits;
    switch (m.type) {
    case Z_EROFS_LCLUSTER_TYPE_PLAIN:
    case Z_EROFS_LCLUSTER_TYPE_HEAD1:
        if (endoff >= m.clusterofs) {
            m.headtype = m.type;
            map->m_la = ((erofs_off_t)m.lcn << lclusterbits) | m.clusterofs;
            break;
        }
        /* m.lcn should be >= 1 if endoff < m.clusterofs */
        if (!m.lcn) {
            erofs_err("invalid logical cluster 0 at nid %llu", (unsigned long long)vi->nid);
            err = BR_ERR(EFSCORRUPTED);
            goto out;
        }
        end = ((unsigned long long)m.lcn << lclusterbits) | m.clusterofs;
        map->m_flags |= EROFS_MAP_FULL_MAPPED;
        m.delta[0] = 1;
        /* fallthrough */
    case Z_EROFS_LCLUSTER_TYPE_NONHEAD:
        /* get the correspoinding first chunk */
        err = z_erofs_extent_lookback(&m, m.delta[0]);
        if (err != 0) {
            goto out;
        }
        break;
    default:
        erofs_err("unknown type %u @ offset %llu of nid %llu",
                  (unsigned)m.type, ofs, (unsigned long long)vi->nid);
        err = BR_ERR(BR_ENOTSUP);
        goto out;
    }
    if (m.partialref) {
        map->m_flags |= EROFS_MAP_PARTIAL_REF;
    }
    map->m_llen = end - map->m_la;
    if (flags & EROFS_GET_BLOCKS_FINDTAIL) {
        vi->z_tailextent_headlcn = m.lcn;
        /* for non-compact indexes, fragmentoff is 64 bits */
        if (fragment && vi->datalayout == EROFS_INODE_COMPRESSED_FULL) {
            vi->fragmentoff |= (u64)m.pblk << 32;
        }
    }
    if (ztailpacking && m.lcn == vi->z_tailextent_headlcn) {
        map->m_flags |= EROFS_MAP_META;
        map->m_pa = vi->z_idataoff;
        map->m_plen = vi->z_idata_size;
    } else if (fragment && m.lcn == vi->z_tailextent_headlcn) {
        map->m_flags |= EROFS_MAP_FRAGMENT;
    } else {
        map->m_pa = erofs_pos(m.pblk);
        err = z_erofs_get_extent_compressedlen(&m, (unsigned int)initial_lcn);
        if (err != 0) {
            goto out;
        }
    }

    if (m.headtype == Z_EROFS_LCLUSTER_TYPE_PLAIN) {
        if (map->m_llen > map->m_plen) {
            DBG_BUGON(1);
            err = BR_ERR(EFSCORRUPTED);
            goto out;
        }
        if (vi->z_advise & Z_EROFS_ADVISE_INTERLACED_PCLUSTER) {
            map->m_algorithmformat = Z_EROFS_COMPRESSION_INTERLACED;
        } else {
            map->m_algorithmformat = Z_EROFS_COMPRESSION_SHIFTED;
        }
    } else {
        map->m_algorithmformat = (char)vi->z_algorithmtype[0];
    }

    if (flags & EROFS_GET_BLOCKS_FIEMAP) {
        /* ★ 偏离 1: 这一段会做 lookahead 的索引读(走共享 scratch) ⇒ 缓存标记失效。 */
        s_z_scratch_valid = false;
        err = z_erofs_get_extent_decompressedlen(&m);
        if (!err) {
            map->m_flags |= EROFS_MAP_FULL_MAPPED;
        }
    }

out:
#ifdef BR_EROFS_VERBOSE_DBG
    erofs_dbg("m_la %llu m_pa %llu m_llen %llu m_plen %llu m_flags 0%o",
              (unsigned long long)map->m_la, (unsigned long long)map->m_pa,
              (unsigned long long)map->m_llen, (unsigned long long)map->m_plen,
              (unsigned)map->m_flags);
#endif
    return err;
}

int z_erofs_map_blocks_iter(struct erofs_inode *vi,
                            struct erofs_map_blocks *map,
                            int flags)
{
    int err = 0;

    /* ★ 偏离 1: 进入映射时谁也不信 —— scratch 可能刚被别人用过。
     * 同时把 map 的 index 归零, 免得 `z_erofs_reload_indexes()` 的旧值骗过录制器。 */
    s_z_scratch_valid = false;
    map->index = (erofs_blk_t)-1;

    /* when trying to read beyond EOF, leave it unmapped */
    if (map->m_la >= vi->i_size) {
        map->m_llen = map->m_la + 1u - vi->i_size;
        map->m_la = vi->i_size;
        map->m_flags = 0;
        goto out;
    }

    err = z_erofs_fill_inode_lazy(vi);
    if (err != 0) {
        goto out;
    }

    if ((vi->z_advise & Z_EROFS_ADVISE_FRAGMENT_PCLUSTER) &&
        !vi->z_tailextent_headlcn) {
        map->m_la = 0;
        map->m_llen = vi->i_size;
        map->m_flags = EROFS_MAP_MAPPED | EROFS_MAP_FULL_MAPPED |
            EROFS_MAP_FRAGMENT;
        goto out;
    }

    err = z_erofs_do_map_blocks(vi, map, flags);
out:
    /* 偏离 3: 上游这里是 `DBG_BUGON(err < 0 && err != -ENOMEM)`(停机)。本件的
     * DBG_BUGON 只打印(见 erofs_compat.h), 错误码照原样回传。 */
    DBG_BUGON(err < 0 && err != BR_ERR(BR_ENOMEM));
    return err;
}
