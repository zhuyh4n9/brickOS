/*
 * third_party/uboot-mmc — 自检套件(TC-EMMC-*)
 *
 * 设计依据: `docs/decisions/0010-plugin-selftest.md`(自检与生产分离)、
 * `docs/decisions/0021-bdev-pci-emmc.md` D-4/D-5(中断 + ADMA2/PIO + flush 口径)。
 *
 * 只经**公开面**驱动: `br_bdev_get`(bdev-core 类 API)+ `br_dev_*`(dev-core 注册表)+
 * 器件 ioctl 编码。不看任何内部状态 —— 否则"经 bdev 真的能写进去再读出来"这件事就没有判据。
 *
 * ## 为什么每次都"先写后读"
 *   门禁的 eMMC 后备镜像是**跨次运行持久**的(build/emmc0.img 只在大小不对时重建),
 *   所以用例**不能**假设任何扇区是 0 或某模式 —— 判据一律是"我写进去的, 我读出来一样"。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志打 `[EMMCCONF] PASS/FAIL` + `SUMMARY`。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_bdev.h>
#include <br/dev/br_dev.h>

#include "emmc_internal.h"

int uboot_mmc_selftest(void);

/* 8 KiB 双缓冲(= 16 扇区), 4 字节对齐 ⇒ ADMA2 可用 */
static br_u32 s_a[(16u * 512u) / 4u];
static br_u32 s_b[(16u * 512u) / 4u];

static br_u32 s_em_pass;
static br_u32 s_em_fail;

static void em_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_em_pass++;
        br_log_info("[EMMCCONF] PASS %s %s", tag, what);
    } else {
        s_em_fail++;
        br_log_info("[EMMCCONF] FAIL %s %s", tag, what);
    }
}

static void em_fill(br_u32 *p, br_u32 words, br_u32 seed)
{
    for (br_u32 i = 0u; i < words; i++) {
        p[i] = seed + i;
    }
}

static br_bool em_same(const br_u32 *a, const br_u32 *b, br_u32 words)
{
    for (br_u32 i = 0u; i < words; i++) {
        if (a[i] != b[i]) {
            return BR_FALSE;
        }
    }
    return BR_TRUE;
}

int uboot_mmc_selftest(void)
{
    s_em_pass = 0u;
    s_em_fail = 0u;

    br_log_info("[EMMCCONF] uboot-mmc conformance (bdev round-trip / ADMA2 / PIO / flush)");

    void                 *priv = BR_NULL;
    const br_bdev_ops_t  *ops  = br_bdev_get(BR_EMMC_DEV_NAME, &priv);
    br_bdev_geom_t        g    = { 0u, 0u, 0u, 0u, 0u };
    const int             grc  = (ops != BR_NULL) ? ops->geometry(priv, &g) : BR_ERR(BR_EINVAL);

    /* ---- TC-EMMC-001: 设备经 bdev 类 API 可见且几何自洽 ---- */
    em_conf((ops != BR_NULL) && (priv != BR_NULL) && (grc == 0) &&
            (g.sector_size == 512u) && (g.n_sectors >= 1024u) &&
            (g.max_rw == BR_EMMC_MAX_RW_SECTORS) &&
            ((g.flags & (BR_BDEV_F_DMA | BR_BDEV_F_PIO)) != 0u),
            "TC-EMMC-001", "br_bdev_get(emmc0) 命中; 512 B 扇区; 几何自洽且有 DMA/PIO 位");

    /* ---- TC-EMMC-002: raw 文件面 = v2(dev-core 条目 open_file == NULL) ---- */
    {
        const br_dev_class_entry_t *e = br_dev_lookup(BR_EMMC_DEV_NAME);
        em_conf((e != BR_NULL) && (e->class_id == (br_u16)BR_CLASS_BDEV) && (e->open_file == BR_NULL),
                "TC-EMMC-002", "emmc0 条目 open_file == NULL(看得见打不开, D21)");
    }

    /* ---- TC-EMMC-003: 默认(DMA)通路 8 扇区写后读回一致 ---- */
    {
        em_fill(s_a, 8u * 128u, 0xE3E30000u);
        br_bool ok = BR_FALSE;
        if (ops != BR_NULL) {
            const int w = ops->write(priv, 64u, s_a, 8u);
            const int r = ops->read(priv, 64u, s_b, 8u);
            ok = (w == 8) && (r == 8) && em_same(s_a, s_b, 8u * 128u);
        }
        em_conf(ok, "TC-EMMC-003", "默认通路: 8 扇区 write→read 逐字一致");
    }

    /* ---- TC-EMMC-004: 越界访问被拒(不发给卡) ---- */
    {
        int rc = 0;
        if ((ops != BR_NULL) && (grc == 0)) {
            rc = ops->read(priv, g.n_sectors, s_b, 1u);
        }
        em_conf(rc < 0, "TC-EMMC-004", "read(lba == n_sectors) ⇒ 负 errno(越界不落卡)");
    }

    /* ---- TC-EMMC-005: 运行期切 PIO, 几何随之报 PIO ---- */
    {
        const br_u32 cmd = BR_IOW(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_SET_PATH, br_u32);
        const br_u32 path = BR_MMC_PATH_PIO;
        br_bdev_geom_t g2 = { 0u, 0u, 0u, 0u, 0u };
        br_bool ok = BR_FALSE;
        if (ops != BR_NULL) {
            const int r_set  = ops->ioctl(priv, cmd, (void *)&path);
            const int r_geom = ops->geometry(priv, &g2);
            ok = (r_set == 0) && (r_geom == 0) && ((g2.flags & BR_BDEV_F_PIO) != 0u) &&
                 ((g2.flags & BR_BDEV_F_DMA) == 0u);
        }
        em_conf(ok, "TC-EMMC-005", "SET_PATH(PIO) 后 geometry.flags 恰好报 PIO");
    }

    /* ---- TC-EMMC-006: PIO 通路 8 扇区写后读回一致 ---- */
    {
        em_fill(s_a, 8u * 128u, 0x06060000u);
        br_bool ok = BR_FALSE;
        if (ops != BR_NULL) {
            const int w = ops->write(priv, 128u, s_a, 8u);
            const int r = ops->read(priv, 128u, s_b, 8u);
            ok = (w == 8) && (r == 8) && em_same(s_a, s_b, 8u * 128u);
        }
        em_conf(ok, "TC-EMMC-006", "PIO 通路: 8 扇区 write→read 逐字一致");
    }

    /* ---- TC-EMMC-007: 切回 ADMA2 并做 16 扇区(> PIO 阈值)多块 DMA ---- */
    {
        const br_u32 cmd = BR_IOW(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_SET_PATH, br_u32);
        const br_u32 path = BR_MMC_PATH_DMA;
        br_bdev_geom_t g2 = { 0u, 0u, 0u, 0u, 0u };
        em_fill(s_a, 16u * 128u, 0xD0D00000u);
        br_bool ok = BR_FALSE;
        if (ops != BR_NULL) {
            const int r_set  = ops->ioctl(priv, cmd, (void *)&path);
            const int r_geom = ops->geometry(priv, &g2);
            const int w = ops->write(priv, 256u, s_a, 16u);
            const int r = ops->read(priv, 256u, s_b, 16u);
            ok = (r_set == 0) && (r_geom == 0) && ((g2.flags & BR_BDEV_F_DMA) != 0u) &&
                 (w == 16) && (r == 16) && em_same(s_a, s_b, 16u * 128u);
        }
        em_conf(ok, "TC-EMMC-007", "SET_PATH(DMA) 后 16 扇区 ADMA2 多块 write→read 一致");
    }

    /* ---- TC-EMMC-008: flush 与 GET_INFO 的回答 ---- */
    {
        const br_u32 cmd = BR_IOR(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_GET_INFO, br_emmc_info_t);
        br_emmc_info_t info;
        br_bool ok = BR_FALSE;
        if (ops != BR_NULL) {
            const int rf = ops->flush(priv);
            const int ri = ops->ioctl(priv, cmd, (void *)&info);
            ok = (rf == 0) && (ri == 0) && (info.base != 0u) &&
                 (info.capacity_sectors == g.n_sectors) &&
                 (info.sector_size == 512u) && (info.path == BR_MMC_PATH_DMA) &&
                 (info.bus_width == 4u || info.bus_width == 8u || info.bus_width == 1u);
        }
        em_conf(ok, "TC-EMMC-008", "flush == 0; GET_INFO 的容量/扇区/BAR/通路回答自洽");
    }

    br_log_info("[EMMCCONF] SUMMARY pass=%u fail=%u total=%u",
                s_em_pass, s_em_fail, s_em_pass + s_em_fail);
    return (int)s_em_fail;
}
