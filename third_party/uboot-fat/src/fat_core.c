// SPDX-License-Identifier: GPL-2.0+
/*
 * third_party/uboot-fat — FAT12/16/32 读路径(移植)
 *
 * ============================== 移植来源与范围 ==============================
 *   上游: `nxp-imx/uboot-imx` 分支 `lf_v2026.04`, commit
 *         `bb41975aeb4cbd153b49c9fd62da1d64cd65968d`, 文件 `fs/fat/fat.c`(1448 行, GPL-2.0+,
 *         Copyright 2002 Marcus Sundberg / rjones@nexus-tech.net / kharris@nexus-tech.net)
 *   本文件: 该文件的**读路径整段**移植 + 本项目的**块层 shim**:
 *     · shim:          `struct blk_desc`/`disk_partition`/`blk_dread`/`blk_dwrite`
 *                      → 卷(uboot_fat_vol_t)+ `br_bdev_ops_t`(见文件末的 shim 一节)
 *     · 卷识别:        downcase / get_name / flush_dirty_fat_buffer 的前置声明 /
 *                      get_fatent / get_cluster / get_contents /
 *                      determine_legacy_fat_bits / fat_valid_media / is_bootsector_valid /
 *                      read_bootsectandvi / get_fs_info
 *     · 目录迭代:      struct fat_itr / fat_itr_root / fat_itr_child / fat_next_cluster /
 *                      next_dent / extract_vfat_name / fat_itr_next / fat_itr_isdir /
 *                      fat_itr_resolve / slot2str / mkcksum / TYPE_* 掩码
 *     · 只读文件面:    fat_exists / fat_size / fat_read_file / file_fat_read
 *     · 目录流:        fat_dir(类型在 fat_internal.h)/ fat_opendir / fat_readdir / fat_closedir
 *     · 卷生命周期:    fat_set_blk_dev / fat_register_device 的**语义**(换成 uboot_fat_vol_*)
 *
 * **未移植**(见 NOTICE 的"未移植清单"): `file_fat_detectfs` 的 u-boot 设备打印、
 * `fat2rtc`/`struct rtc_time`(v1 无 RTC)、`fat_uuid`(要 sprintf)、`fat_close` 的空壳保留
 * (签名一致, 便于消费者)。
 *
 * ============================== 有意偏离(相对上游) ==============================
 *   1. `malloc_cache_aligned()` → `br_malloc()`。本件没有 DMA: 块层是裸 memcpy 到设备缓冲,
 *      而 core 堆的对齐 **≥ 16 B**(br_mem.h 的 BR_MALLOC_ALIGN)且本插件的结构体最大对齐
 *      需求是 8 B ⇒ "cache aligned" 这一层没有对象, 于是**不移植** `ALLOC_CACHE_ALIGN_BUFFER`
 *      与 `ARCH_DMA_MINALIGN` 分支(上游那两段是为 SDK 的 DMA 一致性写的)。
 *   2. `get_cluster()` 里"缓冲未对齐就走单扇区"的分支**删掉**(理由同上); 单次 I/O 的
 *      **真正**约束(max_rw = 64 扇区)由 shim 的 disk_read/disk_write 内部切块保证。
 *   3. `printf()` → `br_log_error()`: 上游在数据路径上"打印并继续", 本件改成打印 + 返回错误码
 *      (半个结果比一个错误码危险得多)。
 *   4. 上游 `get_contents()` 里 `printf("Invalid FAT entry\n")` 之后 `return -1` 的分支保留,
 *      但 FIXME 式的"错误路径返回正数"没有了(fat_itr_resolve 恒负 errno)。
 *   5. 卷的绑定是**显式 API**(uboot_fat_vol_bind)而不是 `fat_register_device(desc, part)`:
 *      本件"整设备一个分区", 分区表(gpt/mbr)属 bdev-core 的 SD-9(v1.x)。
 *   6. 错误路径的 `-1` 仍是 `-1`(上游口径): 内部一律以 `rc < 0` 判定, 见 fat_internal.h。
 * ==========================================================================
 */
#define LOG_CATEGORY LOGC_FS

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>

#include "fat_internal.h"

/* ==================================================================== 卷表与 shim
 *
 * 上游是"全局当前设备 + 当前分区"(cur_dev/cur_part_info)。本件保留**同一形状**:
 * 一个"当前卷"指针(s_cur_vol), 于是移植代码里所有隐含的"当前卷"语义都不用改。
 * 挂载期是本插件单线程的一次性动作(start 相), 自检也跑在同一线程 ⇒ 不做锁;
 * 这是 cdev/bdev 同款的 v1 静态假设(设计 D22: 设备 API thread-only)。
 */

static uboot_fat_vol_t  s_vols[UB_FAT_VOL_MAX];
static uboot_fat_vol_t *s_cur_vol;      /* 上游的 cur_dev */

uboot_fat_vol_t *uboot_fat_vol_current(void)
{
    return s_cur_vol;
}

uboot_fat_vol_t *uboot_fat_vol_get(br_u32 id)
{
    if (id >= (br_u32)UB_FAT_VOL_MAX) {
        return BR_NULL;
    }
    return &s_vols[id];
}

/* 卷内扇区总数(写路径的 check_overflow 用它 —— 上游是静态 `total_sector`) */
u32 uboot_fat_vol_total_sect(void)
{
    return (s_cur_vol != BR_NULL) ? s_cur_vol->fs.total_sect : 0u;
}

u32 uboot_fat_sect_size(void)
{
    return (s_cur_vol != BR_NULL) ? (u32)s_cur_vol->fs.sect_size : 0u;
}

u32 uboot_fat_clust_bytes(void)
{
    if ((s_cur_vol == BR_NULL) || (s_cur_vol->fs.clust_size == 0u)) {
        return 0u;
    }
    return (u32)s_cur_vol->fs.clust_size * (u32)s_cur_vol->fs.sect_size;
}

/* 数据区簇数上界(FAT 表的**值域**; 不是"空闲簇数")。用于把所有扫描循环钳在表内。 */
u32 uboot_fat_max_clust(void)
{
    if ((s_cur_vol == BR_NULL) || (s_cur_vol->fs.fatsize == 0)) {
        return 0u;
    }
    /* 每簇字节数 = clust_size * sect_size ⇒ 簇数 = 总扇区 / 每簇扇区数 */
    return (u32)s_cur_vol->fs.total_sect / (u32)s_cur_vol->fs.clust_size;
}

/*
 * 一次**设备级** I/O 的公共骨架(write == 1 时写)。上游这里就是 `blk_dread/blk_dwrite`
 * 加一句"返回值 != 请求数 ⇒ 失败"; 本件多两件事:
 *   ① **按 geometry.max_rw 切块**(硬约束: 单次 I/O 不超过 bdev 的上限, 实测 64 扇区);
 *   ② 越界拒绝(整卷一个分区 ⇒ 合法 lba 区间是 [0, n_sectors))。
 * 成功返回传输的扇区数(== nr_blocks), 失败返回负 errno —— 于是上游的判据保持成立。
 */
static int uboot_fat_disk_rw(__u32 block, __u32 nr_blocks, void *buf, int write)
{
    br_u32 chunk;
    br_u64 lba;
    br_u32 left = nr_blocks;
    br_u8 *p    = (br_u8 *)buf;

    if (s_cur_vol == BR_NULL) {
        br_log_error("uboot-fat: disk_%s with no volume bound",
                     (write != 0) ? "write" : "read");
        return BR_ERR(BR_ENODEV);
    }
    if (nr_blocks == 0u) {
        return 0;
    }
    if (lba_of(block) >= s_cur_vol->geom.n_sectors) {
        br_log_error("uboot-fat: %s lba %u out of volume (n_sectors=%llu)",
                     (write != 0) ? "write" : "read", block,
                     (unsigned long long)s_cur_vol->geom.n_sectors);
        return BR_ERR(BR_EIO);
    }
    lba = lba_of(block);
    if ((lba + nr_blocks) > s_cur_vol->geom.n_sectors) {
        br_log_error("uboot-fat: %s %u sectors at lba %u crosses volume end",
                     (write != 0) ? "write" : "read", nr_blocks, block);
        return BR_ERR(BR_EIO);
    }
    if (s_cur_vol->ops == BR_NULL) {
        return BR_ERR(BR_ENODEV);
    }

    chunk = (s_cur_vol->geom.max_rw != 0u) ? s_cur_vol->geom.max_rw : nr_blocks;
    if (chunk > nr_blocks) {
        chunk = nr_blocks;
    }
    while (left > 0u) {
        const br_u32 n = (left < chunk) ? left : chunk;
        int rc;

        if (write != 0) {
            rc = s_cur_vol->ops->write(s_cur_vol->priv, lba, p, (br_size_t)n);
        } else {
            rc = s_cur_vol->ops->read(s_cur_vol->priv, lba, p, (br_size_t)n);
        }
        /* bdev 契约: 成功 = 传输扇区数(**允许短读**)。短了就当作设备错误 ——
         * 上游用 `ret != nr_blocks` 判定, 本件在切块层面就把它收敛掉。 */
        if (rc != (int)n) {
            br_log_error("uboot-fat: %s lba %llu n=%u rc=%d",
                         (write != 0) ? "write" : "read",
                         (unsigned long long)lba, n, rc);
            return (rc < 0) ? rc : BR_ERR(BR_EIO);
        }
        lba  += n;
        p    += (br_size_t)n * UB_FAT_SECTOR_SIZE;
        left -= n;
    }
    return (int)nr_blocks;
}

int disk_read(__u32 block, __u32 nr_blocks, void *buf)
{
    return uboot_fat_disk_rw(block, nr_blocks, buf, 0);
}

int disk_write(__u32 block, __u32 nr_blocks, void *buf)
{
    return uboot_fat_disk_rw(block, nr_blocks, buf, 1);
}

/* ==================================================================== 卷绑定(fat_set_blk_dev 的位置)
 *
 * 上游 `fat_set_blk_dev(dev_desc, info)`: 记下当前设备 + 校验引导扇区魔数 0x55AA。
 * 本件 `uboot_fat_vol_bind(vol)`: 取 bdev 几何 → 校验几何自洽 → 读 BPB 判 FAT 类型
 * (读在 fat_core.c 的 get_fs_info, 与上游同源) → 记下当前卷。
 * 返回 0 或负 errno; 失败时把卷恢复成"未绑定"并清掉当前卷(不留半绑定状态)。
 */
int uboot_fat_vol_bind(uboot_fat_vol_t *vol)
{
    int rc;

    if (vol == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (vol->bound == BR_TRUE) {
        /* 已绑定: 只把它切成当前卷(等价于上游 fat_set_blk_dev 的"换设备"语义)。
         * 它的 fs(fatbuf)仍然有效 —— 见 uboot_fat_get_fs_info 的惰性分配说明。 */
        s_cur_vol = vol;
        return 0;
    }

    vol->ops  = br_bdev_get(vol->dev, &vol->priv);
    vol->geom = (br_bdev_geom_t){ 0u, 0u, 0u, 0u, 0u };
    if ((vol->ops == BR_NULL) || (vol->priv == BR_NULL)) {
        br_log_error("uboot-fat: bdev %s not found", vol->dev);
        return BR_ERR(BR_ENODEV);
    }
    rc = vol->ops->geometry(vol->priv, &vol->geom);
    if (rc != 0) {
        br_log_error("uboot-fat: geometry(%s) failed (%d)", vol->dev, rc);
        return rc;
    }
    if ((vol->geom.sector_size != UB_FAT_SECTOR_SIZE) || (vol->geom.n_sectors == 0u)) {
        br_log_error("uboot-fat: %s sector_size=%u n_sectors=%llu (需要 512 B 扇区)",
                     vol->dev, vol->geom.sector_size,
                     (unsigned long long)vol->geom.n_sectors);
        return BR_ERR(BR_EINVAL);
    }

    s_cur_vol = vol;                     /* 后续所有 I/O 都落在这个卷上 */
    rc = uboot_fat_get_fs_info(&vol->fs);
    if (rc < 0) {
        s_cur_vol = BR_NULL;
        return rc;
    }
    vol->fs_valid = BR_TRUE;
    vol->bound    = BR_TRUE;
    return 0;
}

/* 解绑当前卷(把它的 FAT 缓存还给堆)。挂载失败的回滚路径也走它。 */
void uboot_fat_vol_unbind(void)
{
    if (s_cur_vol != BR_NULL) {
        uboot_fat_fatbuf_release(&s_cur_vol->fs);
        s_cur_vol->fs_valid = BR_FALSE;
        s_cur_vol->bound    = BR_FALSE;
    }
    s_cur_vol = BR_NULL;
}

/*
 * 挂载点 → 卷 id。**唯一**真值处: 挂载点与卷(设备)的对应关系写在这里, 而不是散在
 * mount 的调用点上。未知挂载点 ⇒ -ENOENT。
 */
int uboot_fat_mount_to_vol(const char *path, br_u32 *id)
{
    static const struct {
        const char *mnt;
        const char *dev;
    } table[UB_FAT_VOL_MAX] = {
        { UB_FAT_MNT12, UB_FAT_DEV12 },
        { UB_FAT_MNT16, UB_FAT_DEV16 },
        { UB_FAT_MNT32, UB_FAT_DEV32 },
    };
    br_u32 i;

    if ((path == BR_NULL) || (id == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    for (i = 0u; i < (br_u32)UB_FAT_VOL_MAX; i++) {
        if (uboot_fat_strcmp(table[i].mnt, path) == 0) {
            s_vols[i].dev = table[i].dev;
            s_vols[i].mnt = table[i].mnt;
            *id = i;
            return 0;
        }
    }
    return BR_ERR(BR_ENOENT);
}

/* 卷的事实观测(自检 TC-FAT-001 用): 只读, 不改任何状态。 */
int uboot_fat_vol_info(br_u32 id, uboot_fat_vol_info_t *out)
{
    uboot_fat_vol_t *v = uboot_fat_vol_get(id);

    if ((v == BR_NULL) || (out == BR_NULL)) {
        return BR_ERR(BR_EINVAL);
    }
    out->dev                 = v->dev;
    out->mnt                 = v->mnt;
    out->sector_size         = (v->bound == BR_TRUE) ? (br_u32)v->fs.sect_size : 0u;
    out->n_sectors           = (v->bound == BR_TRUE) ? (br_u64)v->fs.total_sect : 0u;
    out->max_rw              = v->geom.max_rw;
    out->clust_size_sectors  = (v->bound == BR_TRUE) ? (br_u32)v->fs.clust_size : 0u;
    out->max_clust           = (v->bound == BR_TRUE)
                             ? (u32)(v->fs.total_sect / (u32)v->fs.clust_size) : 0u;
    out->fatsize             = (v->bound == BR_TRUE) ? v->fs.fatsize : 0;
    out->mounted             = v->bound;
    return 0;
}

/* ==================================================================== 上游 fat.c 的代码(逐函数移植) */

/*
 * Convert a string to lowercase.  Converts at most 'len' characters,
 * 'len' may be larger than the length of 'str' if 'str' is NULL
 * terminated.
 *
 * 移植偏离: 形参 `size_t len` → `br_size_t len`(本件不 typedef size_t); 其余逐字一致。
 */
void downcase(char *str, br_size_t len)
{
    while (*str != '\0' && len--) {
        *str = (char)uboot_fat_tolower((unsigned char)*str);
        str++;
    }
}

#define DOS_BOOT_MAGIC_OFFSET   0x1fe
#define DOS_FS_TYPE_OFFSET      0x36
#define DOS_FS32_TYPE_OFFSET    0x52

/*
 * Extract zero terminated short name from a directory entry.
 * 移植偏离: `memcpy` 用 core 提供的同名符号(可用); `tolower` 走 uboot_fat_tolower。
 */
void get_name(dir_entry *dirent, char *s_name)
{
    char *ptr;

    memcpy(s_name, dirent->nameext.name, 8);
    s_name[8] = '\0';
    ptr = s_name;
    while (*ptr && *ptr != ' ')
        ptr++;
    if (dirent->lcase & CASE_LOWER_BASE)
        downcase(s_name, (br_size_t)(ptr - s_name));
    if (dirent->nameext.ext[0] && dirent->nameext.ext[0] != ' ') {
        *ptr++ = '.';
        memcpy(ptr, dirent->nameext.ext, 3);
        if (dirent->lcase & CASE_LOWER_EXT)
            downcase(ptr, 3);
        ptr[3] = '\0';
        while (*ptr && *ptr != ' ')
            ptr++;
    }
    *ptr = '\0';
    if (*s_name == DELETED_FLAG)
        *s_name = '\0';
    else if (*s_name == (char)aRING)
        *s_name = DELETED_FLAG;
}

/*
 * Get the entry at index 'entry' in a FAT (12/16/32) table.
 * On failure 0x00 is returned.
 *
 * ★ 上游缺陷(本件修掉, 见 NOTICE 的改写要点第 6 条): 上游把 `flush_dirty_fat_buffer()`
 *   的返回值丢掉了(它的失败条件是 `< 0`, 而函数返回 -1/负 errno) —— 那会让"回写 FAT
 *   失败"被静默忽略。本件改成显式失败。
 */
__u32 get_fatent(fsdata *mydata, __u32 entry)
{
    __u32 bufnum;
    __u32 offset, off8;
    __u32 ret = 0x00;

    if (CHECK_CLUST(entry, mydata->fatsize)) {
        log_err("Invalid FAT entry: %#08x", entry);
        return ret;
    }

    switch (mydata->fatsize) {
    case 32:
        bufnum = entry / uboot_fat_fat32_bufsize(mydata->sect_size);
        offset = entry - bufnum * uboot_fat_fat32_bufsize(mydata->sect_size);
        break;
    case 16:
        bufnum = entry / uboot_fat_fat16_bufsize(mydata->sect_size);
        offset = entry - bufnum * uboot_fat_fat16_bufsize(mydata->sect_size);
        break;
    case 12:
        bufnum = entry / uboot_fat_fat12_bufsize(mydata->sect_size);
        offset = entry - bufnum * uboot_fat_fat12_bufsize(mydata->sect_size);
        break;

    default:
        /* Unsupported FAT size */
        return ret;
    }

    debug("FAT%d: entry: 0x%08x = %d, offset: 0x%04x = %d",
          mydata->fatsize, entry, entry, offset, offset);

    /* Read a new block of FAT entries into the cache. */
    if (bufnum != (__u32)mydata->fatbufnum) {
        __u32 getsize = FATBUFBLOCKS;
        __u8 *bufptr = mydata->fatbuf;
        __u32 fatlength = mydata->fatlength;
        __u32 startblock = bufnum * FATBUFBLOCKS;

        /* Cap length if fatlength is not a multiple of FATBUFBLOCKS */
        if (startblock + getsize > fatlength)
            getsize = fatlength - startblock;

        startblock += mydata->fat_sect;  /* Offset from start of disk */

        /* Write back the fatbuf to the disk */
        if (flush_dirty_fat_buffer(mydata) < 0)
            return 0;

        if (getsize > FATBUFBLOCKS) {
            debug("getsize is too large for bufptr");
            getsize = FATBUFBLOCKS;
        }
        /* 偏离: 上游在这里可能拿着 getsize == 0 去调 disk_read(它的返回是 0,
         * 于是"读成功"—— 后面就会把过期缓存当成新块用)。本件显式拒绝。 */
        if (getsize == 0u) {
            log_err("FAT%d: entry %u beyond FAT length (%u sectors)",
                    mydata->fatsize, entry, fatlength);
            return 0;
        }

        if (disk_read(startblock, getsize, bufptr) < 0) {
            debug("Error reading FAT blocks");
            return ret;
        }
        mydata->fatbufnum = (int)bufnum;
    }

    /* Get the actual entry from the table */
    switch (mydata->fatsize) {
    case 32:
        ret = FAT2CPU32(((__u32 *) mydata->fatbuf)[offset]);
        break;
    case 16:
        ret = FAT2CPU16(((__u16 *) mydata->fatbuf)[offset]);
        break;
    case 12:
        off8 = (offset * 3) / 2;
        /* fatbut + off8 may be unaligned, read in byte granularity */
        ret = (__u32)(mydata->fatbuf[off8] + (mydata->fatbuf[off8 + 1] << 8));

        if (offset & 0x1)
            ret >>= 4;
        ret &= 0xfff;
        break;
    default:
        break;
    }
    debug("FAT%d: ret: 0x%08x, entry: 0x%08x, offset: 0x%04x",
          mydata->fatsize, ret, entry, offset);

    return ret;
}

/*
 * Read at most 'size' bytes from the specified cluster into 'buffer'.
 * Return 0 on success, -1 otherwise.
 *
 * 移植偏离: 上游"缓冲未按 ARCH_DMA_MINALIGN 对齐就逐扇区中转"的分支删掉(本件没有
 * DMA 约束, 见文件头); 大段读仍由 disk_read 一次完成(它内部按 max_rw 切块)。
 */
int get_cluster(fsdata *mydata, __u32 clustnum, __u8 *buffer, unsigned long size)
{
    __u32 startsect;
    int ret;

    if (clustnum > 0) {
        startsect = clust_to_sect(mydata, clustnum);
    } else {
        startsect = mydata->rootdir_sect;
    }

    debug("gc - clustnum: %d, startsect: %d", clustnum, startsect);

    if (size >= mydata->sect_size) {
        __u32 bytes_read;
        __u32 sect_count = (__u32)(size / mydata->sect_size);

        ret = disk_read(startsect, sect_count, buffer);
        if (ret != (int)sect_count) {
            debug("Error reading data (got %d)", ret);
            return -1;
        }
        bytes_read = sect_count * mydata->sect_size;
        startsect += sect_count;
        buffer += bytes_read;
        size -= bytes_read;
    }
    if (size) {
        /*
         * ★ 偏离(上游假设的修正): 上游在这里**直接**把一整扇区读进调用方的 `buffer`,
         * 于是隐含"buffer 至少有一扇区可写"这条前提。实盘上这条前提在两种调用下都不成立:
         *   · get_contents() 的"对齐块"传的是簇缓冲(够大) —— 没问题;
         *   · get_contents() 的"剩余字节"块传的是**调用方缓冲**(可能只有几字节, 例如
         *     消费者用 64 B 的栈缓冲读 12 B 的文件)—— 上游会把 512 B 写进去 ⇒ 越界。
         *   本件改成"读进本函数的扇区缓冲, 再拷贝 size 字节" —— 语义不变(仍然只给
         *   调用方 size 字节), 但不再有隐含的缓冲大小前提。
         */
        u8 tmpbuf[UB_FAT_SECTOR_SIZE];

        ret = disk_read(startsect, 1, tmpbuf);
        if (ret != 1) {
            debug("Error reading data (got %d)", ret);
            return -1;
        }
        memcpy(buffer, tmpbuf, size);
    }

    return 0;
}

/*
 * get_contents() - read from file
 *
 * Read at most 'maxsize' bytes from 'pos' in the file associated with 'dentptr'
 * into 'buffer'. Update the number of bytes read in *gotsize or return -1 on
 * fatal errors.
 *
 * 移植偏离: ① `malloc_cache_aligned` → `br_malloc`(失败面一样: NULL);
 *          ② `printf` → br_log_error(数据路径不"打印并继续");
 *          ③ 上游 `actsize` 在 do/while 里被 get_cluster 覆写(wsize 传的是 loff_t,
 *             形参是 unsigned long —— 在本件是同一个宽度, 语义不变)。
 */
int get_contents(fsdata *mydata, dir_entry *dentptr, loff_t pos,
                 __u8 *buffer, loff_t maxsize, loff_t *gotsize)
{
    loff_t filesize = FAT2CPU32(dentptr->size);
    unsigned int bytesperclust = mydata->clust_size * mydata->sect_size;
    __u32 curclust = START(dentptr);
    __u32 endclust, newclust;
    loff_t actsize;

    *gotsize = 0;
    debug("Filesize: %llu bytes", (unsigned long long)filesize);

    if (pos >= filesize) {
        debug("Read position past EOF: %llu", (unsigned long long)pos);
        return 0;
    }

    if (maxsize > 0 && filesize > pos + maxsize)
        filesize = pos + maxsize;

    debug("%llu bytes", (unsigned long long)filesize);

    actsize = bytesperclust;

    /* go to cluster at pos */
    while (actsize <= pos) {
        curclust = get_fatent(mydata, curclust);
        if (CHECK_CLUST(curclust, mydata->fatsize)) {
            debug("curclust: 0x%x", curclust);
            log_err("Invalid FAT entry");
            return -1;
        }
        actsize += bytesperclust;
    }

    /* actsize > pos */
    actsize -= bytesperclust;
    filesize -= actsize;
    pos -= actsize;

    /* align to beginning of next cluster if any */
    if (pos) {
        __u8 *tmp_buffer;

        actsize = min(filesize, (loff_t)bytesperclust);
        tmp_buffer = br_malloc((br_size_t)actsize);
        if (!tmp_buffer) {
            debug("Error: allocating buffer");
            return -1;
        }

        if (get_cluster(mydata, curclust, tmp_buffer, (unsigned long)actsize) != 0) {
            log_err("Error reading cluster");
            br_free(tmp_buffer);
            return -1;
        }
        filesize -= actsize;
        actsize -= pos;
        memcpy(buffer, tmp_buffer + pos, (br_size_t)actsize);
        br_free(tmp_buffer);
        *gotsize += actsize;
        if (!filesize)
            return 0;
        buffer += actsize;

        curclust = get_fatent(mydata, curclust);
        if (CHECK_CLUST(curclust, mydata->fatsize)) {
            debug("curclust: 0x%x", curclust);
            log_err("Invalid FAT entry");
            return -1;
        }
    }

    actsize = bytesperclust;
    endclust = curclust;

    do {
        /* search for consecutive clusters */
        while (actsize < filesize) {
            newclust = get_fatent(mydata, endclust);
            if ((newclust - 1) != endclust)
                goto getit;
            if (CHECK_CLUST(newclust, mydata->fatsize)) {
                debug("curclust: 0x%x", newclust);
                log_err("Invalid FAT entry");
                return -1;
            }
            endclust = newclust;
            actsize += bytesperclust;
        }

        /* get remaining bytes */
        actsize = filesize;
        if (get_cluster(mydata, curclust, buffer, (unsigned long)actsize) != 0) {
            log_err("Error reading cluster");
            return -1;
        }
        *gotsize += actsize;
        return 0;
getit:
        if (get_cluster(mydata, curclust, buffer, (unsigned long)actsize) != 0) {
            log_err("Error reading cluster");
            return -1;
        }
        *gotsize += actsize;
        filesize -= actsize;
        buffer += actsize;

        curclust = get_fatent(mydata, endclust);
        if (CHECK_CLUST(curclust, mydata->fatsize)) {
            debug("curclust: 0x%x", curclust);
            log_err("Invalid FAT entry");
            return -1;
        }
        actsize = bytesperclust;
        endclust = curclust;
    } while (1);
}

/*
 * Extract the file name information from 'slotptr' into 'l_name',
 * starting at l_name[*idx].
 * Return 1 if terminator (zero byte) is found, 0 otherwise.
 */
static int slot2str(dir_slot *slotptr, char *l_name, int *idx)
{
    int j;

    for (j = 0; j <= 8; j += 2) {
        l_name[*idx] = (char)slotptr->name0_4[j];
        if (l_name[*idx] == 0x00)
            return 1;
        (*idx)++;
    }
    for (j = 0; j <= 10; j += 2) {
        l_name[*idx] = (char)slotptr->name5_10[j];
        if (l_name[*idx] == 0x00)
            return 1;
        (*idx)++;
    }
    for (j = 0; j <= 2; j += 2) {
        l_name[*idx] = (char)slotptr->name11_12[j];
        if (l_name[*idx] == 0x00)
            return 1;
        (*idx)++;
    }

    return 0;
}

/* Calculate short name checksum */
__u8 mkcksum(struct nameext *nameext)
{
    int i;
    u8 *pos = (void *)nameext;

    __u8 ret = 0;

    for (i = 0; i < 11; i++)
        ret = (__u8)((((ret & 1) << 7) | ((ret & 0xfe) >> 1)) + pos[i]);

    return ret;
}

/*
 * Determine if the FAT type is FAT12 or FAT16
 *
 * Based on fat_fill_super() from the Linux kernel's fs/fat/inode.c
 *
 * 移植偏离: 上游用 u16/u32 的隐式提升混算; 本件显式取无符号 32 位并把 `bs->dir_entries`
 * 的**非对齐** 16 位读改成 get_unaligned_le16(上游在别处就是这么读的, 这里只是补齐)。
 */
static int determine_legacy_fat_bits(const boot_sector *bs)
{
    u16 fat_start = bs->reserved;
    u32 dir_start = (u32)fat_start + ((u32)bs->fats * (u32)bs->fat_length);
    u32 rootdir_sectors = ((u32)uboot_fat_get_unaligned_le16(bs->dir_entries) *
                           (u32)sizeof(dir_entry)) /
                          (u32)uboot_fat_get_unaligned_le16(bs->sector_size);
    u32 data_start = dir_start + rootdir_sectors;
    u16 sectors = uboot_fat_get_unaligned_le16(bs->sectors);
    u32 total_sectors = (sectors != 0u) ? (u32)sectors : bs->total_sect;
    u32 total_clusters;

    if (total_sectors <= data_start) {
        return 12;      /* 畸形 BPB: 交给上层的不变量检查去拒(见 uboot_fat_get_fs_info) */
    }
    total_clusters = (total_sectors - data_start) / (u32)bs->cluster_size;

    return (total_clusters > MAX_FAT12) ? 16 : 12;
}

/*
 * Determines if the boot sector's media field is valid
 *
 * Based on fat_valid_media() from Linux kernel's include/linux/msdos_fs.h
 */
static int fat_valid_media(u8 media)
{
    return media >= 0xf8 || media == 0xf0;
}

/*
 * Determines if the given boot sector is valid
 *
 * Based on fat_read_bpb() from the Linux kernel's fs/fat/inode.c
 */
static int is_bootsector_valid(const boot_sector *bs)
{
    u16 sector_size = uboot_fat_get_unaligned_le16(bs->sector_size);
    u16 dir_per_block = sector_size / (u16)sizeof(dir_entry);
    u16 dir_entries = uboot_fat_get_unaligned_le16(bs->dir_entries);

    if (!bs->reserved)
        return 0;

    if (!bs->fats)
        return 0;

    if (!fat_valid_media(bs->media))
        return 0;

    if (!uboot_fat_is_power_of_2(sector_size) ||
        sector_size < 512 ||
        sector_size > 4096)
        return 0;

    if (!uboot_fat_is_power_of_2(bs->cluster_size))
        return 0;

    if (!bs->fat_length && !bs->fat32_length)
        return 0;

    if ((dir_per_block == 0u) || (dir_entries & (dir_per_block - 1u)))
        return 0;

    return 1;
}

/*
 * Read boot sector and volume info from a FAT filesystem
 *
 * 移植偏离: 上游用 `cur_dev->blksz` 分配缓冲并读扇区 0; 本件的"设备"就是当前卷。
 */
static int read_bootsectandvi(boot_sector *bs, volume_info *volinfo, int *fatsize)
{
    __u8 *block;
    volume_info *vistart;
    int ret = 0;

    if (s_cur_vol == BR_NULL) {
        debug("Error: no device selected");
        return -1;
    }

    block = br_malloc(UB_FAT_SECTOR_SIZE);
    if (block == BR_NULL) {
        debug("Error: allocating block");
        return -1;
    }

    if (disk_read(0, 1, block) < 0) {
        debug("Error: reading block");
        ret = -1;
        goto out_free;
    }

    memcpy(bs, block, sizeof(boot_sector));
    bs->reserved = FAT2CPU16(bs->reserved);
    bs->fat_length = FAT2CPU16(bs->fat_length);
    bs->secs_track = FAT2CPU16(bs->secs_track);
    bs->heads = FAT2CPU16(bs->heads);
    bs->total_sect = FAT2CPU32(bs->total_sect);

    if (!is_bootsector_valid(bs)) {
        debug("Error: bootsector is invalid");
        ret = -1;
        goto out_free;
    }

    /* FAT32 entries */
    if (!bs->fat_length && bs->fat32_length) {
        /* Assume FAT32 */
        bs->fat32_length = FAT2CPU32(bs->fat32_length);
        bs->flags = FAT2CPU16(bs->flags);
        bs->root_cluster = FAT2CPU32(bs->root_cluster);
        bs->info_sector = FAT2CPU16(bs->info_sector);
        bs->backup_boot = FAT2CPU16(bs->backup_boot);
        vistart = (volume_info *)(block + sizeof(boot_sector));
        *fatsize = 32;
    } else {
        vistart = (volume_info *)&(bs->fat32_length);
        *fatsize = determine_legacy_fat_bits(bs);
    }
    memcpy(volinfo, vistart, sizeof(volume_info));

out_free:
    br_free(block);
    return ret;
}

/*
 * 卷的 FAT 参数(上游 `get_fs_info`)。
 *
 * ★ 本件对上游的一处**结构性偏离**(见 fat_internal.h 的卷模型说明):
 *   上游每个上层 API 都自带 `fsdata fsdata;`(用完 free), 于是"读一次文件"就 malloc/free
 *   一次 3 KiB 的 FAT 缓存; 本件把 fsdata 挂在**卷**上长期持有, 于是
 *     - 每次(重)识别前先 `uboot_fat_fatbuf_release()`(不泄漏, 且不会把旧缓存当新卷用);
 *     - **写路径跨调用不丢未回写的 FAT 修改**: 这是"新建文件 → 写 → 关 → 再读"在同一卷上
 *       保持一致的直接前提(fat_dirty 不能被下一次调用悄悄清掉)。
 *   代价是"每次识别重新读一遍引导扇区"(512 B), 相对 I/O 总量可忽略。
 */
int uboot_fat_get_fs_info(fsdata *mydata)
{
    boot_sector bs;
    volume_info volinfo;
    int ret;

    /* 调用者的作用域里必须有当前卷(上游同理: cur_dev/cur_part_info) */
    if (s_cur_vol == BR_NULL) {
        return BR_ERR(BR_ENODEV);
    }

    /*
     * ★ 越过会话边界的正确性: 这个函数会被反复调用(卷绑定、每次目录迭代的入口)。
     *   如果上一次调用留下的 FAT 窗口还是 dirty 的, **绝不能**直接把它 release 掉 ——
     *   那等于静默丢弃"刚刚分配的簇链"之类的修改, 后续操作会读到陈旧的 FAT。
     *   (实测踩到过: APP 的建/写序列之后紧跟本插件的用例时, 目录项与 FAT 会对不上。)
     */
    if ((mydata->fatbuf != BR_NULL) && (mydata->fat_dirty != 0)) {
        if (flush_dirty_fat_buffer(mydata) < 0) {
            log_err("FAT: cannot flush dirty FAT buffer before re-identify");
            return BR_ERR(BR_EIO);
        }
    }
    uboot_fat_fatbuf_release(mydata);
    memset(mydata, 0, sizeof(*mydata));
    mydata->fatbufnum = -1;
    mydata->sect_size = (__u16)UB_FAT_SECTOR_SIZE;

    ret = read_bootsectandvi(&bs, &volinfo, &mydata->fatsize);
    if (ret) {
        debug("Error: reading boot sector");
        return ret;
    }

    if (mydata->fatsize == 32) {
        mydata->fatlength = bs.fat32_length;
        mydata->total_sect = bs.total_sect;
    } else {
        mydata->fatlength = bs.fat_length;
        mydata->total_sect = uboot_fat_get_unaligned_le16(bs.sectors);
        if (!mydata->total_sect)
            mydata->total_sect = bs.total_sect;
    }
    if (!mydata->total_sect) { /* unlikely */
        mydata->total_sect = (u32)s_cur_vol->geom.n_sectors;
    }

    mydata->fats = bs.fats;
    mydata->fat_sect = bs.reserved;

    mydata->rootdir_sect = (__u32)mydata->fat_sect +
                           (mydata->fatlength * (__u32)bs.fats);

    mydata->sect_size = uboot_fat_get_unaligned_le16(bs.sector_size);
    mydata->clust_size = bs.cluster_size;
    if (mydata->sect_size != (__u16)s_cur_vol->geom.sector_size) {
        log_err("FAT sector size mismatch (fs=%u, dev=%u)",
                mydata->sect_size, (u32)s_cur_vol->geom.sector_size);
        return -1;
    }
    if (mydata->clust_size == 0) {
        log_err("FAT cluster size not set");
        return -1;
    }
    if ((unsigned int)mydata->clust_size * mydata->sect_size > UB_FAT_MAX_CLUSTSIZE) {
        log_err("FAT cluster size too big (cs=%u, max=%u)",
                (unsigned int)mydata->clust_size * mydata->sect_size,
                (unsigned int)UB_FAT_MAX_CLUSTSIZE);
        return -1;
    }

    if (mydata->fatsize == 32) {
        mydata->data_begin = (int)mydata->rootdir_sect -
                             (int)(mydata->clust_size * 2);
        mydata->root_cluster = bs.root_cluster;
    } else {
        mydata->rootdir_size = (int)((uboot_fat_get_unaligned_le16(bs.dir_entries) *
                                      (u32)sizeof(dir_entry)) /
                                     mydata->sect_size);
        mydata->data_begin = (int)mydata->rootdir_sect +
                             mydata->rootdir_size -
                             (int)(mydata->clust_size * 2);

        /*
         * The root directory is not cluster-aligned and may be on a
         * "negative" cluster, this will be handled specially in
         * fat_next_cluster().
         */
        mydata->root_cluster = 0;
    }

    /* FAT 缓存: 分配在本函数里(卷长期持有), 由 uboot_fat_fatbuf_release 归还 */
    mydata->fatbuf = br_malloc(uboot_fat_fat_bufsize(mydata->sect_size));
    if (mydata->fatbuf == BR_NULL) {
        debug("Error: allocating memory");
        return -1;
    }
    mydata->fatbufnum = -1;
    mydata->fat_dirty = 0;

    debug("FAT%d, fat_sect: %d, fatlength: %d",
          mydata->fatsize, mydata->fat_sect, mydata->fatlength);
    debug("Rootdir begins at cluster: %d, sector: %d, offset: %x; Data begins at: %d",
          mydata->root_cluster,
          mydata->rootdir_sect,
          mydata->rootdir_sect * mydata->sect_size, mydata->data_begin);
    debug("Sector size: %d, cluster size: %d",
          mydata->sect_size, mydata->clust_size);

    return 0;
}

/* 交还 FAT 缓存(幂等)。所有"销毁 fsdata / 重识别"的路径都必须经它。 */
void uboot_fat_fatbuf_release(fsdata *mydata)
{
    if ((mydata != BR_NULL) && (mydata->fatbuf != BR_NULL)) {
        br_free(mydata->fatbuf);
        mydata->fatbuf = BR_NULL;
    }
}

/* ==================================================================== 目录迭代器(上游 fat_itr) */

/**
 * fat_itr_root() - initialize an iterator to start at the root
 * directory
 *
 * ★ 本件的 fat_itr 不带 fsdata(它指向**卷**上的那一份) —— 上游的迭代器内嵌 fsdata,
 *   本件把它提到卷上, 于是"目录项的簇链"这件事在多个 API 调用之间是同一份状态。
 *   签名相应地从 `(itr, fsdata)` 收成 `(itr)`(见 NOTICE 的改写要点)。
 */
int fat_itr_root(fat_itr *itr)
{
    fsdata *fs = uboot_fat_fsdata();

    if (fs == BR_NULL)
        return -ENXIO;
    /*
     * ★ 偏离(上游每次入口都 `get_fs_info`): 本件的 fsdata 挂在**卷**上长期有效
     *   (见 uboot_fat_get_fs_info 的说明), 所以"已经有缓存"时不再重读 BPB。
     *   收益: ①少一次 512 B 的引导扇区 I/O; ②不会在操作之间重建缓存 ——
     *   "前一个操作留下的状态被后一个操作悄悄清掉"这类跨操作不一致因此不可能发生。
     *   BPB 在运行期不会变(本件不格式化), 所以复用是安全的。
     */
    if ((fs->fatbuf == BR_NULL) && (uboot_fat_get_fs_info(fs) != 0)) {
        return -ENXIO;
    }

    itr->fsdata = fs;
    itr->start_clust = fs->root_cluster;
    itr->clust = fs->root_cluster;
    itr->next_clust = fs->root_cluster;
    itr->dent = NULL;
    itr->remaining = 0;
    itr->last_cluster = 0;
    itr->is_root = 1;
    itr->name = NULL;
    itr->dent_start = NULL;
    itr->dent_rem = 0;
    itr->dent_clust = 0;
    itr->l_name[0] = '\0';
    itr->s_name[0] = '\0';

    return 0;
}

/**
 * fat_itr_child() - initialize an iterator to descend into a sub-
 * directory
 *
 * 移植偏离: 上游的 `assert(fat_itr_isdir(parent))` 换成"打印 + 立刻返回"
 * (无 libc 的 assert; 而"把文件当目录下推"在盘上损坏时真的会发生)。
 */
void fat_itr_child(fat_itr *itr, fat_itr *parent)
{
    fsdata *mydata = parent->fsdata;  /* for silly macros */
    unsigned clustnum = START(parent->dent);

    if (!fat_itr_isdir(parent)) {
        log_err("FAT: fat_itr_child on non-directory entry");
        return;
    }

    itr->fsdata = parent->fsdata;
    itr->start_clust = clustnum;
    if (clustnum > 0) {
        itr->clust = clustnum;
        itr->next_clust = clustnum;
        itr->is_root = 0;
    } else {
        itr->clust = parent->fsdata->root_cluster;
        itr->next_clust = parent->fsdata->root_cluster;
        itr->start_clust = parent->fsdata->root_cluster;
        itr->is_root = 1;
    }
    itr->dent = NULL;
    itr->remaining = 0;
    itr->last_cluster = 0;
    (void)mydata;
}

/**
 * fat_next_cluster() - load next FAT cluster
 *
 * The function is used when iterating through directories. It loads the
 * next cluster with directory entries
 */
void *fat_next_cluster(fat_itr *itr, unsigned int *nbytes)
{
    int ret;
    u32 sect;
    u32 read_size;

    /* have we reached the end? */
    if (itr->last_cluster)
        return NULL;

    if (itr->is_root && itr->fsdata->fatsize != 32) {
        /*
         * The root directory is located before the data area and
         * cannot be indexed using the regular unsigned cluster
         * numbers (it may start at a "negative" cluster or not at a
         * cluster boundary at all), so consider itr->next_clust to be
         * a offset in cluster-sized units from the start of rootdir.
         */
        unsigned sect_offset = itr->next_clust * itr->fsdata->clust_size;
        unsigned remaining_sects = (unsigned)itr->fsdata->rootdir_size - sect_offset;
        sect = itr->fsdata->rootdir_sect + sect_offset;
        /* do not read past the end of rootdir */
        read_size = min_t(u32, itr->fsdata->clust_size,
                          remaining_sects);
    } else {
        sect = clust_to_sect(itr->fsdata, itr->next_clust);
        read_size = itr->fsdata->clust_size;
    }

    log_debug("FAT read(sect=%d), clust_size=%d, read_size=%u",
              sect, itr->fsdata->clust_size, read_size);

    /*
     * NOTE: do_fat_read_at() had complicated logic to deal w/
     * vfat names that span multiple clusters in the fat16 case,
     * which get_dentfromdir() probably also needed (and was
     * missing).  And not entirely sure what fat32 didn't have
     * the same issue..  We solve that by only caring about one
     * dent at a time and iteratively constructing the vfat long
     * name.
     */
    ret = disk_read(sect, read_size, itr->block);
    if (ret < 0) {
        debug("Error: reading block");
        return NULL;
    }

    *nbytes = read_size * itr->fsdata->sect_size;
    itr->clust = itr->next_clust;
    if (itr->is_root && itr->fsdata->fatsize != 32) {
        itr->next_clust++;
        if (itr->next_clust * itr->fsdata->clust_size >=
            (unsigned)itr->fsdata->rootdir_size) {
            debug("nextclust: 0x%x", itr->next_clust);
            itr->last_cluster = 1;
        }
    } else {
        itr->next_clust = get_fatent(itr->fsdata, itr->next_clust);
        if (CHECK_CLUST(itr->next_clust, itr->fsdata->fatsize)) {
            debug("nextclust: 0x%x", itr->next_clust);
            itr->last_cluster = 1;
        }
    }

    return itr->block;
}

dir_entry *next_dent(fat_itr *itr)
{
    if (itr->remaining == 0) {
        unsigned nbytes;
        struct dir_entry *dent = fat_next_cluster(itr, &nbytes);

        /* have we reached the last cluster? */
        if (!dent) {
            /* a sign for no more entries left */
            itr->dent = NULL;
            return NULL;
        }

        itr->remaining = (int)(nbytes / sizeof(dir_entry)) - 1;
        itr->dent = dent;
    } else {
        itr->remaining--;
        itr->dent++;
    }

    /* have we reached the last valid entry? */
    if (itr->dent->nameext.name[0] == 0)
        return NULL;

    return itr->dent;
}

dir_entry *extract_vfat_name(fat_itr *itr)
{
    struct dir_entry *dent = itr->dent;
    int seqn = itr->dent->nameext.name[0] & ~LAST_LONG_ENTRY_MASK;
    u8 chksum, alias_checksum = ((dir_slot *)dent)->alias_checksum;
    int n = 0;

    while (seqn--) {
        char buf[13];
        int idx = 0;

        slot2str((dir_slot *)dent, buf, &idx);

        if (n + idx >= (int)sizeof(itr->l_name))
            return NULL;

        /* shift accumulated long-name up and copy new part in: */
        memmove(itr->l_name + idx, itr->l_name, (br_size_t)n);
        memcpy(itr->l_name, buf, (br_size_t)idx);

        n += idx;

        dent = next_dent(itr);
        if (!dent)
            return NULL;
    }

    /*
     * We are now at the short file name entry.
     * If it is marked as deleted, just skip it.
     */
    if (dent->nameext.name[0] == DELETED_FLAG ||
        dent->nameext.name[0] == (char)aRING)
        return NULL;

    itr->l_name[n] = '\0';

    chksum = mkcksum(&dent->nameext);

    /* checksum mismatch could mean deleted file, etc.. skip it: */
    if (chksum != alias_checksum) {
        debug("** chksum=%x, alias_checksum=%x, l_name=%s, s_name=%8s.%3s",
              chksum, alias_checksum, itr->l_name, dent->nameext.name,
              dent->nameext.ext);
        return NULL;
    }

    return dent;
}

int fat_itr_next(fat_itr *itr)
{
    dir_entry *dent;

    itr->name = NULL;

    /*
     * One logical directory entry consist of following slots:
     *			name[0]	Attributes
     *   dent[N - N]: LFN[N - 1]	N|0x40	ATTR_VFAT
     *   ...
     *   dent[N - 2]: LFN[1]	2	ATTR_VFAT
     *   dent[N - 1]: LFN[0]	1	ATTR_VFAT
     *   dent[N]:     SFN			ATTR_ARCH
     */
    while (1) {
        dent = next_dent(itr);
        if (!dent) {
            itr->dent_start = NULL;
            return 0;
        }
        itr->dent_rem = itr->remaining;
        itr->dent_start = itr->dent;
        itr->dent_clust = itr->clust;
        if (dent->nameext.name[0] == DELETED_FLAG)
            continue;

        if (dent->attr & ATTR_VOLUME) {
            if ((dent->attr & ATTR_VFAT) == ATTR_VFAT &&
                (dent->nameext.name[0] & LAST_LONG_ENTRY_MASK)) {
                /* long file name */
                dent = extract_vfat_name(itr);
                /*
                 * If succeeded, dent has a valid short file
                 * name entry for the current entry.
                 * If failed, itr points to a current bogus
                 * entry. So after fetching a next one,
                 * it may have a short file name entry
                 * for this bogus entry so that we can still
                 * check for a short name.
                 */
                if (!dent)
                    continue;
                itr->name = itr->l_name;
                break;
            } else {
                /* Volume label or VFAT entry, skip */
                continue;
            }
        }

        /* short file name */
        break;
    }

    get_name(dent, itr->s_name);
    if (!itr->name)
        itr->name = itr->s_name;

    return 1;
}

int fat_itr_isdir(fat_itr *itr)
{
    return !!(itr->dent->attr & ATTR_DIR);
}

/*
 * Helpers:
 */

/**
 * fat_itr_resolve() - traverse directory structure to resolve the
 * requested path.
 *
 * Traverse directory structure to the requested path.  If the specified
 * path is to a directory, this will descend into the directory and
 * leave it iterator at the start of the directory.  If the path is to a
 * file, it will leave the iterator in the parent directory with current
 * cursor at file's entry in the directory.
 */
int fat_itr_resolve(fat_itr *itr, const char *path, unsigned int type)
{
    const char *next;
    br_size_t plen = uboot_fat_strlen(path);

    /* chomp any extra leading slashes: */
    while (path[0] && ISDIRDELIM(path[0]))
        path++;

    /* are we at the end? */
    if (uboot_fat_strnlen(path, plen + 1u) == 0u) {
        if (!(type & TYPE_DIR))
            return -ENOENT;
        return 0;
    }

    /* find length of next path entry: */
    next = path;
    while (next[0] && !ISDIRDELIM(next[0]))
        next++;

    if (itr->is_root) {
        /* root dir doesn't have "." nor ".." */
        if ((((next - path) == 1) && !uboot_fat_strncmp(path, ".", 1)) ||
            (((next - path) == 2) && !uboot_fat_strncmp(path, "..", 2))) {
            /* point back to itself */
            itr->clust = itr->fsdata->root_cluster;
            itr->next_clust = itr->fsdata->root_cluster;
            itr->start_clust = itr->fsdata->root_cluster;
            itr->dent = NULL;
            itr->remaining = 0;
            itr->last_cluster = 0;

            if (next[0] == 0) {
                if (type & TYPE_DIR)
                    return 0;
                else
                    return -ENOENT;
            }

            return fat_itr_resolve(itr, next, type);
        }
    }

    while (fat_itr_next(itr)) {
        int match = 0;
        br_size_t n = max(uboot_fat_strlen(itr->name), (br_size_t)(next - path));

        /* check both long and short name: */
        if (!uboot_fat_strncasecmp(path, itr->name, n))
            match = 1;
        else if (itr->name != itr->s_name &&
                 !uboot_fat_strncasecmp(path, itr->s_name, n))
            match = 1;

        if (!match)
            continue;

        if (fat_itr_isdir(itr)) {
            /* recurse into directory: */
            fat_itr_child(itr, itr);
            return fat_itr_resolve(itr, next, type);
        } else if (next[0]) {
            /*
             * If next is not empty then we have a case
             * like: /path/to/realfile/nonsense
             */
            debug("bad trailing path: %s", next);
            return -ENOENT;
        } else if (!(type & TYPE_FILE)) {
            return -ENOTDIR;
        } else {
            return 0;
        }
    }

    return -ENOENT;
}

/* ==================================================================== 只读文件面 */

int fat_exists(const char *filename)
{
    fat_itr *itr;
    int ret;

    itr = br_malloc(sizeof(fat_itr));
    if (!itr)
        return 0;
    ret = fat_itr_root(itr);
    if (ret)
        goto out;

    ret = fat_itr_resolve(itr, filename, TYPE_ANY);
out:
    br_free(itr);
    return ret == 0;
}

int fat_size(const char *filename, loff_t *size)
{
    fat_itr *itr;
    int ret;

    itr = br_malloc(sizeof(fat_itr));
    if (!itr)
        return -ENOMEM;
    ret = fat_itr_root(itr);
    if (ret)
        goto out_free_itr;

    ret = fat_itr_resolve(itr, filename, TYPE_FILE);
    if (ret) {
        /*
         * Directories don't have size, but fs_size() is not
         * expected to fail if passed a directory path:
         */
        ret = fat_itr_root(itr);
        if (ret)
            goto out_free_itr;
        ret = fat_itr_resolve(itr, filename, TYPE_DIR);
        if (!ret)
            *size = 0;
        goto out_free_itr;
    }

    *size = FAT2CPU32(itr->dent->size);
out_free_itr:
    br_free(itr);
    return ret;
}

int fat_read_file(const char *filename, void *buf, loff_t offset, loff_t len,
                  loff_t *actread)
{
    fat_itr *itr;
    int ret;

    itr = br_malloc(sizeof(fat_itr));
    if (!itr)
        return -ENOMEM;
    ret = fat_itr_root(itr);
    if (ret)
        goto out_free_itr;

    ret = fat_itr_resolve(itr, filename, TYPE_FILE);
    if (ret)
        goto out_free_itr;

    debug("reading %s at pos %llu", filename, (unsigned long long)offset);

    /* For saving default max clustersize memory allocated to malloc pool */
    dir_entry *dentptr = itr->dent;

    ret = get_contents(itr->fsdata, dentptr, offset, buf, len, actread);

out_free_itr:
    br_free(itr);
    return ret;
}

int file_fat_read(const char *filename, void *buffer, int maxsize)
{
    loff_t actread;
    int ret;

    ret = fat_read_file(filename, buffer, 0, maxsize, &actread);
    if (ret)
        return ret;
    else
        return (int)actread;
}

/* ==================================================================== 目录流 */

int fat_opendir(const char *filename, struct fs_dir_stream **dirsp)
{
    fat_dir *dir;
    int ret;

    if (dirsp == BR_NULL) {
        return -EINVAL;
    }
    dir = br_malloc(sizeof(*dir));
    if (!dir)
        return -ENOMEM;
    memset(dir, 0, sizeof(*dir));

    dir->fsdata = uboot_fat_fsdata();     /* 借用卷上的参数(不拥有) */
    if (dir->fsdata == BR_NULL) {
        ret = -ENODEV;
        goto fail_free_dir;
    }
    ret = fat_itr_root(&dir->itr);
    if (ret)
        goto fail_free_dir;

    ret = fat_itr_resolve(&dir->itr, filename, TYPE_DIR);
    if (ret)
        goto fail_free_dir;

    *dirsp = (struct fs_dir_stream *)dir;
    return 0;

fail_free_dir:
    br_free(dir);
    return ret;
}

int fat_readdir(struct fs_dir_stream *dirs, struct fs_dirent **dentp)
{
    fat_dir *dir = (fat_dir *)dirs;
    struct fs_dirent *dent = &dir->dirent;

    if ((dirs == BR_NULL) || (dentp == BR_NULL)) {
        return -EINVAL;
    }
    if (!fat_itr_next(&dir->itr))
        return -ENOENT;

    memset(dent, 0, sizeof(*dent));
    uboot_fat_strcpy(dent->name, dir->itr.name, sizeof(dent->name));
    /* 偏离: 上游这里在 CONFIG_IS_ENABLED(EFI_LOADER) 下填 attr/三类时间戳;
     * 本件没有 RTC(v1), 也不给 EFI 用 ⇒ 只填 VFS 面真正消费的 {type, size}。 */
    if (fat_itr_isdir(&dir->itr)) {
        dent->type = FS_DT_DIR;
    } else {
        dent->type = FS_DT_REG;
        dent->size = FAT2CPU32(dir->itr.dent->size);
    }
    dent->attr = dir->itr.dent->attr;

    *dentp = dent;

    return 0;
}

void fat_closedir(struct fs_dir_stream *dirs)
{
    fat_dir *dir = (fat_dir *)dirs;

    /* 偏离: 上游要 free(dir->fsdata.fatbuf)(它的 fsdata 是迭代器私有的);
     * 本件的 fsdata 挂在**卷**上(见 uboot_fat_get_fs_info), 所以这里只还目录流本身。 */
    br_free(dir);
}

void fat_close(void)
{
    /* 上游是空实现(它靠 fs 层统一收尾)。本件保留同签名: 消费者(自检/未来 runtime)
     * 可能按上游习惯调它, 而"关掉所有卷"不在 v1 的语义里(unmount 槽位是 BR_NULL)。 */
}

/* ==================================================================== 卷上的 fsdata 取用
 *
 * 上游每个 API 自带 `fsdata fsdata;`(用完 free); 本件把它挂在卷上长期持有
 * (见 uboot_fat_get_fs_info 的惰性分配说明)。于是所有 API 都有同一份"当前卷的 FAT 参数",
 * 而调用者不再需要自己管理 3 KiB 的 FAT 缓存 —— 这也是"单卷任意深度的写序列"能保持
 * 一致(未回写的 FAT 修改不丢)的前提。
 */
fsdata *uboot_fat_fsdata(void)
{
    if ((s_cur_vol == BR_NULL) || (s_cur_vol->fs_valid == BR_FALSE)) {
        return BR_NULL;
    }
    return &s_cur_vol->fs;
}
