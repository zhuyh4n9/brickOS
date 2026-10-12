/*
 * adapter/uboot-mmc — 自检套件(TC-EMMC-*)
 *
 * 设计依据: `docs/decisions/0010-plugin-selftest.md`(自检与生产分离)、
 * `docs/decisions/0021-bdev-pci-emmc.md` D-4/D-5(中断 + ADMA2/PIO + flush 口径)、
 * `docs/decisions/0022-erofs-fat.md`(四块卡的分工与"擦写区"约定)。
 *
 * 只经**公开面**驱动: `br_bdev_get`(bdev-core 类 API)+ `br_dev_*`(dev-core 注册表)+
 * 器件 ioctl 编码。不看任何内部状态 —— 否则"经 bdev 真的能写进去再读出来"这件事就没有判据。
 *
 * ## 为什么每次都"先写后读"
 *   门禁的后备镜像里, `emmc0/1/2` 上**装着活的 FAT 卷**(由宿主用 mkfs.fat + mcopy 造好),
 *   `emmc3` 上装着 EROFS 镜像。所以设备级用例**只在 `emmc3` 的擦写区**写
 *   (`BR_EMMC_SCRATCH_LBA` 起的 512 KiB, 远在 EROFS 镜像之后) —— 判据一律是"我写进去的,
 *   我读出来一样", 不假设任何扇区是 0 或某模式。**不许**往 emmc0/1/2 写: 那会撕掉 FAT 卷,
 *   而本套件跑在 FAT/EROFS 套件之后(自检相位) ⇒ 撕掉的是别人已经用过的数据。
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

/* 16 KiB 双缓冲(= 32 扇区), 4 字节对齐 ⇒ ADMA2 可用 */
static br_u32 s_a[(32u * 512u) / 4u];
static br_u32 s_b[(32u * 512u) / 4u];

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

/* `emmc<d>`; 与 emmc_bdev.c 的命名规则同一处真值(各自 5 行, 不跨文件共享一个可变缓冲) */
static void em_name(char *out, br_u32 idx)
{
    out[0] = 'e'; out[1] = 'm'; out[2] = 'm'; out[3] = 'c';
    out[4] = (char)('0' + (char)(idx % 10u));
    out[5] = '\0';
}

int uboot_mmc_selftest(void)
{
    s_em_pass = 0u;
    s_em_fail = 0u;

    br_log_info("[EMMCCONF] uboot-mmc conformance (4 cards / bdev round-trip / ADMA2 / PIO / flush)");

    /* ---- 四块卡都经 bdev 类 API 可见, 几何各自自洽 ---- */
    const br_bdev_ops_t *ops[BR_EMMC_MAX_DEVS];
    void                *priv[BR_EMMC_MAX_DEVS];
    br_bdev_geom_t       geo[BR_EMMC_MAX_DEVS];
    br_u32               ok_geo = 0u;

    for (br_u32 i = 0u; i < BR_EMMC_MAX_DEVS; i++) {
        char nm[BR_EMMC_NAME_MAX];
        em_name(nm, i);
        priv[i] = BR_NULL;
        ops[i]  = br_bdev_get(nm, &priv[i]);
        geo[i]  = (br_bdev_geom_t){ 0u, 0u, 0u, 0u, 0u };
        if (ops[i] != BR_NULL && priv[i] != BR_NULL &&
            ops[i]->geometry(priv[i], &geo[i]) == 0 &&
            geo[i].sector_size == 512u && geo[i].n_sectors >= 1024u &&
            geo[i].max_rw == BR_EMMC_MAX_RW_SECTORS &&
            ((geo[i].flags & (BR_BDEV_F_DMA | BR_BDEV_F_PIO)) != 0u)) {
            ok_geo++;
        }
    }
    em_conf(ok_geo == BR_EMMC_MAX_DEVS, "TC-EMMC-001",
            "emmc0..emmc3 四块卡都命中且几何自洽(512 B 扇区 / max_rw / DMA|PIO 位)");

    /* 擦写区必须在 EROFS 卡上, 且容量够本套件最大偏移(128 + 32 扇区) */
    const char *assets = BR_EMMC_ASSETS_DEV;
    br_u32 assets_idx = (br_u32)(assets[4] - '0');
    /* 擦写区整段(含本套件最大偏移 128 + 32 扇区)必须落在卡内 —— 越界用例会失败,
     * 而不是"静默写到卡外"。与 EROFS 镜像不重叠这条性质由造镜像脚本断言(见文件头)。 */
    em_conf((assets_idx < BR_EMMC_MAX_DEVS) &&
            (ops[assets_idx] != BR_NULL) &&
            (BR_EMMC_SCRATCH_LBA + 160u < geo[assets_idx].n_sectors),
            "TC-EMMC-002", "擦写区(emmc3 的 LBA 4096 起, 512 KiB)整段落在卡内");

    /* ---- 其余两条: dev-core 条目形状与 raw 文件面口径(D21: 看得见打不开) ---- */
    {
        br_u32 hits = 0u;
        for (br_u32 i = 0u; i < BR_EMMC_MAX_DEVS; i++) {
            char nm[BR_EMMC_NAME_MAX];
            em_name(nm, i);
            const br_dev_class_entry_t *e = br_dev_lookup(nm);
            if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV && e->open_file == BR_NULL) {
                hits++;
            }
        }
        em_conf(hits == BR_EMMC_MAX_DEVS, "TC-EMMC-003",
                "四块卡的 dev-core 条目 class == bdev 且 open_file == NULL(D21)");
    }

    /* ---- 004: 越界访问被拒(不发给卡) ---- */
    {
        int rc = 0;
        if (ops[assets_idx] != BR_NULL) {
            rc = ops[assets_idx]->read(priv[assets_idx], geo[assets_idx].n_sectors, s_b, 1u);
        }
        em_conf(rc < 0, "TC-EMMC-004", "read(lba == n_sectors) ⇒ 负 errno(越界不落卡)");
    }

    /* ---- 005: 默认(DMA)通路: 擦写区 8 扇区写后读回一致 ---- */
    {
        em_fill(s_a, 8u * 128u, 0xE3E30000u);
        br_bool ok = BR_FALSE;
        if (ops[assets_idx] != BR_NULL) {
            const int w = ops[assets_idx]->write(priv[assets_idx], BR_EMMC_SCRATCH_LBA, s_a, 8u);
            const int r = ops[assets_idx]->read(priv[assets_idx], BR_EMMC_SCRATCH_LBA, s_b, 8u);
            ok = (w == 8) && (r == 8) && em_same(s_a, s_b, 8u * 128u);
        }
        em_conf(ok, "TC-EMMC-005", "默认 ADMA2 通路: 擦写区 8 扇区 write→read 逐字一致");
    }

    /* ---- 006: 在**一块**卡上切 PIO: 该卡几何随之报 PIO, 别的卡仍报 DMA ---- */
    {
        const br_u32 cmd = BR_IOW(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_SET_PATH, br_u32);
        const br_u32 path = BR_MMC_PATH_PIO;
        br_bdev_geom_t g3 = (br_bdev_geom_t){ 0u, 0u, 0u, 0u, 0u };
        br_bdev_geom_t g0 = (br_bdev_geom_t){ 0u, 0u, 0u, 0u, 0u };
        br_bool ok = BR_FALSE;
        if (ops[assets_idx] != BR_NULL && ops[0] != BR_NULL) {
            const int r_set  = ops[assets_idx]->ioctl(priv[assets_idx], cmd, (void *)&path);
            const int r_g3   = ops[assets_idx]->geometry(priv[assets_idx], &g3);
            const int r_g0   = ops[0]->geometry(priv[0], &g0);
            ok = (r_set == 0) && (r_g3 == 0) && (r_g0 == 0) &&
                 ((g3.flags & BR_BDEV_F_PIO) != 0u) && ((g3.flags & BR_BDEV_F_DMA) == 0u) &&
                 ((g0.flags & BR_BDEV_F_DMA) != 0u) && ((g0.flags & BR_BDEV_F_PIO) == 0u);
        }
        em_conf(ok, "TC-EMMC-006", "SET_PATH(PIO) 只改该卡: 该卡报 PIO, emmc0 仍报 DMA(多实例)");
    }

    /* ---- 007: PIO 通路 8 扇区写后读回一致 ---- */
    {
        em_fill(s_a, 8u * 128u, 0x06060000u);
        br_bool ok = BR_FALSE;
        if (ops[assets_idx] != BR_NULL) {
            const br_u64 lba = BR_EMMC_SCRATCH_LBA + 64u;
            const int w = ops[assets_idx]->write(priv[assets_idx], lba, s_a, 8u);
            const int r = ops[assets_idx]->read(priv[assets_idx], lba, s_b, 8u);
            ok = (w == 8) && (r == 8) && em_same(s_a, s_b, 8u * 128u);
        }
        em_conf(ok, "TC-EMMC-007", "PIO 通路: 8 扇区 write→read 逐字一致");
    }

    /* ---- 008: 切回 ADMA2 做 32 扇区多块 DMA + flush + GET_INFO 回答自洽 ---- */
    {
        const br_u32 cmd_set = BR_IOW(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_SET_PATH, br_u32);
        const br_u32 cmd_inf = BR_IOR(BR_EMMC_IOC_MAGIC, BR_EMMC_IOC_GET_INFO, br_emmc_info_t);
        const br_u32 path = BR_MMC_PATH_DMA;
        br_emmc_info_t info;
        em_fill(s_a, 32u * 128u, 0xD0D00000u);
        br_bool ok = BR_FALSE;
        if (ops[assets_idx] != BR_NULL) {
            const br_u64 lba = BR_EMMC_SCRATCH_LBA + 128u;
            const int r_set = ops[assets_idx]->ioctl(priv[assets_idx], cmd_set, (void *)&path);
            const int w = ops[assets_idx]->write(priv[assets_idx], lba, s_a, 32u);
            const int r = ops[assets_idx]->read(priv[assets_idx], lba, s_b, 32u);
            const int rf = ops[assets_idx]->flush(priv[assets_idx]);
            const int ri = ops[assets_idx]->ioctl(priv[assets_idx], cmd_inf, (void *)&info);
            ok = (r_set == 0) && (w == 32) && (r == 32) && em_same(s_a, s_b, 32u * 128u) &&
                 (rf == 0) && (ri == 0) && (info.base != 0u) &&
                 (info.capacity_sectors == geo[assets_idx].n_sectors) &&
                 (info.sector_size == 512u) && (info.path == BR_MMC_PATH_DMA) &&
                 (info.bus_width == 4u || info.bus_width == 8u || info.bus_width == 1u);
        }
        em_conf(ok, "TC-EMMC-008",
                "SET_PATH(DMA) 后 32 扇区 ADMA2 多块一致; flush == 0; GET_INFO 自洽");
    }

    br_log_info("[EMMCCONF] SUMMARY pass=%u fail=%u total=%u",
                s_em_pass, s_em_fail, s_em_pass + s_em_fail);
    return (int)s_em_fail;
}
