// SPDX-License-Identifier: GPL-2.0+
/*
 * third_party/uboot-erofs — 超级块(移植自 u-boot `fs/erofs/super.c`, GPL-2.0+)
 *
 * ============================== 上游对照 ==============================
 *   上游文件 : fs/erofs/super.c(108 行)
 *   移植范围 : `erofs_read_superblock()` 的全部字段解析 + 特性位校验 + 布局兼容性检查。
 *   逐函数对照:
 *     check_layout_compatibility()  → 保留(语义一字不改)
 *     erofs_init_devices()          → **改写成单卷版**(见下"偏离 1")
 *     erofs_read_superblock()       → 保留(读法/字段完全一致)
 *
 * ============================== 有意偏离 ==============================
 *   1. **单卷**(任务书的设计): 上游支持"多设备 EROFS"(extra devices 表, 每台设备一段
 *      块地址空间)。本插件是一个卷、一个 bdev(`emmc3`), 所以:
 *        - `ondisk_extradevs != 0` ⇒ `-ENOTSUP`(不是 -EINVAL: 镜像本身合法, 是本件不支持);
 *        - `total_blocks = primarydevice_blocks`, `devs` 表整个不存在(结构体里已裁掉)。
 *      `erofs_map_dev()`(data.c)对单卷因此是恒等映射。
 *   2. **超级块读取的缓冲**: 上游用栈上的 `u8 data[EROFS_MAX_BLOCK_SIZE]`(4 KiB)。
 *      本件改用共享的静态 scratch(见 erofs_internal.h 的"栈纪律")。
 *      读出来的 `dsb` 指针指向 scratch + EROFS_SUPER_OFFSET, 生命周期只在本函数内。
 *   3. **magic 的位置是硬要求**: `EROFS_SUPER_OFFSET` = 1024, 也就是说 magic 必须出现在
 *      **偏移 1024** 处。上游的实现方式是"读第 0 块(4 KiB)再偏移 1024" —— 本件照做,
 *      因为 blkszbits 本身是从这里读出来的, 此时还不知道块大小(鸡与蛋)。
 *   4. `sbi` → `uboot_erofs_sbi`(符号前缀; 结构体本身已在 erofs_internal.h 里裁过)。
 *   5. 错误路径: 上游只打 `erofs_dbg`(release 下静默), 本件在**校验失败**处打
 *      `br_log_error` —— 挂载失败在"首败即停机"的口径下必须留下可读的原因。
 * ==========================================================================
 */
#include "erofs_internal.h"

struct erofs_sb_info uboot_erofs_sbi;

static bool check_layout_compatibility(struct erofs_sb_info *sbi,
                                       struct erofs_super_block *dsb)
{
    const unsigned int feature = le32_to_cpu(dsb->feature_incompat);

    sbi->feature_incompat = feature;

    /* check if current kernel meets all mandatory requirements */
    if (feature & ~(unsigned int)EROFS_ALL_FEATURE_INCOMPAT) {
        br_log_error("uboot-erofs: 未知的 incompatible 特性 %x(镜像比本件新?)",
                     feature & ~(unsigned int)EROFS_ALL_FEATURE_INCOMPAT);
        return false;
    }
    return true;
}

/*
 * 偏离 1: 单卷版。上游会在这里 calloc 出 extra-devices 表并逐个读 `erofs_deviceslot`
 * 把各设备的块数累加进 `total_blocks`。本件一个 bdev, 于是:
 *   - 盘上说"有 n 个额外设备"而本件只认 1 个 ⇒ 直接拒(而不是装作没看见继续读 ——
 *     那样会在读多设备文件时把别的设备的块号解释成本设备的块号)。
 *   - 不分配任何东西(于是也没有上游的 `free(sbi->devs)` 回滚路径)。
 */
static int erofs_init_devices(struct erofs_sb_info *sbi,
                              struct erofs_super_block *dsb)
{
    unsigned int ondisk_extradevs;

    sbi->total_blocks = sbi->primarydevice_blocks;

    ondisk_extradevs = erofs_sb_has_device_table()
                     ? (unsigned int)le16_to_cpu(dsb->extra_devices) : 0u;

    if (ondisk_extradevs != 0u) {
        br_log_error("uboot-erofs: 镜像含 %u 个额外设备, 本插件只支持单卷",
                     ondisk_extradevs);
        return BR_ERR(BR_ENOTSUP);
    }
    return 0;
}

int erofs_read_superblock(void)
{
    struct erofs_sb_info  *sbi = &uboot_erofs_sbi;
    struct erofs_super_block *dsb;
    u8 *data = uboot_erofs_scratch();
    int ret;

    /* 读第 0 块(此时还不知道块大小, 所以按**本平台的最大块** 4 KiB 读)。
     * 4 KiB = 8 个 512 B 扇区 ⇒ 8 次单扇区读, 每次 1 ≤ geometry.max_rw。 */
    ret = erofs_blk_read(data, 0, erofs_blknr((erofs_off_t)EROFS_MAX_BLOCK_SIZE));
    if (ret < 0) {
        br_log_error("uboot-erofs: 读超级块失败 rc=%d", ret);
        return BR_ERR(BR_EIO);
    }
    dsb = (struct erofs_super_block *)(void *)(data + EROFS_SUPER_OFFSET);

    ret = BR_ERR(BR_EINVAL);
    if (le32_to_cpu(dsb->magic) != EROFS_SUPER_MAGIC_V1) {
        br_log_error("uboot-erofs: 偏移 %u 处的 magic 不是 0x%08x(实得 0x%08x)",
                     (unsigned)EROFS_SUPER_OFFSET, (unsigned)EROFS_SUPER_MAGIC_V1,
                     (unsigned)le32_to_cpu(dsb->magic));
        return ret;
    }

    /* ★ 有意偏离(记一笔): 上游**不**校验 checksum(它只在 mkfs 侧算)。
     *   本件同样不校验 —— 多读一次超级块 + 一次 crc32c 在这个规模上换不到什么,
     *   而且没有 crc32c 的公共实现可复用(上游 internal.h 里那段是给 mkfs 用的)。
     *   登记为 README 的欠债。 */

    sbi->feature_compat = le32_to_cpu(dsb->feature_compat);

    sbi->blkszbits = dsb->blkszbits;
    if (sbi->blkszbits < 9u ||
        sbi->blkszbits > (unsigned char)ilog2(EROFS_MAX_BLOCK_SIZE)) {
        br_log_error("uboot-erofs: blkszbits=%u 不受支持(本平台 9..%u)",
                     (unsigned)sbi->blkszbits, (unsigned)ilog2(EROFS_MAX_BLOCK_SIZE));
        return ret;
    } else if (!check_layout_compatibility(sbi, dsb)) {
        return ret;
    }

    sbi->primarydevice_blocks = le32_to_cpu(dsb->blocks);
    sbi->meta_blkaddr         = le32_to_cpu(dsb->meta_blkaddr);
    sbi->xattr_blkaddr        = le32_to_cpu(dsb->xattr_blkaddr);
    sbi->islotbits            = EROFS_ISLOTBITS;
    sbi->root_nid             = le16_to_cpu(dsb->root_nid);
    sbi->packed_nid           = le64_to_cpu(dsb->packed_nid);
    sbi->inos                 = le64_to_cpu(dsb->inos);
    sbi->checksum             = le32_to_cpu(dsb->checksum);

    sbi->build_time      = le64_to_cpu(dsb->build_time);
    sbi->build_time_nsec = le32_to_cpu(dsb->build_time_nsec);

    /* uuid / volume_name: 上游 memcpy 进 sbi.uuid; 本件保留 uuid(诊断), 裁掉 volume_name
     * (没有消费者 —— u-boot 的 erofs 只回答 uuid, 而本件连 uuid 都不往上抛)。 */
    memcpy(&sbi->uuid, dsb->uuid, sizeof(dsb->uuid));

    /* ★ 本件**不**写 `sbi->available_compr_algs` / `sbi->lz4_max_distance`(ADR-0024 §9)。
     *   上游这个 commit 从头到尾**没有**给它们赋过值, 也没有任何读者(全树 grep 只有
     *   `erofs_fs.h`/`internal.h` 的字段声明)。本件早先抄了一段(Linux 内核 super.c 的
     *   口径)去填它们, 但填完没人读 —— 那正是"上游没有的代码" ⇒ 已删, 恢复上游语义。
     *   压缩算法的判定一律由 zmap 的 map header 说话(上游亦然)。 */

    return erofs_init_devices(sbi, dsb);
}
