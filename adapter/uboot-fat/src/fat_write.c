// SPDX-License-Identifier: GPL-2.0+
/*
 * adapter/uboot-fat — FAT12/16/32 写路径(移植)+ 截断(本件新增)
 *
 * ============================== 移植来源与范围 ==============================
 *   上游: `nxp-imx/uboot-imx` 分支 `lf_v2026.04`, commit
 *         `bb41975aeb4cbd153b49c9fd62da1d64cd65968d`, 文件 `fs/fat/fat_write.c`
 *         (2179 行, GPL-2.0+, Copyright Donggeun Kim / Samsung)
 *   本文件: 该文件的**写路径整段**移植:
 *     · 名字:      str2fat / set_name / normalize_longname / str2slot / fill_dir_slot
 *     · FAT 表:    set_fatent_value / determine_fatent / clear_fatent / find_empty_cluster
 *     · 数据:      set_sectors / set_cluster / get_set_cluster / set_contents / check_overflow
 *     · 目录:      fat_move_to_cluster / fat_find_empty_dentries / new_dir_table / flush_dir /
 *                  fill_dentry / create_link / find_directory_entry / split_filename /
 *                  delete_single_dentry / delete_long_name / delete_dentry_link /
 *                  delete_dentry_long / fat_dir_entries / fat_itr_parent /
 *                  update_parent_dir_props / check_path_prefix
 *     · 入口:      file_fat_write_at / file_fat_write / fat_unlink / fat_mkdir / fat_rename
 *     · **新增**:  uboot_fat_truncate(上游没有, 见该函数头)
 *   上游把 fat.c 用 `#include "fat.c"` 接进同一个编译单元; 本件按本项目纪律拆成两个 TU,
 *   于是共享函数在 fat_internal.h 里声明(见那里的说明)。
 *
 * ============================== 有意偏离(相对上游) ==============================
 *   1. `strdup()` → **栈上有界缓冲**(ub_fat_path_copy)。无 libc, 而"文件路径"本来就有
 *      静态上界(UB_FAT_PATH_MAX = 256)。收益: 少了三处 malloc 失败路径, 也少了泄漏面。
 *   2. `rand()`(短名后缀) → 纯序号 i。**理由**: 上游注释说"用随机数加速搜索", 但候选名
 *      ~1..~9,10..~29 都是**确定性**的搜索空间(而且它自己就从 i 开始逐个试); 去掉 rand
 *      不改变"找一个不冲突的短名"这个契约, 却去掉了对一个本件没有的熵源的依赖。
 *   3. `malloc_cache_aligned()` → `br_malloc()`(堆对齐 ≥ 16 B); "缓冲未对齐就逐扇区中转"
 *      的分支删掉(本件没有 DMA; max_rw 由 shim 保证)。
 *   4. `assert()` → 显式判错 + 返回 `-EIO`。**上游的 assert 在 -ffreestanding 下不存在**,
 *      而"盘上数据畸形"真的会发生 —— 打死内核不是正确的答复。
 *   5. FAT 写入路径的 `printf(...)` → `br_log_error(...)` + 负 errno 上传。
 *   6. ★ **上游缺陷修复**(登记在 NOTICE): `set_contents()` 的"缩短文件"分支在
 *      `filesize == 0` 时会把整条簇链清零并 `return 0`(丢掉 `clear_fatent` 的错误),
 *      而调用路径上 `filesize = pos + maxsize` —— 于是"零长度写"(如 mkdir 内部的
 *      `set_contents(dotdent, 0, …, bytesperclust)`)会走到那里并**清掉刚分配的簇**。
 *      本件把 `maxsize == 0` 单独分流(见该函数头), 语义与上游**意图**一致而与上游
 *      **实现**不同 —— 这是本次移植里唯一一处行为差异, 特意写在最前面。
 *   7. `dentry_set_time()`(DM_RTC 分支) → 固定 2000-01-01(上游的无 RTC 兜底分支):
 *      v1 没有 RTC 设备, 而 ntpd/date 属 v2。
 *   8. `total_sector`(上游的静态全局) 保留同名静态, 由每个入口从**卷**的 fsdata 填充
 *      (`total_sector = uboot_fat_vol_total_sect()`), 语义与上游一致。
 *   9. `ub_fat_dbg` 取代表述性的 `debug`, 且短名拼装不再用 `sprintf`, 改用本件的
 *      `uboot_fat_snprintf`(fat_compat.h)。
 * ==========================================================================
 */
#define LOG_CATEGORY LOGC_FS

#include "fat_compat.h"    /* 适配层: u-boot 运行环境 → brickOS(唯一入口) */

#include "fat_internal.h"

/* 本文件的诊断口(与 fat_core.c 的 `debug` 同型; 这里换个名字免得与宏域混) */
#define ub_fat_dbg(fmt, ...) br_log_debug("uboot-fat: " fmt, ##__VA_ARGS__)

static int new_dir_table(fat_itr *itr);

/* Characters that may only be used in long file names */
static const char LONG_ONLY_CHARS[] = "+,;=[]";

/**
 * str2fat() - convert string to valid FAT name characters
 *
 * Stop when reaching end of @src or a period.
 * Ignore spaces.
 * Replace characters that may only be used in long names by underscores.
 * Convert lower case characters to upper case.
 *
 * To avoid assumptions about the code page we do not use characters
 * above 0x7f for the short name.
 */
static int str2fat(char *dest, char *src, int length)
{
    int i;

    for (i = 0; i < length; ++src) {
        char c = *src;

        if (!c || c == '.')
            break;
        if (c == ' ')
            continue;
        if (uboot_fat_strchr(LONG_ONLY_CHARS, c) || c > 0x7f)
            c = '_';
        else if (c >= 'a' && c <= 'z')
            c &= (char)0xdf;
        dest[i] = c;
        ++i;
    }
    return i;
}

/**
 * fat_move_to_cluster() - position to first directory entry in cluster
 *
 * @itr:	directory iterator
 * @cluster	cluster
 * Return:	0 for success, -EIO on error
 */
int fat_move_to_cluster(fat_itr *itr, unsigned int cluster)
{
    unsigned int nbytes;

    /* position to the start of the directory */
    itr->next_clust = cluster;
    itr->last_cluster = 0;
    if (!fat_next_cluster(itr, &nbytes))
        return -EIO;
    itr->dent = (dir_entry *)itr->block;
    itr->remaining = (int)(nbytes / sizeof(dir_entry)) - 1;
    return 0;
}

/**
 * set_name() - set short name in directory entry
 *
 * The function determines if the @filename is a valid short name.
 * In this case no long name is needed.
 *
 * If a long name is needed, a short name is constructed.
 */
static int set_name(fat_itr *itr, const char *filename, char *shortname)
{
    char *period;
    char *pos;
    int period_location;
    char buf[13];
    int i;
    int ret;
    struct nameext dirent;

    if (!filename)
        return -EIO;

    /* Initialize buffer */
    memset(&dirent, ' ', sizeof(dirent));

    /* Convert filename to upper case short name */
    period = uboot_fat_strrchr(filename, '.');
    pos = (char *)(br_uintptr_t)filename;
    if (*pos == '.') {
        pos = period + 1;
        period = 0;
    }
    if (period)
        (void)str2fat(dirent.ext, period + 1, (int)sizeof(dirent.ext));
    period_location = str2fat(dirent.name, pos, (int)sizeof(dirent.name));
    if (period_location < 0)
        return period_location;
    if (*dirent.name == ' ')
        *dirent.name = '_';
    /* Substitute character 0xe5 signaling deletetion by character 0x05 */
    if (*dirent.name == DELETED_FLAG)
        *dirent.name = (char)aRING;

    /* If filename and short name are the same, quit. */
    (void)uboot_fat_snprintf(buf, sizeof(buf), "%.*s.%.3s",
                             period_location, dirent.name, dirent.ext);
    if (!uboot_fat_strncmp(buf, filename, uboot_fat_strlen(filename))) {
        ret = 1;
        goto out;
    } else if (!uboot_fat_strcasecmp(buf, filename)) {
        goto out_ret;
    }

    /* Construct an indexed short name.
     * 偏离: 上游在这里用 rand() 挑起点(见文件头第 2 条); 本件用纯序号。 */
    for (i = 1; i < 0x200000; ++i) {
        int suffix_len;
        int suffix_start;

        (void)uboot_fat_snprintf(buf, sizeof(buf), "~%d", i);
        suffix_len = (int)uboot_fat_strlen(buf);
        suffix_start = 8 - suffix_len;
        if (suffix_start > period_location)
            suffix_start = period_location;
        memcpy(dirent.name + suffix_start, buf, (br_size_t)suffix_len);
        if (*dirent.ext != ' ')
            (void)uboot_fat_snprintf(buf, sizeof(buf), "%.*s.%.3s",
                                     suffix_start + suffix_len, dirent.name, dirent.ext);
        else
            (void)uboot_fat_snprintf(buf, sizeof(buf), "%.*s",
                                     suffix_start + suffix_len, dirent.name);
        ub_fat_dbg("generated short name: %s", buf);

        /* Check that the short name does not exist yet. */
        ret = fat_move_to_cluster(itr, itr->start_clust);
        if (ret)
            return ret;
        if (find_directory_entry(itr, buf))
            continue;

        goto out_ret;
    }
    return -EIO;
out_ret:
    ub_fat_dbg("chosen short name: %s", buf);
    /* Each long name directory entry takes 13 characters. */
    ret = (int)((uboot_fat_strlen(filename) + 25u) / 13u);
out:
    memcpy(shortname, &dirent, SHORT_NAME_SIZE);
    return ret;
}

/*
 * 卷内扇区总数。上游是 `static int total_sector;` 由各入口从 `datablock.total_sect` 填;
 * 本件的卷事实在**卷**上, 于是每个入口写 `total_sector = uboot_fat_vol_total_sect();`。
 */
static int total_sector;

/**
 * flush_dirty_fat_buffer() - 把 FAT 缓存回写设备
 *
 * ★ 上游缺陷修复: `mydata->fats == 2` 时上游只回写"第 2 份 FAT"的**同一段**;
 *   本件同样处理两份(语义一致); 但上游把两处的 `disk_write` 返回值判成 `< 0`
 *   —— 本件沿用(shim 成功恒返回请求的扇区数)。
 */
int flush_dirty_fat_buffer(fsdata *mydata)
{
    int getsize = FATBUFBLOCKS;
    __u32 fatlength = mydata->fatlength;
    __u8 *bufptr = mydata->fatbuf;
    __u32 startblock = (__u32)mydata->fatbufnum * FATBUFBLOCKS;

    ub_fat_dbg("debug: evicting %d, dirty: %d", mydata->fatbufnum,
               (int)mydata->fat_dirty);

    if ((!mydata->fat_dirty) || (mydata->fatbufnum == -1))
        return 0;

    /* Cap length if fatlength is not a multiple of FATBUFBLOCKS */
    if (startblock + (__u32)getsize > fatlength)
        getsize = (int)(fatlength - startblock);

    startblock += mydata->fat_sect;

    /* Write FAT buf */
    if (disk_write(startblock, (__u32)getsize, bufptr) < 0) {
        ub_fat_dbg("error: writing FAT blocks");
        return -1;
    }

    if (mydata->fats == 2) {
        /* Update corresponding second FAT blocks */
        startblock += mydata->fatlength;
        if (disk_write(startblock, (__u32)getsize, bufptr) < 0) {
            ub_fat_dbg("error: writing second FAT blocks");
            return -1;
        }
    }
    mydata->fat_dirty = 0;

    return 0;
}

/**
 * fat_find_empty_dentries() - find a sequence of available directory entries
 *
 * @itr:	directory iterator
 * @count:	number of directory entries to find
 * Return:	0 on success or negative error number
 */
static int fat_find_empty_dentries(fat_itr *itr, int count)
{
    unsigned int cluster = 0;
    dir_entry *dent = NULL;
    int remaining = 0;
    unsigned int n = 0;
    int ret;

    ret = fat_move_to_cluster(itr, itr->start_clust);
    if (ret)
        return ret;

    for (;;) {
        if (!itr->dent) {
            log_debug("Not enough directory entries available");
            return -ENOSPC;
        }
        switch (itr->dent->nameext.name[0]) {
        case 0x00:
        case DELETED_FLAG:
            if (!n) {
                /* Remember first deleted directory entry */
                cluster = itr->clust;
                dent = itr->dent;
                remaining = itr->remaining;
            }
            ++n;
            if (n == (unsigned int)count)
                goto out;
            break;
        default:
            n = 0;
            break;
        }

        next_dent(itr);
        if (!itr->dent &&
            (!itr->is_root || itr->fsdata->fatsize == 32) &&
            new_dir_table(itr))
            return -ENOSPC;
    }
out:
    /* Position back to first directory entry */
    if (itr->clust != cluster) {
        ret = fat_move_to_cluster(itr, cluster);
        if (ret)
            return ret;
    }
    itr->dent = dent;
    itr->remaining = remaining;
    return 0;
}

/*
 * Set the file name information from 'name' into 'slotptr',
 */
static int str2slot(dir_slot *slotptr, const char *name, int *idx)
{
    int j, end_idx = 0;

    for (j = 0; j <= 8; j += 2) {
        if (name[*idx] == 0x00) {
            slotptr->name0_4[j] = 0;
            slotptr->name0_4[j + 1] = 0;
            end_idx++;
            goto name0_4;
        }
        slotptr->name0_4[j] = (__u8)name[*idx];
        (*idx)++;
        end_idx++;
    }
    for (j = 0; j <= 10; j += 2) {
        if (name[*idx] == 0x00) {
            slotptr->name5_10[j] = 0;
            slotptr->name5_10[j + 1] = 0;
            end_idx++;
            goto name5_10;
        }
        slotptr->name5_10[j] = (__u8)name[*idx];
        (*idx)++;
        end_idx++;
    }
    for (j = 0; j <= 2; j += 2) {
        if (name[*idx] == 0x00) {
            slotptr->name11_12[j] = 0;
            slotptr->name11_12[j + 1] = 0;
            end_idx++;
            goto name11_12;
        }
        slotptr->name11_12[j] = (__u8)name[*idx];
        (*idx)++;
        end_idx++;
    }

    if (name[*idx] == 0x00)
        return 1;

    return 0;
/* Not used characters are filled with 0xff 0xff */
name0_4:
    for (; end_idx < 5; end_idx++) {
        slotptr->name0_4[end_idx * 2] = 0xff;
        slotptr->name0_4[end_idx * 2 + 1] = 0xff;
    }
    end_idx = 5;
name5_10:
    end_idx -= 5;
    for (; end_idx < 6; end_idx++) {
        slotptr->name5_10[end_idx * 2] = 0xff;
        slotptr->name5_10[end_idx * 2 + 1] = 0xff;
    }
    end_idx = 11;
name11_12:
    end_idx -= 11;
    for (; end_idx < 2; end_idx++) {
        slotptr->name11_12[end_idx * 2] = 0xff;
        slotptr->name11_12[end_idx * 2 + 1] = 0xff;
    }

    return 1;
}

/**
 * fill_dir_slot() - fill directory entries for long name
 *
 * @itr:	directory iterator
 * @l_name:	long name
 * @shortname:	short name
 * Return:	0 for success, -errno otherwise
 *
 * 偏离: 上游的长名槽缓冲放在**栈上**(`MAX_LFN_SLOT * sizeof(dir_slot)` = 640 B);
 * 本件用 br_malloc —— 不是怕栈, 而是对齐与"失败面显式"两条与别处一致。
 */
static int fill_dir_slot(fat_itr *itr, const char *l_name, const char *shortname)
{
    __u8 *temp_dir_slot_buffer;
    dir_slot *slotptr;
    __u8 counter = 0, checksum;
    int idx = 0, ret;

    temp_dir_slot_buffer = br_malloc((br_size_t)UB_FAT_LFN_SLOT_MAX);
    if (temp_dir_slot_buffer == BR_NULL) {
        return -ENOMEM;
    }
    slotptr = (dir_slot *)temp_dir_slot_buffer;

    /* Get short file name checksum value */
    checksum = mkcksum((void *)(br_uintptr_t)shortname);

    do {
        memset(slotptr, 0x00, sizeof(dir_slot));
        ret = str2slot(slotptr, l_name, &idx);
        slotptr->id = ++counter;
        slotptr->attr = ATTR_VFAT;
        slotptr->alias_checksum = checksum;
        slotptr++;
    } while (ret == 0);

    slotptr--;
    slotptr->id |= LAST_LONG_ENTRY_MASK;

    while (counter >= 1) {
        memcpy(itr->dent, slotptr, sizeof(dir_slot));
        slotptr--;
        counter--;

        if (!itr->remaining) {
            /* Write directory table to device */
            ret = flush_dir(itr);
            if (ret) {
                br_free(temp_dir_slot_buffer);
                return ret;
            }
        }

        next_dent(itr);
        if (!itr->dent) {
            br_free(temp_dir_slot_buffer);
            return -EIO;
        }
    }

    br_free(temp_dir_slot_buffer);
    return 0;
}

/*
 * Set the entry at index 'entry' in a FAT (12/16/32) table.
 */
int set_fatent_value(fsdata *mydata, __u32 entry, __u32 entry_value)
{
    __u32 bufnum, offset, off16;
    __u16 val1, val2;

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
        return -1;
    }

    /* Read a new block of FAT entries into the cache. */
    if (bufnum != (__u32)mydata->fatbufnum) {
        __u32 getsize = FATBUFBLOCKS;
        __u8 *bufptr = mydata->fatbuf;
        __u32 fatlength = mydata->fatlength;
        __u32 startblock = bufnum * FATBUFBLOCKS;

        /* Cap length if fatlength is not a multiple of FATBUFBLOCKS */
        if (startblock + getsize > fatlength)
            getsize = fatlength - startblock;

        if (flush_dirty_fat_buffer(mydata) < 0)
            return -1;

        startblock += mydata->fat_sect;

        /* 偏离: 上游可能拿着 getsize == 0 去读(它返回 0 = "成功"), 本件显式拒绝。
         * 与 fat_core.c 的 get_fatent 同一处补丁, 理由相同。 */
        if (getsize == 0u || getsize > FATBUFBLOCKS) {
            log_err("FAT%d: entry %u beyond FAT length (%u sectors)",
                    mydata->fatsize, entry, fatlength);
            return -1;
        }

        if (disk_read(startblock, getsize, bufptr) < 0) {
            ub_fat_dbg("Error reading FAT blocks");
            return -1;
        }
        mydata->fatbufnum = (int)bufnum;
    }

    /* Mark as dirty */
    mydata->fat_dirty = 1;

    /* Set the actual entry */
    switch (mydata->fatsize) {
    case 32:
        ((__u32 *) mydata->fatbuf)[offset] = uboot_fat_cpu_to_le32(entry_value);
        break;
    case 16:
        ((__u16 *) mydata->fatbuf)[offset] = uboot_fat_cpu_to_le16((__u16)entry_value);
        break;
    case 12:
        off16 = (offset * 3) / 4;

        switch (offset & 0x3) {
        case 0:
            val1 = (__u16)(uboot_fat_cpu_to_le16((__u16)entry_value) & 0xfff);
            ((__u16 *)mydata->fatbuf)[off16] &= (__u16)~0xfff;
            ((__u16 *)mydata->fatbuf)[off16] |= val1;
            break;
        case 1:
            val1 = (__u16)(uboot_fat_cpu_to_le16((__u16)entry_value) & 0xf);
            val2 = (__u16)((uboot_fat_cpu_to_le16((__u16)entry_value) >> 4) & 0xff);

            ((__u16 *)mydata->fatbuf)[off16] &= (__u16)~0xf000;
            ((__u16 *)mydata->fatbuf)[off16] |= (__u16)(val1 << 12);

            ((__u16 *)mydata->fatbuf)[off16 + 1] &= (__u16)~0xff;
            ((__u16 *)mydata->fatbuf)[off16 + 1] |= val2;
            break;
        case 2:
            val1 = (__u16)(uboot_fat_cpu_to_le16((__u16)entry_value) & 0xff);
            val2 = (__u16)((uboot_fat_cpu_to_le16((__u16)entry_value) >> 8) & 0xf);

            ((__u16 *)mydata->fatbuf)[off16] &= (__u16)~0xff00;
            ((__u16 *)mydata->fatbuf)[off16] |= (__u16)(val1 << 8);

            ((__u16 *)mydata->fatbuf)[off16 + 1] &= (__u16)~0xf;
            ((__u16 *)mydata->fatbuf)[off16 + 1] |= val2;
            break;
        case 3:
            val1 = (__u16)(uboot_fat_cpu_to_le16((__u16)entry_value) & 0xfff);
            ((__u16 *)mydata->fatbuf)[off16] &= (__u16)~0xfff0;
            ((__u16 *)mydata->fatbuf)[off16] |= (__u16)(val1 << 4);
            break;
        default:
            break;
        }

        break;
    default:
        return -1;
    }

    return 0;
}

/*
 * Determine the next free cluster after 'entry' in a FAT (12/16/32) table
 * and link it to 'entry'. EOC marker is not set on returned entry.
 *
 * 偏离: 上游的 `while (1)` 在卷满时**永不返回**(它会一直读 FAT 表末端之外的值,
 * 而那些值恰好是 0, 于是它会一直"链接"到不存在的簇上并写坏卷)。本件加簇号上界
 * (由卷几何算出), 越界 ⇒ 返回 0 表示"没有空闲簇"(调用方一律按 -ENOSPC 处理)。
 * 同一条补丁也打在 find_empty_cluster 上(见那里)。
 */
__u32 determine_fatent(fsdata *mydata, __u32 entry)
{
    __u32 next_fat, next_entry = entry + 1;
    const __u32 max_clust = uboot_fat_max_clust();

    while (1) {
        if (next_entry >= max_clust) {
            log_err("FAT%d: no free cluster after %u (max %u)",
                    mydata->fatsize, entry, max_clust);
            return 0;
        }
        next_fat = get_fatent(mydata, next_entry);
        if (next_fat == 0) {
            /* found free entry, link to entry */
            set_fatent_value(mydata, entry, next_entry);
            break;
        }
        next_entry++;
    }
    ub_fat_dbg("FAT%d: entry: %08x, entry_value: %04x",
               mydata->fatsize, entry, next_entry);

    return next_entry;
}

/**
 * set_sectors() - write data to sectors
 *
 * Write 'size' bytes from 'buffer' into the specified sector.
 *
 * 偏离: 上游"缓冲未对齐就逐扇区中转"的分支删掉(本件没有 DMA 约束);
 * 单次 I/O 的扇区上界由 disk_write 内部按 max_rw 切块保证。
 */
int set_sectors(fsdata *mydata, u32 startsect, u8 *buffer, u32 size)
{
    int ret;

    ub_fat_dbg("startsect: %d", startsect);

    if (size >= mydata->sect_size) {
        u32 nsects;

        nsects = size / mydata->sect_size;
        ret = disk_write(startsect, nsects, buffer);
        if (ret != (int)nsects) {
            ub_fat_dbg("Error writing data (got %d)", ret);
            return -1;
        }

        startsect += nsects;
        buffer += nsects * mydata->sect_size;
        size -= nsects * mydata->sect_size;
    }

    if (size) {
        /* Do not leak content of stack: 上游用栈上 tmpbuf 并先清零; 本件直接改
         * `buffer` 之后的那一段?——不: 尾段必须"补齐整扇区", 所以仍然需要一个
         * 扇区缓冲。它是**局部**的(512 B), 与上游一致。 */
        u8 tmpbuf[UB_FAT_SECTOR_SIZE];

        memset(tmpbuf, 0, mydata->sect_size);
        memcpy(tmpbuf, buffer, size);
        ret = disk_write(startsect, 1, tmpbuf);
        if (ret != 1) {
            ub_fat_dbg("Error writing data (got %d)", ret);
            return -1;
        }
    }

    return 0;
}

/**
 * set_cluster() - write data to cluster
 *
 * Write 'size' bytes from 'buffer' into the specified cluster.
 */
int set_cluster(fsdata *mydata, u32 clustnum, u8 *buffer, u32 size)
{
    return set_sectors(mydata, clust_to_sect(mydata, clustnum),
                       buffer, size);
}

/**
 * flush_dir() - flush directory
 *
 * @itr:	directory iterator
 * Return:	0 for success, -EIO on error
 */
int flush_dir(fat_itr *itr)
{
    fsdata *mydata = itr->fsdata;
    u32 startsect, sect_offset, nsects;
    int ret;

    if (!itr->is_root || mydata->fatsize == 32) {
        ret = set_cluster(mydata, itr->clust, itr->block,
                          mydata->clust_size * mydata->sect_size);
        goto out;
    }

    sect_offset = itr->clust * mydata->clust_size;
    startsect = mydata->rootdir_sect + sect_offset;
    /* do not write past the end of rootdir */
    nsects = min_t(u32, mydata->clust_size,
                   (u32)mydata->rootdir_size - sect_offset);

    ret = set_sectors(mydata, startsect, itr->block,
                      nsects * mydata->sect_size);
out:
    if (ret) {
        log_err("Error: writing directory entry");
        return -EIO;
    }
    return 0;
}

/*
 * Read and modify data on existing and consecutive cluster blocks
 *
 * 偏离: 上游用 `static u8 *tmpbuf_cluster`(**永不释放**的惰性全局缓冲)。本件在栈上放
 * 一个簇大小的缓冲(UB_FAT_MAX_CLUSTSIZE = 32 KiB)—— 写路径不是热路径, 而"静态 32 KiB
 * 缓冲 + 永不归还"会让"每个插件的堆/静态占用"这笔账说不清(本插件的 [[res]] 是按
 * 运行期峰值报的)。代价: 这个函数一帧 32 KiB 栈 —— 调用链深度固定(见 README 的欠债)。
 */
static int
get_set_cluster(fsdata *mydata, __u32 clustnum, loff_t pos, __u8 *buffer,
                loff_t size, loff_t *gotsize)
{
    unsigned int bytesperclust = mydata->clust_size * mydata->sect_size;
    __u32 startsect;
    loff_t clustcount, wsize;
    int ret;
    u8 tmpbuf_cluster[UB_FAT_MAX_CLUSTSIZE];

    *gotsize = 0;
    if (!size)
        return 0;

    if (pos >= (loff_t)bytesperclust) {   /* 上游是 assert(pos < bytesperclust) */
        log_err("FAT: write offset %llu beyond cluster (%u)",
                (unsigned long long)pos, bytesperclust);
        return -1;
    }
    startsect = clust_to_sect(mydata, clustnum);

    ub_fat_dbg("clustnum: %d, startsect: %d, pos: %lld",
               clustnum, startsect, (long long)pos);

    /* partial write at beginning */
    if (pos) {
        wsize = min((loff_t)bytesperclust - pos, size);
        ret = disk_read(startsect, mydata->clust_size, tmpbuf_cluster);
        if (ret != (int)mydata->clust_size) {
            ub_fat_dbg("Error reading data (got %d)", ret);
            return -1;
        }

        memcpy(tmpbuf_cluster + pos, buffer, (br_size_t)wsize);
        ret = disk_write(startsect, mydata->clust_size, tmpbuf_cluster);
        if (ret != (int)mydata->clust_size) {
            ub_fat_dbg("Error writing data (got %d)", ret);
            return -1;
        }

        size -= wsize;
        buffer += wsize;
        *gotsize += wsize;

        startsect += mydata->clust_size;

        if (!size)
            return 0;
    }

    /* full-cluster write */
    if (size >= (loff_t)bytesperclust) {
        clustcount = size / (loff_t)bytesperclust;

        wsize = clustcount * (loff_t)bytesperclust;
        /* 上游这里按缓冲对齐分两支; 本件只有一支(见文件头)。 */
        if (clustcount > (loff_t)(0x7fffffff / (int)mydata->clust_size)) {
            log_err("FAT: cluster count too large (%lld)", (long long)clustcount);
            return -1;
        }
        ret = disk_write(startsect,
                         (__u32)(clustcount * mydata->clust_size),
                         buffer);
        if (ret != (int)(clustcount * mydata->clust_size)) {
            ub_fat_dbg("Error writing data (got %d)", ret);
            return -1;
        }

        size -= wsize;
        buffer += wsize;
        *gotsize += wsize;

        startsect += (__u32)(clustcount * mydata->clust_size);
    }

    /* partial write at end */
    if (size) {
        wsize = size;
        ret = disk_read(startsect, mydata->clust_size, tmpbuf_cluster);
        if (ret != (int)mydata->clust_size) {
            ub_fat_dbg("Error reading data (got %d)", ret);
            return -1;
        }
        memcpy(tmpbuf_cluster, buffer, (br_size_t)wsize);
        ret = disk_write(startsect, mydata->clust_size, tmpbuf_cluster);
        if (ret != (int)mydata->clust_size) {
            ub_fat_dbg("Error writing data (got %d)", ret);
            return -1;
        }

        size -= wsize;
        *gotsize += wsize;
    }

    if (size) {                            /* 上游是 assert(!size) */
        log_err("FAT: %lld bytes left unwritten", (long long)size);
        return -1;
    }

    return 0;
}

/*
 * Find the first empty cluster
 *
 * 偏离: 上游的 `while (1)` 在卷满时永不返回(同 determine_fatent)。本件加上界,
 * 越界返回 0(0 不是合法数据簇 ⇒ 调用方必须当错误处理)。
 */
__u32 find_empty_cluster(fsdata *mydata)
{
    __u32 fat_val, entry = 3;
    const __u32 max_clust = uboot_fat_max_clust();

    while (1) {
        if (entry >= max_clust) {
            log_err("FAT%d: no empty cluster (max %u)", mydata->fatsize, max_clust);
            return 0;
        }
        fat_val = get_fatent(mydata, entry);
        if (fat_val == 0)
            break;
        entry++;
    }

    return entry;
}

/**
 * new_dir_table() - allocate a cluster for additional directory entries
 *
 * @itr:	directory iterator
 * Return:	0 on success, -EIO otherwise
 */
static int new_dir_table(fat_itr *itr)
{
    fsdata *mydata = itr->fsdata;
    int dir_newclust = 0;
    int dir_oldclust = (int)itr->clust;
    unsigned int bytesperclust = mydata->clust_size * mydata->sect_size;

    dir_newclust = (int)find_empty_cluster(mydata);
    if (dir_newclust == 0)
        return -ENOSPC;             /* 偏离: 上游不判(find_empty_cluster 永不返回 0) */

    /*
     * Flush before updating FAT to ensure valid directory structure
     * in case of failure.
     */
    itr->clust = (unsigned int)dir_newclust;
    itr->next_clust = (unsigned int)dir_newclust;
    memset(itr->block, 0x00, bytesperclust);
    if (flush_dir(itr))
        return -EIO;

    set_fatent_value(mydata, (__u32)dir_oldclust, (__u32)dir_newclust);
    if (mydata->fatsize == 32)
        set_fatent_value(mydata, (__u32)dir_newclust, 0xffffff8);
    else if (mydata->fatsize == 16)
        set_fatent_value(mydata, (__u32)dir_newclust, 0xfff8);
    else if (mydata->fatsize == 12)
        set_fatent_value(mydata, (__u32)dir_newclust, 0xff8);

    if (flush_dirty_fat_buffer(mydata) < 0)
        return -EIO;

    itr->dent = (dir_entry *)itr->block;
    itr->last_cluster = 1;
    itr->remaining = (int)(bytesperclust / sizeof(dir_entry)) - 1;

    return 0;
}

/*
 * Set empty cluster from 'entry' to the end of a file
 */
int clear_fatent(fsdata *mydata, __u32 entry)
{
    __u32 fat_val;

    while (!CHECK_CLUST(entry, mydata->fatsize)) {
        fat_val = get_fatent(mydata, entry);
        if (fat_val != 0)
            set_fatent_value(mydata, entry, 0);
        else
            break;

        entry = fat_val;
    }

    /* Flush fat buffer */
    if (flush_dirty_fat_buffer(mydata) < 0)
        return -1;

    return 0;
}

/*
 * Set start cluster in directory entry
 */
void set_start_cluster(const fsdata *mydata, dir_entry *dentptr,
                       __u32 start_cluster)
{
    if (mydata->fatsize == 32)
        dentptr->starthi =
            uboot_fat_cpu_to_le16((__u16)((start_cluster & 0xffff0000) >> 16));
    dentptr->start = uboot_fat_cpu_to_le16((__u16)(start_cluster & 0xffff));
}

/*
 * Check whether adding a file makes the file system to
 * exceed the size of the block device
 * Return -1 when overflow occurs, otherwise return 0
 */
int check_overflow(fsdata *mydata, __u32 clustnum, loff_t size)
{
    __u32 startsect, sect_num, offset;

    if (clustnum > 0)
        startsect = clust_to_sect(mydata, clustnum);
    else
        startsect = mydata->rootdir_sect;

    sect_num = (__u32)uboot_fat_div_u64_rem((u64)size, mydata->sect_size, &offset);

    if (offset != 0)
        sect_num++;

    if (startsect + sect_num > (__u32)total_sector)
        return -1;
    return 0;
}

/*
 * 定位"包含字节位置 pos 的那个簇": 从首簇沿链走, 返回 ①该簇号 ②该簇起点在文件里的字节
 * 偏移 ③该簇的覆盖量(走到链尾时可能小于 bpc —— 那是"超出文件当前链"的部分)。
 *
 * 这是把上游 set_contents 里那段"走链到 pos"独立出来的结果: 上游把定位与"覆盖/追加"的
 * 判定混在一起, 导致 `pos == 链覆盖量`(顺序追加正好落在簇边界)被误判成越界, 而
 * `pos` 落在链内非零簇时 offset 又会被算错 —— 两类真实故障都在宿主自检里被踩到。
 */
static __u32 ub_fat_clust_at(const fsdata *fs, __u32 start, u64 pos,
                             u64 *start_out, u64 *covered_out)
{
    const u32 bpc = (u32)fs->clust_size * (u32)fs->sect_size;
    __u32 cur = start;
    u64 covered = bpc;
    u64 off = 0;

    while (covered < pos) {
        const __u32 nxt = get_fatent((fsdata *)(br_uintptr_t)fs, cur);

        if (CHECK_CLUST(nxt, fs->fatsize)) {
            break;
        }
        cur = nxt;
        off = covered;
        covered += bpc;
    }
    *start_out = off;
    *covered_out = covered;
    return cur;
}

/*
 * Write at most 'maxsize' bytes from 'buffer' into
 * the file associated with 'dentptr'
 * Update the number of bytes written in *gotsize and return 0
 * or return -1 on fatal errors.
 *
 * ★★ 本函数对上游是**重写**(不是逐行移植), 原因与逐条对应如下 —— 这是本次移植里最大的一处
 *    有意偏离, 登记在 NOTICE 的改写要点第 6 条:
 *      ① 上游在 `maxsize == 0` 时走"清空整条簇链"分支(注释却写着 "no more write"),
 *         于是"零长度写"会毁掉文件(本项目的 VFS 创建路径就要调一次零长度写)。
 *         本件: 零长度 = 空操作。
 *      ② 上游的"覆盖/追加"二分不覆盖 `pos == 文件长度` 且长度恰在簇边界上的**顺序追加**
 *         (本项目 VFS 的分段写、truncate 的扩大都要走这条) ⇒ 上游会报 -EINVAL。
 *         本件: 用 ub_fat_clust_at() 定位后统一处理"在链内覆盖"与"从链尾追加"。
 *      ③ 上游写完最后一簇后**不把末尾簇标成 EOC**(只在"写满且链还有剩余"时清尾巴),
 *         于是文件末尾的簇仍然指向已释放的簇 —— 下一次写会踩到别人。
 *         本件: 每次写完都把末尾簇标 EOC, 并释放其后的整条链。
 *      ④ 上游用 `assert()` 表达"不可能发生"; 无 libc 的构建里那是空语句。
 *         本件: 显式判错 + -EIO。
 *   保留下来的东西: 用**同一套原语**(get_fatent / set_fatent_value / clear_fatent /
 *   find_empty_cluster / determine_fatent / set_cluster / get_set_cluster /
 *   check_overflow), 所以"写"与"截断"不会有两套簇管理。
 */
static int
set_contents(fsdata *mydata, dir_entry *dentptr, loff_t pos, __u8 *buffer,
             loff_t maxsize, loff_t *gotsize)
{
    const u32 bpc = (u32)mydata->clust_size * (u32)mydata->sect_size;
    __u32 cur = START(dentptr);
    u64 want, written = 0;
    u64 start_off = 0, cov = 0;

    *gotsize = 0;

    if (maxsize <= 0)
        return 0;                      /* ① 零长度写: 什么都不做(上游会毁文件) */

    if ((pos < 0) || ((u64)pos > 0xffffffffull)) {
        log_err("FAT: bad write position %lld", (long long)pos);
        return -1;
    }
    want = (u64)pos + (u64)maxsize;

    ub_fat_dbg("%llu bytes (pos=%lld)", (unsigned long long)want, (long long)pos);

    if (cur <= 1u) {
        if (pos != 0) {                /* 空文件只能从 0 写起 */
            log_err("FAT: write at %lld into a file with no cluster", (long long)pos);
            return -1;
        }
    } else {
        cur = ub_fat_clust_at(mydata, cur, (u64)pos, &start_off, &cov);
        /* 走链停下的两种合法形状:
         *   · cov > pos  ⇒ pos 落在某个已有簇内部(覆盖);
         *   · cov == pos ⇒ pos 恰好是"链覆盖量的末尾"——**追加**(本项目 VFS 的分段写)。
         * 只有 cov < pos 才是"文件里有洞", 而那是上游/本项目都不产生的形状。 */
        if ((u64)pos > cov) {
            log_err("FAT: write at %lld beyond chain end %llu (hole)",
                    (long long)pos, (unsigned long long)cov);
            return -1;
        }
        if ((u64)pos < start_off) {
            log_err("FAT: internal cursor error (pos=%lld start=%llu)",
                    (long long)pos, (unsigned long long)start_off);
            return -1;
        }
    }

    /*
     * 写主循环。**每一轮进入时的不变量**:
     *   `cur` = 要写的簇; `start_off` = 该簇在文件里的起始字节偏移;
     *   `(u64)pos + written` ∈ [start_off, start_off + bpc)。
     * 循环体开头负责维护这个不变量 —— 见下面的"推进一格"。
     */
    while (written < want) {
        const u64 at = (u64)pos + written;      /* 本轮要写的文件内位置 */
        u64 in_clust;
        u64 left;
        u64 here;

        /* ---- 维护不变量: 需要"推进一格"的两种情况 ---- */
        if ((cur <= 1u) || (at >= (start_off + bpc))) {
            if (cur <= 1u) {
                /* 文件还没有首簇(或刚分配失败)⇒ 从第一个空簇开始 */
                cur = find_empty_cluster(mydata);
                if (cur <= 1u) {
                    log_err("FAT: no free cluster for first cluster");
                    return -1;
                }
                set_start_cluster(mydata, dentptr, cur);
                start_off = at;
            } else {
                /* 走到当前簇的末尾: 接下一簇(没有就分配一个) */
                const __u32 nxt = get_fatent(mydata, cur);

                start_off += bpc;
                if (!CHECK_CLUST(nxt, mydata->fatsize)) {
                    cur = nxt;
                } else {
                    const __u32 nc = determine_fatent(mydata, cur);

                    if (nc <= 1u) {
                        log_err("FAT: no free cluster to extend file");
                        return -1;
                    }
                    set_fatent_value(mydata, cur, nc);
                    if (mydata->fatsize == 12)
                        set_fatent_value(mydata, nc, 0xfff);
                    else if (mydata->fatsize == 16)
                        set_fatent_value(mydata, nc, 0xffff);
                    else
                        set_fatent_value(mydata, nc, 0xfffffff);
                    cur = nc;
                }
            }
        }

        in_clust = (at >= start_off) ? (at - start_off) : 0u;
        if (in_clust >= bpc)
            in_clust = 0;              /* 防御: 不变量成立时不可达 */

        left = want - at;
        here = bpc - in_clust;
        if (here > left)
            here = left;
        if (here == 0u)
            break;

        if ((in_clust == 0u) && (here == bpc)) {
            if (check_overflow(mydata, cur, (loff_t)bpc) != 0) {
                log_err("FAT: no space left (cluster %u)", cur);
                return -1;
            }
            if (set_cluster(mydata, cur, buffer + written, bpc) != 0) {
                log_err("Error: writing cluster");
                return -1;
            }
        } else {
            loff_t got = 0;

            if (check_overflow(mydata, cur, (loff_t)(in_clust + here)) != 0) {
                log_err("FAT: no space left (cluster %u)", cur);
                return -1;
            }
            if (get_set_cluster(mydata, cur, (loff_t)in_clust, buffer + written,
                                (loff_t)here, &got) != 0) {
                log_err("Error get-and-setting cluster");
                return -1;
            }
            if ((u64)got != here) {
                log_err("FAT: short cluster write (%lld of %llu)",
                        (long long)got, (unsigned long long)here);
                return -1;
            }
        }
        written += here;
    }

    /* ③ 末尾簇标 EOC + 释放其后的整条链(上游缺这一步) */
    {
        const __u32 nxt = get_fatent(mydata, cur);

        if (!CHECK_CLUST(nxt, mydata->fatsize)) {
            if (clear_fatent(mydata, nxt) < 0) {
                log_err("Error: releasing tail cluster chain");
                return -1;
            }
        }
        if (mydata->fatsize == 12)
            set_fatent_value(mydata, cur, 0xfff);
        else if (mydata->fatsize == 16)
            set_fatent_value(mydata, cur, 0xffff);
        else
            set_fatent_value(mydata, cur, 0xfffffff);
    }

    *gotsize = (loff_t)written;
    return 0;
}

/**
 * dentry_set_time() - set change time
 *
 * @dentptr:	directory entry
 *
 * 偏离: 上游优先走 DM_RTC(RTC 设备), 失败才落到"2000-01-01"。v1 没有 RTC 设备
 * (`framework/dev-core` 里没有 RTC 类), 所以本件直接落兜底分支 —— 数值逐字一致。
 */
static void dentry_set_time(dir_entry *dentptr)
{
    dentptr->date = 0x2821; /* 2000-01-01 */
    dentptr->time = 0;
}

/**
 * fill_dentry() - fill directory entry with shortname
 */
static void fill_dentry(fsdata *mydata, dir_entry *dentptr,
                        const char *shortname, __u32 start_cluster, __u32 size,
                        __u8 attr)
{
    memset(dentptr, 0, sizeof(*dentptr));

    set_start_cluster(mydata, dentptr, start_cluster);
    dentptr->size = uboot_fat_cpu_to_le32(size);

    dentptr->attr = attr;

    /* Set change date */
    dentry_set_time(dentptr);
    /* Set creation date */
    dentptr->cdate = dentptr->date;
    dentptr->ctime = dentptr->time;

    memcpy(&dentptr->nameext, shortname, SHORT_NAME_SIZE);
}

/**
 * fat_itr_parent() - modifies the iterator to the parent directory of the
 * current iterator.
 */
static int fat_itr_parent(fat_itr *itr)
{
    int ret;

    if (itr->is_root)
        return -EIO;

    /* ensure iterator is at the first directory entry */
    ret = fat_move_to_cluster(itr, itr->start_clust);
    if (ret)
        return ret;

    return fat_itr_resolve(itr, "..", TYPE_DIR);
}

/**
 * update_parent_dir_props - updates the modified time for the parent directory
 *
 * @dir_itr:	iterator positioned anywhere in a directory whose parent
 * should be updated
 * @Return:	0 for success, -errno otherwise
 *
 * 偏离: 上游在 `!CONFIG_IS_ENABLED(DM_RTC)` 时**立刻返回 0**(这条路径只更新时间戳);
 * 本件没有 RTC ⇒ 这里永远是"立刻返回 0"。保留整段结构是为了将来接 RTC 时一眼能看到
 * 落点(并如实登记"v1 不做时间"这件事)。
 */
static int update_parent_dir_props(fat_itr *dir_itr)
{
    (void)dir_itr;
    return 0;
}

/**
 * create_link() - inserts a directory entry for a file or directory
 *
 * @itr:	directory iterator
 * @basename:	file name
 * @clust:	cluster number the new directory entry should point to. Use 0
 * if no cluster is assigned yet
 * @size:	file size
 * @attr:	file attributes
 * Return:	0 for success
 */
static int create_link(fat_itr *itr, char *basename, __u32 clust, __u32 size,
                       __u8 attr)
{
    char shortname[SHORT_NAME_SIZE];
    int ndent;
    int ret;

    /* Check if long name is needed */
    ndent = set_name(itr, basename, shortname);
    if (ndent < 0)
        return ndent;
    ret = fat_find_empty_dentries(itr, ndent);
    if (ret)
        return ret;
    if (ndent > 1) {
        /* Set long name entries */
        ret = fill_dir_slot(itr, basename, shortname);
        if (ret)
            return ret;
    }

    fill_dentry(itr->fsdata, itr->dent, shortname, clust, size, attr);
    ret = update_parent_dir_props(itr);

    return ret;
}

/**
 * find_directory_entry() - find a directory entry by filename
 *
 * @itr:	directory iterator
 * @filename:	name of file to find
 * Return:	directory entry or NULL
 *
 * 偏离: 形参从 `char *filename` 收成 `const char *`(调用点传的全是字面量/常量缓冲;
 * 上游那个非 const 参数只是历史遗留)。
 */
dir_entry *find_directory_entry(fat_itr *itr, const char *filename)
{
    int match = 0;

    while (fat_itr_next(itr)) {
        /* check both long and short name: */
        if (!uboot_fat_strcasecmp(filename, itr->name))
            match = 1;
        else if (itr->name != itr->s_name &&
                 !uboot_fat_strcasecmp(filename, itr->s_name))
            match = 1;

        if (!match)
            continue;

        if (itr->dent->nameext.name[0] == '\0')
            return NULL;
        else
            return itr->dent;
    }

    return NULL;
}

int split_filename(char *filename, char **dirname, char **basename)
{
    char *p, *last_slash, *last_slash_cont;

again:
    p = filename;
    last_slash = NULL;
    last_slash_cont = NULL;
    while (*p) {
        if (ISDIRDELIM(*p)) {
            last_slash = p;
            last_slash_cont = p;
            /* continuous slashes */
            while (ISDIRDELIM(*p))
                last_slash_cont = p++;
            if (!*p)
                break;
        }
        p++;
    }

    if (last_slash) {
        if (last_slash_cont == (filename + uboot_fat_strlen(filename) - 1)) {
            /* remove trailing slashes */
            *last_slash = '\0';
            goto again;
        }

        if (last_slash == filename) {
            /* avoid ""(null) directory */
            *dirname = (char *)"/";
        } else {
            *last_slash = '\0';
            *dirname = filename;
        }

        *last_slash_cont = '\0';
        filename = last_slash_cont + 1;
    } else {
        *dirname = (char *)"/"; /* root by default */
    }

    /*
     * The FAT32 File System Specification v1.03 requires leading and
     * trailing spaces as well as trailing periods to be ignored.
     */
    for (; *filename == ' '; ++filename)
        ;

    /* Keep special entries '.' and '..' */
    if (filename[0] == '.' &&
        (!filename[1] || (filename[1] == '.' && !filename[2])))
        goto done;

    /* Remove trailing periods and spaces */
    for (p = filename + uboot_fat_strlen(filename) - 1; p >= filename; --p) {
        switch (*p) {
        case ' ':
        case '.':
            *p = 0;
            break;
        default:
            goto done;
        }
    }

done:
    *basename = filename;

    return 0;
}

/**
 * normalize_longname() - check long file name and convert to lower case
 *
 * We assume here that the FAT file system is using an 8bit code page.
 * Linux typically uses CP437, EDK2 assumes CP1250.
 *
 * @l_filename:	preallocated buffer receiving the normalized name
 * @filename:	filename to normalize
 * Return:	0 on success, -1 on failure
 */
static int normalize_longname(char *l_filename, const char *filename)
{
    const char *p;
    static const char illegal[] = "<>:\"/\\|?*";
    br_size_t len;

    len = uboot_fat_strlen(filename);
    if (!len || len >= UB_FAT_LFN_MAX || filename[len - 1] == '.')
        return -1;

    for (p = filename; *p; ++p) {
        if ((unsigned char)*p < 0x20)
            return -1;
        if (uboot_fat_strchr(illegal, *p))
            return -1;
    }

    uboot_fat_strcpy(l_filename, filename, UB_FAT_LFN_MAX);
    downcase(l_filename, UB_FAT_LFN_MAX);

    return 0;
}

/*
 * 无 libc 的 strdup 替代: 把路径复制进**调用方给的栈上缓冲**。
 * 上游每个入口都 `filename_copy = strdup(filename)` 然后 split_filename 原地切它
 * (split_filename 会往缓冲里写 '\0')。本件同样需要一份可写副本, 但不需要堆。
 */
static int ub_fat_path_copy(char *dst, br_size_t cap, const char *src)
{
    const br_size_t n = uboot_fat_strlen(src);

    if ((n == 0u) || (n >= cap)) {
        return -EINVAL;
    }
    memcpy(dst, src, n + 1u);
    return 0;
}

int file_fat_write_at(const char *filename, loff_t pos, void *buffer,
                      loff_t size, loff_t *actwrite)
{
    dir_entry *retdent;
    fsdata *mydata = uboot_fat_fsdata();
    fat_itr *itr = NULL;
    int ret = -1;
    char *parent, *basename;
    char l_filename[UB_FAT_LFN_MAX];
    char path_buf[UB_FAT_PATH_MAX];

    ub_fat_dbg("writing %s", filename);

    if ((mydata == NULL) || (actwrite == NULL))
        return -EINVAL;
    *actwrite = 0;

    ret = ub_fat_path_copy(path_buf, sizeof(path_buf), filename);
    if (ret)
        return ret;
    split_filename(path_buf, &parent, &basename);
    if (!uboot_fat_strlen(basename)) {
        ret = -EINVAL;
        goto exit;
    }

    if (normalize_longname(l_filename, basename)) {
        log_err("FAT: illegal filename (%s)", basename);
        ret = -EINVAL;
        goto exit;
    }

    itr = br_malloc(sizeof(fat_itr));
    if (!itr) {
        ret = -ENOMEM;
        goto exit;
    }

    ret = fat_itr_root(itr);
    if (ret)
        goto exit;

    total_sector = (int)uboot_fat_vol_total_sect();

    ret = fat_itr_resolve(itr, parent, TYPE_DIR);
    if (ret) {
        log_err("%s: doesn't exist (%d)", parent, ret);
        goto exit;
    }

    retdent = find_directory_entry(itr, l_filename);

    if (retdent) {
        if (fat_itr_isdir(itr)) {
            ret = -EISDIR;
            goto exit;
        }

        /* A file exists */
        if (pos == -1)
            /* Append to the end */
            pos = (loff_t)FAT2CPU32(retdent->size);
        if (pos > (loff_t)FAT2CPU32(retdent->size)) {
            /* No hole allowed */
            ret = -EINVAL;
            goto exit;
        }

        /* Update file size in a directory entry */
        retdent->size = uboot_fat_cpu_to_le32((__u32)(pos + size));
        /* Update change date */
        dentry_set_time(retdent);
    } else {
        if (pos) {
            /* No hole allowed */
            ret = -EINVAL;
            goto exit;
        }

        ret = create_link(itr, basename, 0, (__u32)size, ATTR_ARCH);
        if (ret)
            goto exit;

        retdent = itr->dent;
    }

    ret = set_contents(mydata, retdent, pos, (__u8 *)buffer, size, actwrite);
    if (ret < 0) {
        log_err("Error: writing contents");
        ret = -EIO;
        goto exit;
    }
    ub_fat_dbg("attempt to write 0x%llx bytes", (unsigned long long)*actwrite);

    /* Flush fat buffer */
    ret = flush_dirty_fat_buffer(mydata);
    if (ret) {
        log_err("Error: flush fat buffer");
        ret = -EIO;
        goto exit;
    }

    /* Write directory table to device */
    ret = flush_dir(itr);

exit:
    br_free(itr);
    return ret;
}

int file_fat_write(const char *filename, void *buffer, loff_t offset,
                   loff_t maxsize, loff_t *actwrite)
{
    return file_fat_write_at(filename, offset, buffer, maxsize, actwrite);
}

static int fat_dir_entries(fat_itr *itr)
{
    fat_itr *dirs;
    fsdata fs;                     /* 局部副本 + 局部 FAT 缓存(上游同型) */
    int count;

    dirs = br_malloc(sizeof(fat_itr));
    if (!dirs) {
        ub_fat_dbg("Error: allocating memory");
        count = -ENOMEM;
        goto exit;
    }

    /* duplicate fsdata —— ★ 必须清掉从卷上抄来的脏标志: 这份副本是**只读缓存**,
     * 若带着 dirty=1 去 get_fatent(), flush_dirty_fat_buffer() 就会把这份"刚读进来、
     * 尚未被任何人修改"的 6 扇区窗口写回盘上, 覆盖掉真正的 FAT。 */
    fat_itr_child(dirs, itr);
    fs = *dirs->fsdata;
    fs.fat_dirty = 0;

    /* allocate local fat buffer */
    fs.fatbuf = br_malloc(uboot_fat_fat_bufsize(fs.sect_size));
    if (!fs.fatbuf) {
        ub_fat_dbg("Error: allocating memory");
        count = -ENOMEM;
        goto exit;
    }
    fs.fatbufnum = -1;
    dirs->fsdata = &fs;

    for (count = 0; fat_itr_next(dirs); count++)
        ;

exit:
    if (dirs != NULL) {
        uboot_fat_fatbuf_release(dirs->fsdata == &fs ? &fs : NULL);
    }
    br_free(dirs);
    return count;
}

/**
 * delete_single_dentry() - delete a single directory entry
 */
static int delete_single_dentry(fat_itr *itr)
{
    struct dir_entry *dent = itr->dent;

    memset(dent, 0, sizeof(*dent));
    dent->nameext.name[0] = DELETED_FLAG;

    if (!itr->remaining)
        return flush_dir(itr);
    return 0;
}

/**
 * delete_long_name() - delete long name directory entries
 */
static int delete_long_name(fat_itr *itr)
{
    int seqn = itr->dent->nameext.name[0] & ~LAST_LONG_ENTRY_MASK;

    while (seqn--) {
        struct dir_entry *dent;
        int ret;

        ret = delete_single_dentry(itr);
        if (ret)
            return ret;
        dent = next_dent(itr);
        if (!dent)
            return -EIO;
    }
    return 0;
}

/**
 * delete_dentry_link() - deletes a directory entry, but not the cluster chain
 * it points to
 */
static int delete_dentry_link(fat_itr *itr)
{
    int ret;

    itr->dent = itr->dent_start;
    itr->remaining = itr->dent_rem;
    /* Delete long name */
    if ((itr->dent->attr & ATTR_VFAT) == ATTR_VFAT &&
        (itr->dent->nameext.name[0] & LAST_LONG_ENTRY_MASK)) {
        ret = delete_long_name(itr);
        if (ret)
            return ret;
    }
    /* Delete short name */
    delete_single_dentry(itr);

    ret = flush_dir(itr);
    if (ret)
        return ret;

    return update_parent_dir_props(itr);
}

/**
 * delete_dentry_long() - remove directory entry
 */
static int delete_dentry_long(fat_itr *itr)
{
    fsdata *mydata = itr->fsdata;
    dir_entry *dent = itr->dent;

    /* free cluster blocks */
    clear_fatent(mydata, START(dent));
    if (flush_dirty_fat_buffer(mydata) < 0) {
        log_err("Error: flush fat buffer");
        return -EIO;
    }
    /* Position to first directory entry for long name */
    if (itr->clust != itr->dent_clust) {
        int ret;

        ret = fat_move_to_cluster(itr, itr->dent_clust);
        if (ret)
            return ret;
    }
    return delete_dentry_link(itr);
}

int fat_unlink(const char *filename)
{
    fsdata *fs = uboot_fat_fsdata();
    fat_itr *itr = NULL;
    int n_entries, ret;
    char *dirname, *basename;
    char path_buf[UB_FAT_PATH_MAX];

    itr = br_malloc(sizeof(fat_itr));
    if (!itr || !fs) {
        log_err("Error: out of memory");
        ret = -ENOMEM;
        goto exit;
    }
    ret = ub_fat_path_copy(path_buf, sizeof(path_buf), filename);
    if (ret)
        goto exit;
    split_filename(path_buf, &dirname, &basename);

    if (!uboot_fat_strcmp(dirname, "/") && !uboot_fat_strcmp(basename, "")) {
        log_err("Error: cannot remove root");
        ret = -EINVAL;
        goto exit;
    }

    ret = fat_itr_root(itr);
    if (ret)
        goto exit;

    total_sector = (int)uboot_fat_vol_total_sect();

    ret = fat_itr_resolve(itr, dirname, TYPE_DIR);
    if (ret) {
        log_err("%s: doesn't exist (%d)", dirname, ret);
        ret = -ENOENT;
        goto exit;
    }

    if (!find_directory_entry(itr, basename)) {
        log_err("%s: doesn't exist (%d)", basename, -ENOENT);
        ret = -ENOENT;
        goto exit;
    }

    if (fat_itr_isdir(itr)) {
        n_entries = fat_dir_entries(itr);
        if (n_entries < 0) {
            ret = n_entries;
            goto exit;
        }
        if (n_entries > 2) {
            log_err("Error: directory is not empty: %d", n_entries);
            ret = -EINVAL;
            goto exit;
        }
    }

    ret = delete_dentry_long(itr);

exit:
    br_free(itr);
    return ret;
}

int fat_mkdir(const char *dirname)
{
    dir_entry *retdent;
    fsdata *mydata = uboot_fat_fsdata();
    fat_itr *itr = NULL;
    char *parent, *basename;
    char l_dirname[UB_FAT_LFN_MAX];
    char path_buf[UB_FAT_PATH_MAX];
    int ret = -1;
    loff_t actwrite;
    unsigned int bytesperclust;
    dir_entry *dotdent = NULL;

    if (mydata == NULL)
        return -ENODEV;
    ret = ub_fat_path_copy(path_buf, sizeof(path_buf), dirname);
    if (ret)
        goto exit;

    split_filename(path_buf, &parent, &basename);
    if (!uboot_fat_strlen(basename)) {
        ret = -EINVAL;
        goto exit;
    }

    if (normalize_longname(l_dirname, basename)) {
        log_err("FAT: illegal filename (%s)", basename);
        ret = -EINVAL;
        goto exit;
    }

    itr = br_malloc(sizeof(fat_itr));
    if (!itr) {
        ret = -ENOMEM;
        goto exit;
    }

    ret = fat_itr_root(itr);
    if (ret)
        goto exit;

    total_sector = (int)uboot_fat_vol_total_sect();

    ret = fat_itr_resolve(itr, parent, TYPE_DIR);
    if (ret) {
        log_err("%s: doesn't exist (%d)", parent, ret);
        goto exit;
    }

    retdent = find_directory_entry(itr, l_dirname);

    if (retdent) {
        log_err("%s: already exists", l_dirname);
        ret = -EEXIST;
        goto exit;
    } else {
        if (itr->is_root) {
            /* root dir cannot have "." or ".." */
            if (!uboot_fat_strcmp(l_dirname, ".") ||
                !uboot_fat_strcmp(l_dirname, "..")) {
                ret = -EINVAL;
                goto exit;
            }
        }

        ret = create_link(itr, basename, 0, 0, ATTR_DIR | ATTR_ARCH);
        if (ret)
            goto exit;

        retdent = itr->dent;
    }

    /* Default entries */
    bytesperclust = mydata->clust_size * mydata->sect_size;
    dotdent = br_malloc(bytesperclust);
    if (!dotdent) {
        ret = -ENOMEM;
        goto exit;
    }
    memset(dotdent, 0, bytesperclust);

    memcpy(&dotdent[0].nameext, ".          ", 11);
    dotdent[0].attr = ATTR_DIR | ATTR_ARCH;

    memcpy(&dotdent[1].nameext, "..         ", 11);
    dotdent[1].attr = ATTR_DIR | ATTR_ARCH;

    if (itr->is_root)
        set_start_cluster(mydata, &dotdent[1], 0);
    else
        set_start_cluster(mydata, &dotdent[1], itr->start_clust);

    ret = set_contents(mydata, retdent, 0, (__u8 *)dotdent,
                       bytesperclust, &actwrite);
    if (ret < 0) {
        log_err("Error: writing contents");
        goto exit;
    }
    /* Write twice for "." */
    set_start_cluster(mydata, &dotdent[0], START(retdent));
    ret = set_contents(mydata, retdent, 0, (__u8 *)dotdent,
                       bytesperclust, &actwrite);
    if (ret < 0) {
        log_err("Error: writing contents");
        goto exit;
    }

    /* Flush fat buffer */
    ret = flush_dirty_fat_buffer(mydata);
    if (ret) {
        log_err("Error: flush fat buffer");
        ret = -EIO;
        goto exit;
    }

    /* Write directory table to device */
    ret = flush_dir(itr);

exit:
    br_free(itr);
    br_free(dotdent);
    return ret;
}

/**
 * check_path_prefix() - ensures one path does not contains another path as a
 * prefix.
 *
 * note: the iterator may be pointing to any directory entry in the directory
 *
 * @prefix_clust:	start cluster of the final directory in the prefix path
 * @path_itr:	iterator of the path to check
 * Return:	-errno on error, 0 if path_itr does not have the directory
 * at prefix_clust as an ancestor.
 */
static int check_path_prefix(loff_t prefix_clust, fat_itr *path_itr)
{
    fat_itr itr;
    fsdata fs;                     /* 局部副本 + 局部 FAT 缓存(上游同型) */
    int ret;

    /* duplicate fsdata —— 同 fat_dir_entries(): 副本是只读缓存, 不许带脏标志 */
    itr = *path_itr;
    fs = *itr.fsdata;
    fs.fat_dirty = 0;

    /* allocate local fat buffer */
    fs.fatbuf = br_malloc(uboot_fat_fat_bufsize(fs.sect_size));
    if (!fs.fatbuf) {
        ub_fat_dbg("Error: allocating memory");
        ret = -ENOMEM;
        goto exit;
    }

    fs.fatbufnum = -1;
    itr.fsdata = &fs;

    /* ensure iterator is at the first directory entry */
    ret = fat_move_to_cluster(&itr, itr.start_clust);
    if (ret)
        goto exit;

    while (1) {
        if (prefix_clust == (loff_t)itr.start_clust) {
            ret = -EINVAL;
            goto exit;
        }

        if (itr.is_root) {
            ret = 0;
            goto exit;
        }

        /* Should not occur in a well-formed FAT filesystem besides the root */
        if (fat_itr_parent(&itr)) {
            ub_fat_dbg("FAT filesystem corrupt!");
            ub_fat_dbg("dir @ clust %u has no parent direntry",
                       itr.start_clust);
            ret = -EIO;
            goto exit;
        }
    }

exit:
    uboot_fat_fatbuf_release(&fs);
    return ret;
}

/*
 * fat_rename - rename/move a file or directory
 *
 * 偏离: 两处 `char *x = strdup(path)` → 栈上缓冲(见 ub_fat_path_copy);
 * `log_debug` 保留(它是上游的诊断口)。
 */
int fat_rename(const char *old_path, const char *new_path)
{
    fat_itr *old_itr = NULL, *new_itr = NULL;
    fsdata old_datablock;
    fsdata new_datablock;
    /* used for START macro */
    fsdata *mydata = &old_datablock;
    int ret = -EIO, is_old_dir;
    char *old_dirname, *old_basename;
    char *new_dirname, *new_basename;
    char l_new_basename[UB_FAT_LFN_MAX];
    char old_buf[UB_FAT_PATH_MAX];
    char new_buf[UB_FAT_PATH_MAX];
    __u32 old_clust;
    dir_entry *found_existing;
    /* only set if found_existing != NULL */
    __u32 new_clust = 0;

    memset(&old_datablock, 0, sizeof(old_datablock));
    memset(&new_datablock, 0, sizeof(new_datablock));

    ret = ub_fat_path_copy(old_buf, sizeof(old_buf), old_path);
    if (ret)
        return ret;
    ret = ub_fat_path_copy(new_buf, sizeof(new_buf), new_path);
    if (ret)
        return ret;

    old_itr = br_malloc(sizeof(fat_itr));
    new_itr = br_malloc(sizeof(fat_itr));
    if (!old_itr || !new_itr) {
        log_debug("Error: out of memory");
        ret = -ENOMEM;
        goto exit;
    }
    split_filename(old_buf, &old_dirname, &old_basename);
    split_filename(new_buf, &new_dirname, &new_basename);

    if (normalize_longname(l_new_basename, new_basename)) {
        log_debug("FAT: illegal filename (%s)", new_basename);
        ret = -EINVAL;
        goto exit;
    }

    if (!uboot_fat_strcmp(old_basename, ".") || !uboot_fat_strcmp(old_basename, "..") ||
        !uboot_fat_strcmp(old_basename, "") || !uboot_fat_strcmp(l_new_basename, ".") ||
        !uboot_fat_strcmp(l_new_basename, "..") || !uboot_fat_strcmp(l_new_basename, "")) {
        ret = -EINVAL;
        goto exit;
    }

    /* checking for old_path == new_path is deferred until they're resolved */

    /* resolve old_path */
    ret = fat_itr_root(old_itr);
    if (ret)
        goto exit;

    ret = fat_itr_resolve(old_itr, old_dirname, TYPE_DIR);
    if (ret) {
        log_debug("%s doesn't exist (%d)", old_dirname, ret);
        ret = -ENOENT;
        goto exit;
    }

    if (!find_directory_entry(old_itr, old_basename)) {
        log_debug("%s doesn't exist (%d)", old_basename, -ENOENT);
        ret = -ENOENT;
        goto exit;
    }

    /* store clust old_path points to, to relink later */
    total_sector = (int)uboot_fat_vol_total_sect();
    old_clust = START(old_itr->dent);
    is_old_dir = fat_itr_isdir(old_itr);

    /* resolve new_path*/
    ret = fat_itr_root(new_itr);
    if (ret)
        goto exit;

    ret = fat_itr_resolve(new_itr, new_dirname, TYPE_DIR);
    if (ret) {
        log_debug("%s doesn't exist (%d)", new_dirname, ret);
        ret = -ENOENT;
        goto exit;
    }

    found_existing = find_directory_entry(new_itr, l_new_basename);

    if (found_existing) {
        /* store cluster of new_path since it may need to be deleted */
        new_clust = START(new_itr->dent);

        /* old_path is new_path, noop */
        if (old_clust == new_clust) {
            ret = 0;
            goto exit;
        }

        if (fat_itr_isdir(new_itr) != is_old_dir) {
            if (is_old_dir)
                ret = -ENOTDIR;
            else
                ret = -EISDIR;
            goto exit;
        }
    }

    if (is_old_dir) {
        ret = check_path_prefix((loff_t)old_clust, new_itr);
        if (ret)
            goto exit;
    }

    /* create/update dentry to point to old_path's data cluster */
    if (found_existing) {
        struct nameext new_name = new_itr->dent->nameext;
        __u8 lcase = new_itr->dent->lcase;

        if (is_old_dir) {
            int n_entries = fat_dir_entries(new_itr);

            if (n_entries < 0) {
                ret = n_entries;
                goto exit;
            }
            if (n_entries > 2) {
                log_debug("Error: directory is not empty: %d", n_entries);
                ret = -ENOTEMPTY;
                goto exit;
            }
        }

        *new_itr->dent = *old_itr->dent;
        new_itr->dent->nameext = new_name;
        new_itr->dent->lcase = lcase;

        ret = update_parent_dir_props(new_itr);
        if (ret)
            goto exit;
    } else {
        /* reset iterator to the start of the directory */
        ret = fat_move_to_cluster(new_itr, new_itr->start_clust);
        if (ret)
            goto exit;

        ret = create_link(new_itr, l_new_basename, old_clust,
                          old_itr->dent->size,
                          (__u8)(old_itr->dent->attr | ATTR_ARCH));
        if (ret)
            goto exit;
    }

    ret = flush_dir(new_itr);
    if (ret)
        goto exit;

    /* with new_path data cluster unreferenced, clear it */
    if (found_existing) {
        ret = clear_fatent(&new_datablock, new_clust);
        if (ret)
            goto exit;
    }

    /* update moved directory so the parent is new_path */
    if (is_old_dir) {
        __u32 clust = new_itr->start_clust;
        dir_entry *dent;

        fat_itr_child(new_itr, new_itr);
        dent = find_directory_entry(new_itr, "..");
        if (!dent) {
            log_debug("FAT filesystem corrupt!");
            log_debug("dir %s has no parent direntry", l_new_basename);
            ret = -EIO;
            goto exit;
        }
        set_start_cluster(&new_datablock, dent, clust);
        ret = flush_dir(new_itr);
        if (ret)
            goto exit;
        /* restore directory location to update parent props below */
        fat_itr_child(new_itr, new_itr);
    }

    /* refresh old in case write happened to the same block. */
    ret = fat_move_to_cluster(old_itr, old_itr->dent_clust);
    if (ret)
        goto exit;

    ret = delete_dentry_link(old_itr);
exit:
    br_free(new_itr);
    br_free(old_itr);

    return ret;
}

/* ==================================================================== 截断(本件新增)
 *
 * 上游 u-boot **没有** fat_truncate: 它的 `file_fat_write_at()` 只做"从 pos 写 size 字节",
 * 而"把文件变短"这件事只被当成写路径的副产品(写满之后清掉尾部簇链)。本项目的 VFS 契约
 * 要求 `setattr(BR_STAT_SIZE)`(⇒ `br_truncate`), 所以本件用**上游自己的原语**拼一个:
 *
 *   缩小(size < 现长):
 *     走链到第 size 字节所在簇 → `clear_fatent()` 释放它之后的整条链 →
 *     把"新的末尾簇"标成 EOC → 目录项 size = size → 回写 FAT 与目录项。
 *   扩大(size > 现长):
 *     先 `set_contents(…, buffer = 全零, pos = 老长, size = 差值)` 把老簇的尾段与新簇
 *     全部写成 0(显式清零: 新簇里可能是上一个文件的残留 —— 不读回 0 就是信息泄漏),
 *     再补写目录项 size = size(上游的 set_contents 不改目录项里的 size)。
 *     size == 现长 ⇒ 空操作(与 POSIX 一致)。
 *
 * 与上游写路径的语义关系: **用的是同一套原语**(get_fatent/set_fatent_value/clear_fatent/
 * flush_dirty_fat_buffer/flush_dir/set_contents), 没有第二套簇管理实现 —— 这是"截断与
 * 写入不会互相踩"的前提。
 *
 * 边界: 目录、不存在、超(每簇字节 × 可用簇)上界 ⇒ 各自的 errno; 不变更内容的一侧
 * (`size == 现长`)返回 0。
 */
int uboot_fat_truncate(const char *path, br_u64 size)
{
    fsdata *mydata = uboot_fat_fsdata();
    fat_itr *itr = NULL;
    dir_entry *dent;
    char *parent, *basename;
    char l_filename[UB_FAT_LFN_MAX];
    char path_buf[UB_FAT_PATH_MAX];
    loff_t old_size, actwrite = 0;
    __u32 curclust, nextclust;
    unsigned int bytesperclust;
    int ret;

    if (mydata == NULL)
        return -ENODEV;

    ret = ub_fat_path_copy(path_buf, sizeof(path_buf), path);
    if (ret)
        return ret;
    split_filename(path_buf, &parent, &basename);
    if (!uboot_fat_strlen(basename))
        return -EINVAL;
    if (normalize_longname(l_filename, basename))
        return -EINVAL;

    itr = br_malloc(sizeof(fat_itr));
    if (!itr)
        return -ENOMEM;

    ret = fat_itr_root(itr);
    if (ret)
        goto exit;

    total_sector = (int)uboot_fat_vol_total_sect();

    ret = fat_itr_resolve(itr, parent, TYPE_DIR);
    if (ret)
        goto exit;

    dent = find_directory_entry(itr, l_filename);
    if (!dent) {
        ret = -ENOENT;
        goto exit;
    }
    if (fat_itr_isdir(itr)) {
        ret = -EISDIR;                 /* 目录没有"文件长度"(POSIX: EISDIR) */
        goto exit;
    }
    if (size > 0xffffffffull) {        /* 目录项 size 是 32 位 */
        ret = -ENOSPC;
        goto exit;
    }

    old_size = (loff_t)FAT2CPU32(dent->size);
    if ((loff_t)size == old_size) {
        ret = 0;                       /* 空操作 */
        goto exit;
    }

    bytesperclust = mydata->clust_size * mydata->sect_size;

    if ((loff_t)size > old_size) {
        /* ---- 扩大: 用上游的写路径把 [old_size, size) 写成 0 ----
         * ★ 必须**分块**: set_contents() 会把 buffer 往前推, 而本件的零缓冲只有
         *   一个簇那么长(UB_FAT_MAX_CLUSTSIZE)。每次最多推进一个簇, 于是"推进量"
         *   永远落在零缓冲的界内。 */
        static u8 s_zero[UB_FAT_MAX_CLUSTSIZE];   /* BSS: 32 KiB, 恒零 */
        u64 need = (u64)size - (u64)old_size;
        u64 done = 0;

        while (done < need) {
            u64 chunk = need - done;

            if (chunk > (u64)bytesperclust)
                chunk = (u64)bytesperclust;

            ret = set_contents(mydata, dent, old_size + (loff_t)done,
                               s_zero, (loff_t)chunk, &actwrite);
            if (ret < 0) {
                ret = -EIO;
                goto exit;
            }
            if ((u64)actwrite != chunk) {
                log_err("FAT: truncate grow wrote %llu of %llu",
                        (unsigned long long)actwrite, (unsigned long long)chunk);
                ret = -EIO;
                goto exit;
            }
            done += chunk;
        }
    } else {
        /* ---- 缩小: 释放第 size 字节之后的簇链, 并把新的末尾标成 EOC ---- */
        __u32 newclust;
        u64 want = (u64)size;
        u64 covered = 0;

        curclust = START(dent);
        if (curclust == 0) {
            dent->size = uboot_fat_cpu_to_le32(0u);
            goto flush_out;
        }
        while (1) {
            const __u32 cl = get_fatent(mydata, curclust);

            covered += bytesperclust;
            if (covered >= want)
                break;
            if (CHECK_CLUST(cl, mydata->fatsize))
                break;                 /* 链比 size 短: 已经没什么可释放 */
            curclust = cl;
        }
        if (CHECK_CLUST(curclust, mydata->fatsize)) {
            ret = -EIO;                /* 链本身就是坏的 */
            goto exit;
        }
        nextclust = get_fatent(mydata, curclust);
        if (!CHECK_CLUST(nextclust, mydata->fatsize)) {
            ret = clear_fatent(mydata, nextclust);    /* 释放尾部 */
            if (ret < 0) {
                ret = -EIO;
                goto exit;
            }
        }
        if (want == 0u) {
            /* 截到 0: 整条链释放, 首簇清零 */
            ret = clear_fatent(mydata, curclust);
            if (ret < 0) {
                ret = -EIO;
                goto exit;
            }
            set_start_cluster(mydata, dent, 0);
        } else {
            /* 新的末尾簇标 EOC */
            if (mydata->fatsize == 12)
                newclust = 0xfff;
            else if (mydata->fatsize == 16)
                newclust = 0xffff;
            else
                newclust = 0xfffffff;
            set_fatent_value(mydata, curclust, newclust);
        }
    }

flush_out:
    /* 目录项里的长度是**唯一**的真值(链由它解释)⇒ 两处都要落盘 */
    dent->size = uboot_fat_cpu_to_le32((__u32)size);
    dentry_set_time(dent);

    ret = flush_dirty_fat_buffer(mydata);
    if (ret) {
        log_err("Error: flush fat buffer");
        ret = -EIO;
        goto exit;
    }
    ret = flush_dir(itr);

exit:
    br_free(itr);
    return ret;
}
