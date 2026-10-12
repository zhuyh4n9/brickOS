/* adapter/uboot-mmc — `<blk.h>` 的**接口类型壳**(不是 u-boot 那份的复制)
 *
 * ============================== 为什么是"壳" ==============================
 * 上游 `drivers/mmc/` 下的原件 要 `struct blk_desc`、`U_BOOT_LEGACY_BLK()`、`blk_dselect_hwpart()`
 * 这些**块层接口类型**。为什么不 vendor 上游 `include/blk.h`:
 *
 *   u-boot 的 `include/blk.h` 第 10-12 行是
 *       #include <bouncebuf.h>
 *       #include <dm/uclass-id.h>
 *       #include <efi.h>
 *   ⇒ vendor 它就等于把 `<bouncebuf.h>`(整族 bounce buffer 机制)与 `<efi.h>`
 *   (EFI 引导服务/`efi_guid_t`/`efi_status_t` 整族)拖进来。而本件只用得到其中
 *   **一个结构、一个宏、两个函数原型** —— 那正是 ADR-0026 D-3 说的"给忠实但最小的
 *   接口类型", 与 `adapter/uboot-erofs/compat/part.h` 的处置同款。
 *
 * ★ "忠实"的含义(逐条对齐上游, 不是自创):
 *   - `lbaint_t` 与 `LBAF`/`LBAFU` 的取法(`CONFIG_SYS_64BIT_LBA` 未定义 ⇒ `ulong`,
 *     LP64 上就是 64 位 —— 与 aarch64 u-boot 缺省一致);
 *   - `struct blk_desc` 的**非 BLK 字段顺序**: 本件 `CONFIG_BLK` 有意不定义, 于是上游
 *     结构体走的是 `#else` 那一支(尾部三个 legacy 回调 + `priv`), 我们逐字段照排;
 *   - `BLK_VEN_SIZE`/`BLK_PRD_SIZE`/`BLK_REV_SIZE`: **值必须与上游一致** ——
 *     `mmc_startup()` 会 `sprintf(bdesc->vendor, ...)`, 缓冲区大小是布局的一部分;
 *   - `U_BOOT_LEGACY_BLK()`: 上游把它展开成"往 `.u_boot_list_2_blk_driver_2_<name>`
 *     段放一条 `struct blk_driver`"。本件**没有**那套链接段枚举(我们不枚举块驱动,
 *     而是由 glue 直接持有 `struct mmc *`), 所以这里把它降级成一个**普通的全局对象**:
 *     `mmc_legacy.c` 末尾的 `U_BOOT_LEGACY_BLK(mmc) = { ... };` 因此仍然逐字成立,
 *     只是没人去遍历它(`--gc-sections` 会把它丢掉)。这是**已知且故意的行为偏离**,
 *     登记在 `ADAPTER-NOTES.md` 的"偏离"一节。
 *
 * ★ `enum uclass_id` 不在这里定义: 它是零依赖的上游原件(`third_party/.../dm/uclass-id.h`),
 *   所以 `compat/dm/uclass-id.h` 直接转发它。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H

#include "mmc_env.h"
#include <dm/uclass-id.h>

/* ---- 逻辑块地址类型(上游 blk.h 的取法) ---- */
#ifdef CONFIG_SYS_64BIT_LBA
typedef uint64_t lbaint_t;
#define LBAFlength "ll"
#else
typedef ulong lbaint_t;          /* aarch64 LP64: ulong 就是 64 位 */
#define LBAFlength "l"
#endif
#define LBAF  "%" LBAFlength "x"
#define LBAFU "%" LBAFlength "u"

#define DEFAULT_BLKSZ     512

struct udevice;                  /* 前置声明: 本件没有 DM ⇒ 只作类型存在 */

#define BLK_VEN_SIZE      40
#define BLK_PRD_SIZE      20
#define BLK_REV_SIZE      8

#define PART_FORMAT_PCAT  0x1
#define PART_FORMAT_GPT   0x2

/* 与上游同值(enum sig_type); 本件不做分区表识别, 但结构体字段要有 */
enum sig_type {
    SIG_TYPE_NONE,
    SIG_TYPE_MBR,
    SIG_TYPE_GUID,
    SIG_TYPE_COUNT
};

/*
 * 块设备描述符。★ 这里是本件**与上游唯一的实质接口**: 上游 `mmc.c` 往它里面填
 * `blksz`/`log2blksz`/`lba`/`vendor`/`product`/`revision`/`hwpart`, 并从它读
 * `devnum` 去找回 `struct mmc`; 我们的 glue 也从它读 `lba`(容量)与 `blksz`。
 * 字段顺序与上游 `include/blk.h` 的 `!CONFIG_BLK` 形态逐个一致。
 */
struct blk_desc {
    enum uclass_id  uclass_id;   /* type of the interface */
    int             devnum;      /* device number */
    unsigned char   part_type;   /* partition type */
    unsigned char   target;      /* target SCSI ID */
    unsigned char   lun;         /* target LUN */
    unsigned char   hwpart;      /* HW partition, e.g. for eMMC */
    unsigned char   type;        /* device type */
    unsigned char   removable;   /* removable device */
    bool            lba48;       /* device can use 48bit addr (ATA/ATAPI v7) */
    unsigned char   atapi;       /* Use ATAPI protocol */
    unsigned char   bb;          /* Use bounce buffer */
    lbaint_t        lba;         /* number of blocks */
    unsigned long   blksz;       /* block size */
    int             log2blksz;   /* for convenience: log2(blksz) */
    char            vendor[BLK_VEN_SIZE + 1];   /* device vendor string */
    char            product[BLK_PRD_SIZE + 1];  /* device product number */
    char            revision[BLK_REV_SIZE + 1]; /* firmware revision */
    enum sig_type   sig_type;    /* Partition table signature type */
    union {
        uint32_t mbr_sig;        /* MBR integer signature */
        char     guid_sig[16];   /* GPT GUID Signature(上游是 efi_guid_t; 本件不解析,
                                  * 但**保留 16 字节**以维持布局不漂移) */
    };
    /* ---- `!CONFIG_BLK` 支: 传统块回调 ---- */
    unsigned long (*block_read)(struct blk_desc *block_dev, lbaint_t start,
                                lbaint_t blkcnt, void *buffer);
    unsigned long (*block_write)(struct blk_desc *block_dev, lbaint_t start,
                                 lbaint_t blkcnt, const void *buffer);
    unsigned long (*block_erase)(struct blk_desc *block_dev, lbaint_t start,
                                 lbaint_t blkcnt);
    void *priv;                  /* driver private struct pointer */
};

/* 上游 `CONFIG_BLK` 未启用时的 block driver 表形态; 见文件头"U_BOOT_LEGACY_BLK"。 */
struct blk_driver {
    const char *uclass_idname;
    enum uclass_id uclass_id;
    int max_devs;
    struct blk_desc *desc;
    int (*get_dev)(int devnum, struct blk_desc **descp);
    int (*select_hwpart)(struct blk_desc *desc, int hwpart);
};

/*
 * ★ 行为偏离(见文件头末段): 上游 = 往链接段里登记一条 `blk_driver`; 本件 = 一个
 *   普通的全局对象。`mmc_legacy.c` 的原文因此仍然逐字成立, 但**没有**"按 uclass_id
 *   查块驱动"这条机制 —— 本件不需要它(glue 自己持有实例)。
 */
#define U_BOOT_LEGACY_BLK(__name) \
    struct blk_driver __attribute__((unused)) uboot_mmc_legacy_blk_ ## __name

/* glue 兑现(见 glue/emmc_bdev.c): 这两个是上游 mmc 块层路径上的挂点。
 * `mmc_bread()` 每次读都要先 `blk_dselect_hwpart(block_dev, block_dev->hwpart)`。 */
int blk_dselect_hwpart(struct blk_desc *desc, int hwpart);
int blk_select_hwpart_devnum(enum uclass_id uclass_id, int devnum, int hwpart);

#endif /* BR_ADAPTER_UBOOT_MMC_SHIM_BLK_H */
