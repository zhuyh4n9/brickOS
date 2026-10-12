/*
 * adapter/uboot-fat — 块层 shim + 卷表 + BPB 观测 + 环境的"我们实现的那几个"(本项目代码)
 *
 * ============================== 本文件在分层里的位置 ==============================
 * 上游 `fs/fat/fat_write.c`(它 include 了 `fat.c`)是**一个编译单元**, 它对环境的全部要求
 * 都在 `compat/fat_env.h` 里兑现; 本文件补上三件**环境侧必须有实体**的东西:
 *   ① `blk_dread/blk_dwrite`: 上游 `disk_read/disk_write` 的落地 —— 接到
 *      `br_bdev_ops_t.read/write`, 并按设备几何切块(max_rw)+ 越界拒绝;
 *   ② 卷表与绑定: 上游的"当前设备"是 `static struct blk_desc *cur_dev`, 本件把它变成
 *      "当前卷"(`s_cur_vol`), 绑定动作走**上游自己的** `fat_set_blk_dev()`
 *      (整设备一个分区 ⇒ `start = 0`, `size = n_sectors`) —— 于是上游一行都不用改;
 *   ③ 环境头里声明、但**只能由本插件实现**的小工具: `uboot_fat_strdup`/`uboot_fat_sprintf`/
 *      `uboot_fat_rand`, 以及截断的公开 API 拼装 `uboot_fat_truncate`。
 *
 * ============================== 为什么 glue 也要解析 BPB ==============================
 * 上游的 `get_fs_info()`/`determine_legacy_fat_bits()` 是 **static**(单编译单元的私有
 * 实现), 没有任何对外观测点。而本插件的挂载判据(TC-FAT-001 + mount 的几何校验)要回答
 * "这个卷是 FAT12/16/32、簇数落在定义区间、数据区在卷内吗"。于是 glue 用 vendored
 * `include/fat.h` 的 `struct boot_sector`(**布局的唯一真值**还是 vendor 那份)**只读地**
 * 解析一遍 —— 它只喂判据与日志, **不参与任何 I/O 决策**(I/O 全部在上游手里, 上游每次
 * API 调用都会自己重解析一遍 BPB)。这是"观测"而不是"第二套实现":
 *   · 判定算法与上游 `determine_legacy_fat_bits` 同形(逐行对照过), 出处写在函数头;
 *   · 若观测结论与上游不一致, 表现是**挂载期判据红**(而不是"读写错了"), 因为读写不信它。
 * ==========================================================================
 */
#include "fat_glue.h"

/* 盘上结构(boot_sector/dir_entry/MAX_LFN_SLOT/…): compat 的 `<fat.h>` 转发壳 ⇒
 * 内容来自 vendor 的 `include/fat.h`, 我们这边一个字段都不抄。 */
#include <fat.h>

/* ==================================================================== 卷表与当前卷
 *
 * 上游是"全局当前设备 + 当前分区"(`cur_dev`/`cur_part_info`)。本件保留**同一形状**:
 * "当前卷"指针 + 由它构造出来的 `struct blk_desc`/`disk_partition`。挂载期是本插件单线程
 * 的一次性动作(start 相), 自检也跑在同一线程 ⇒ 不做锁(与 cdev/bdev 同款的 v1 静态假设;
 * 并发的真正缺口登记在 WORKAROUNDS.md 的 br-wa-boot-002)。
 */

static uboot_fat_vol_t  s_vols[UB_FAT_VOL_MAX];
static uboot_fat_vol_t *s_cur_vol;

/* "整设备一个分区"的描述符 + 分区表项(每次绑定按卷几何刷新) */
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
 * 卷内扇区号 → 设备 lba。整设备一个分区(start = 0)⇒ **恒等**, 但保留这层是为了让
 * "分区偏移"这件事在代码里仍然显式可见(上游写的是 `cur_part_info.start + block`)。
 */
static br_u64 uboot_fat_lba_of(br_u32 block)
{
    return ((br_u64)s_part.start) + (br_u64)block;
}

/* ==================================================================== 块层(shim)
 *
 * 上游判据: 成功 = 传输的扇区数(== 请求数), 失败 < 0。本件在这里多两件事:
 *   ① 按 `geometry.max_rw` 切块(单次 I/O 不超过 bdev 上限; 上游没有这条约束);
 *   ② 越界拒绝(整卷一个分区 ⇒ 合法 lba 区间是 [start, start + size))。
 * 任一块失败即返回负 errno ⇒ 上游的 `ret != nr_blocks` 判据仍然成立。
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
        /* bdev 契约: 成功 = 传输扇区数(**允许短读**)。短了按设备错误处理 ——
         * 上游用 `ret != nr_blocks` 判定, 本件在切块层面就把它收敛掉。 */
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
    (void)desc;                     /* 本件只有一个"当前卷", 描述符只是形状 */
    return uboot_fat_blk_rw(start, blkcnt, buf, 0);
}

unsigned long blk_dwrite(struct blk_desc *desc, lbaint_t start, lbaint_t blkcnt, const void *buf)
{
    (void)desc;
    return uboot_fat_blk_rw(start, blkcnt, (void *)(br_uintptr_t)buf, 1);
}

/* ==================================================================== BPB 观测 */

/*
 * FAT 类型判定。**与上游 `fs/fat/fat.c` 的 `determine_legacy_fat_bits()` 同形**
 * (上游那份是 static: 基于 Linux `fat_fill_super()` 的算法; 这里逐行对照重写一遍是为了
 * 让**挂载判据**能拿到类型 —— 见文件头"为什么 glue 也要解析 BPB")。
 * 32 位那一支另按上游 `read_bootsectandvi()` 的判据: `fat_length == 0 && fat32_length != 0`。
 */
static int uboot_fat_detect_bits(const boot_sector *bs)
{
    const u32 fat_start = bs->reserved;              /* 已由调用方 FAT2CPU 过 */
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
        return 0;                       /* 畸形: 交给挂载判据拒挂 */
    }
    if (total_sectors <= data_start) {
        return 0;
    }
    span = total_sectors - data_start;
    total_clusters = span / bs->cluster_size;
    return (total_clusters > UB_FAT_MAX_FAT12) ? 16 : 12;
}

/* 读引导扇区(第 0 扇区)并按 vendored `boot_sector` 的字段填观测值。返回 0 / 负 errno。 */
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
        br_log_error("uboot-fat: %s: 读引导扇区失败 rc=%d", vol->dev, rc);
        return (rc < 0) ? rc : BR_ERR(EIO);
    }

    memset(&bs, 0, sizeof(bs));
    memcpy(&bs, sec, sizeof(bs));

    /* 与上游 `read_bootsectandvi()` 同款: 结构体里是**盘上原字节**, __u16/__u32 字段要过一遍
     * FAT2CPU(小端目标上是恒等, 见 compat/fat_env.h 的 #error); __u8[2] 的字段(扇区大小/
     * 目录项数/扇区数)按 LE 取, 与上游一样用 get_unaligned_le16。 */
    bs.reserved    = FAT2CPU16(bs.reserved);
    bs.fat_length  = FAT2CPU16(bs.fat_length);
    bs.total_sect  = FAT2CPU32(bs.total_sect);

    sect_size = FAT2CPU16(get_unaligned_le16(bs.sector_size));
    if (sect_size != vol->geom.sector_size) {
        br_log_error("uboot-fat: %s: BPB 扇区大小 %u != 设备 %u",
                     vol->dev, (unsigned)sect_size, (unsigned)vol->geom.sector_size);
        return BR_ERR(EINVAL);
    }
    if (bs.cluster_size == 0u) {
        br_log_error("uboot-fat: %s: BPB 簇大小未设置", vol->dev);
        return BR_ERR(EINVAL);
    }

    vol->sect_size = sect_size;
    vol->clust_size = bs.cluster_size;
    vol->fats = bs.fats;
    vol->fat_sect = bs.reserved;
    vol->fatsize = uboot_fat_detect_bits(&bs);
    if (vol->fatsize == 0) {
        br_log_error("uboot-fat: %s: BPB 自相矛盾(无法判定 FAT 类型)", vol->dev);
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
        total_sect = (u32)vol->geom.n_sectors;   /* 上游同款兜底 */
    }
    vol->fatlength = fatlength;
    vol->total_sect = total_sect;
    vol->rootdir_sect = vol->fat_sect + (fatlength * vol->fats);

    if (vol->fatsize == 32) {
        /* 上游: data_begin = rootdir_sect - clust_size*2(簇 2 起算) */
        vol->data_begin = vol->rootdir_sect - (vol->clust_size * 2u);
    } else {
        /* 非 FAT32 的根目录不在簇链上, 可能落在"负簇"(上游注释明说) */
        vol->data_begin = vol->rootdir_sect + vol->rootdir_size -
                          (vol->clust_size * 2u);
    }
    return 0;
}

/* ==================================================================== 卷绑定(fat_set_blk_dev 的位置) */

/*
 * 上游 `fat_set_blk_dev(dev_desc, info)`: 记下当前设备 + 校验引导扇区魔数 0x55AA 与
 * 文件系统签名。本件 `uboot_fat_vol_bind(vol)`:
 *   ① 取 bdev 几何并校验(512 B 扇区 + 扇区数非 0);
 *   ② 按几何装填 shim 的 `struct blk_desc`/`disk_partition`(整设备一个分区);
 *   ③ 调**上游自己的** `fat_set_blk_dev()` —— 挂载期的引导扇区校验归上游(我们不改它的判据);
 *   ④ 再读一遍 BPB 填观测值(判据/日志用, 见文件头)。
 * 返回 0 或负 errno; 失败时不留半绑定状态。
 */
int uboot_fat_vol_bind(uboot_fat_vol_t *vol)
{
    int rc;

    if (vol == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (vol->bound == BR_TRUE) {
        /* 已绑定: 只把它切成当前卷(等价于上游 fat_set_blk_dev 的"换设备"语义)。
         * ★ 多卷挂载的关键: 上游只认"当前卷"(fat.c 的 static cur_dev), 所以 glue 在
         *   **每个入口**都要先 bind 一次, 否则会拿上一个卷的 FAT 参数去读这一个卷。 */
        s_cur_vol = vol;
        s_desc.lba     = (lbaint_t)vol->geom.n_sectors;
        s_desc.blksz   = (unsigned long)vol->geom.sector_size;
        s_part.start   = 0u;
        s_part.size    = (lbaint_t)vol->geom.n_sectors;
        s_part.blksz   = (ulong)vol->geom.sector_size;
        /* ★ 即使已经绑定过, 也**仍然**过一遍上游的 `fat_set_blk_dev()`: 它刷新上游那两个
         *   static 全局(`cur_dev`/`cur_part_info`, 本件只有这一个消费者)并重新校验引导扇区
         *   魔数 —— 代价是每个入口一次 512 B 读, 换来的是"卷被换掉/介质变了"这种情况不会被
         *   静默吃掉。失败 ⇒ 不留半绑定状态。 */
        if (fat_set_blk_dev(&s_desc, &s_part) != 0) {
            br_log_error("uboot-fat: %s: 重新绑定失败(引导扇区无效)", vol->dev);
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
        br_log_error("uboot-fat: %s sector_size=%u n_sectors=%llu (需要 512 B 扇区)",
                     vol->dev, (unsigned)vol->geom.sector_size,
                     (unsigned long long)vol->geom.n_sectors);
        return BR_ERR(EINVAL);
    }

    /* 整设备一个分区: start = 0, size = 设备扇区数(u-boot 的 part 0 无分区表时同形) */
    memset(&s_desc, 0, sizeof(s_desc));
    memset(&s_part, 0, sizeof(s_part));
    s_desc.uclass_id = 0;
    s_desc.devnum    = 0;
    s_desc.lba       = (lbaint_t)vol->geom.n_sectors;
    s_desc.blksz     = (unsigned long)vol->geom.sector_size;
    s_part.start     = 0u;
    s_part.size      = (lbaint_t)vol->geom.n_sectors;
    s_part.blksz     = (ulong)vol->geom.sector_size;

    s_cur_vol = vol;                    /* 后续所有 I/O 都落在这个卷上 */
    if (fat_set_blk_dev(&s_desc, &s_part) != 0) {
        br_log_error("uboot-fat: %s: 不是有效的 FAT 引导扇区", vol->dev);
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

/* 解绑当前卷(幂等)。挂载失败的回滚路径也走它。 */
void uboot_fat_vol_unbind(void)
{
    if (s_cur_vol != BR_NULL) {
        s_cur_vol->bound = BR_FALSE;
    }
    s_cur_vol = BR_NULL;
}

/*
 * (源设备, 挂载点) → 卷 id  —— **认领一个卷槽**。
 *
 * ★ ADR-0030: 对应关系不再写死在插件的表里, 而由 **APP 的挂载计划**给出:
 *   APP 调 `mount("emmc0", "/fat12", "fat", 0, NULL)`, 于是 (emmc0, /fat12) 成为一对。
 *   同一个 (dev, mnt) 重复认领 ⇒ 复用同一个槽(幂等; 也挡住"同一卷挂两次");
 *   槽用完 ⇒ -ENOSPC(本件的并发挂载卷数上界 = UB_FAT_VOL_MAX)。
 *
 * ★ 这条改动消灭了一处**两处真值**: 从前"挂载点 ↔ 设备"既写在插件的表里、又写在
 *   manifest 的 [[mount]] 里, 改一处不报错、只会静默挂错设备(见 br-wa-fs-001)。
 */
int uboot_fat_vol_claim(const char *dev, const char *mnt, br_u32 *id)
{
    if ((dev == BR_NULL) || (mnt == BR_NULL) || (id == BR_NULL)) {
        return BR_ERR(EINVAL);
    }
    /* ① 已经认领过同一对 ⇒ 复用(幂等) */
    for (br_u32 i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
        if ((s_vols[i].dev != BR_NULL) && (s_vols[i].mnt != BR_NULL) &&
            (uboot_fat_strcmp(s_vols[i].dev, dev) == 0) &&
            (uboot_fat_strcmp(s_vols[i].mnt, mnt) == 0)) {
            *id = i;
            return 0;
        }
    }
    /* ② 否则占一个空槽 */
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

/* 卷的事实观测(挂载判据 + 自检 TC-FAT-001 用): 只读, 不改任何状态。 */
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

/* ==================================================================== 环境的"我们实现的那几个" */

/*
 * `strdup`(上游写路径用它复制路径; 无 libc)。上界见 compat/fat_env.h: 本插件的 VFS 面
 * 把路径限在 `UB_FAT_PATH_MAX` 内。超长/无内存都返回 NULL —— 上游调用点对 NULL 的答复是
 * `-ENOMEM`(逐条查过), 于是失败面与 libc 口径一致。
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
        br_log_error("uboot-fat: 路径超过 %u 字节, 拒绝复制", (unsigned)(UB_FAT_STRDUP_CAP - 1u));
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
 * 有界 `sprintf`(无 libc; 上游四处调用点的转换符逐个覆盖: `%.*s` / `%.3s` / `%d` / `%02X` /
 * `%%`)。逆序生成数字、超界丢弃; 返回"应写入的字符数"(sprintf 口径), 结尾恒有 '\0'。
 * ★ 为什么不是 `__builtin_sprintf`: 裸机没有它, 而 core 的 `br_log_write` 只输出不回填
 *   缓冲 ⇒ 这里必须自带一个极小格式化器(与 adapter/uboot-erofs 的判据面同款取舍)。
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
        i++;                                        /* 跳过 '%' */
        if (fmt[i] == '0') {                        /* 只为 `%02X` 支持 '0' 标志 */
            zero_pad = 1;
            i++;
        }
        while ((fmt[i] >= '0') && (fmt[i] <= '9')) {
            width = (width * 10) + (int)(fmt[i] - '0');
            i++;
        }
        if (fmt[i] == '.') {
            i++;
            if (fmt[i] == '*') {                    /* `%.*s`(上游两处) */
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
            /* 逆序生成, 再按 width(仅 '0'/空格填充)补齐后正序吐出 */
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
            /* 不认的转换符: 原样吐出(不静默吞掉, 免得"日志/名字少了一段"这种诡异现象) */
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
 * `rand()`(上游 `set_name()` 用它挑"~N"短名后缀的搜索起点)。u-boot 的 `rand()` 没有对应的
 * vendored 原件(见 NOTICE), 而**确定性**在这里是优点: 候选空间本来就是"从 1 逐个试到不
 * 冲突", 起点不影响"找到的那个名字"这个契约(只影响搜索顺序), 却让同样的盘上状态给出同样
 * 的结果 ⇒ 自检可复现。LCG 常数取自 Numerical Recipes(与具体实现无关, 只需良好散布)。
 */
static unsigned int s_rand_state = 0x12345678u;

int uboot_fat_rand(void)
{
    s_rand_state = (s_rand_state * 1664525u) + 1013904223u;
    return (int)(s_rand_state >> 1);
}

/* ==================================================================== 截断(只用上游公开 API) */

/*
 * 截断/扩展到 `newsize`。上游**没有** truncate 入口, 但它自己有一套公开的写语义可以拼出
 * 三种情形(逐条对着上游源码核过, 见 ADR-0026 §5 的实测结论):
 *
 *   ① **变短**: `file_fat_write_at(path, newsize, NULL, 0, &act)` —— 上游先把目录项 size
 *      改成 `pos + size`(= newsize), 然后 `set_contents()` 走到 "truncate the rest" 分支:
 *      `clear_fatent()` 把尾部簇链还回去, 并给末簇打 EOC。`get_set_cluster(size == 0)` 立刻
 *      返回 ⇒ **NULL buffer 不会被解引用**(这一条是"传 NULL 安全"的全部理由)。
 *   ② **变 0**: 同一调用(`newsize == 0`)走上游 `filesize == 0` 的专门分支: 清整条簇链 +
 *      把首簇置 0 + size = 0。
 *   ③ **变长**: 上游没有"扩洞"语义(`set_contents` 的 `assert(!pos)`/`assert(pos == cur_pos)`
 *      是死代码, 而它按簇走查的路径在"pos 落在链尾之外"时会报 "something wrong") ⇒ 扩展
 *      只能**显式写零字节**: 从当前长度起追加 `newsize - cur` 个 0(分块, 512 B/次)。
 *      TC-FAT-008 要求的正是"新区域读回 0"(而不是上一个文件的残留), 所以这条实现与判据
 *      同义 —— 我们**没有**放宽判据, 只是把"扩展"这一支写成上游真能接受的形式。
 *
 * 返回值: 0 / 负 errno。
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
        return 0;                        /* 幂等: 长度没变就不碰介质 */
    }

    if (newsize < (br_u64)cur) {
        /* ①/② 变短或变 0 —— 上游自己的截断分支 */
        rc = file_fat_write_at(path, (loff_t)newsize, BR_NULL, 0, &act);
        return (rc < 0) ? rc : 0;
    }

    /* ③ 变长 —— 显式零填充(见函数头) */
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
                return BR_ERR(EIO);      /* 没写进去: 不许假装成功 */
            }
            pos += act;
        }
    }
    return 0;
}
