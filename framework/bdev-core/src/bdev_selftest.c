/*
 * brickOS prototype v0.2.0 — framework/bdev-core 的**自检套件**(src/bdev_selftest.c)
 *
 * 设计依据与裁定: `docs/decisions/0010-plugin-selftest.md`(自检与核心代码分离)、
 * `docs/decisions/0021-bdev-pci-emmc.md`(bdev 形状与 raw 面 = v2)。
 *
 * ## 它测什么, 与生产代码的边界在哪
 *   块设备子分类的**机制**(自编号 `TC-BDEV-*`): 名字/ops/几何的注册期校验、取回语义、
 *   注册表簿记、以及"bdev 条目没有文件面"(D21 ⇒ `/dev/<bdev>` 看得见打不开)。
 *   **不**测任何具体介质 —— 真实块的往返在驱动自己的套件里(如 `adapter/uboot-mmc`
 *   的 `TC-EMMC-*`), 那是"设备"的证据, 不是"子分类框架"的证据。
 *
 * ## 为什么大量用例走"合法名字 + 非法输入"这一手
 *   `br_bdev_register` 的校验在**入表之前**, 所以用"合法的、当前空闲的名字"配上非法
 *   ops/几何, 可以既覆盖每条拒绝分支、又**不真的登记**任何东西(注册表不被污染, 用例之间
 *   也就没有顺序耦合)。这是 cdev-core 的 `br_cdev_register('cdevx', <只有 open 的 ops>)`
 *   同一手法。
 *
 * ## 与 br_plugin_manager_selftest() 的契约
 *   返回**失败项数**(0 = 全绿); 日志自己打(`[BDEVCONF] PASS/FAIL ...` + `SUMMARY`)。
 *   失败不停机 —— 红绿由门禁判(`tests/gates.toml` 的 forbid 里有 `[BDEVCONF] FAIL`)。
 */
#include <br/dev/br_bdev.h>     /* 对外契约(br_bdev_ops_t / br_bdev_geom_t) */

#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>
#include <br/dev/br_dev.h>

#include "bdev_internal.h"

/* 自检入口原型(定义在本文件末尾; 生成物 `plugin_desc.c` 的 `.selftest` 钩子引用它)。 */
int bdev_core_selftest(void);

/* =====================================================================
 * 探针设备(只服务本文件, 不登记)
 *
 * geometry 由 priv 指向的 `br_bdev_geom_t` 原样回答 ⇒ 同一张 ops 表可以扮演"几何合法"
 * 与"几何各种非法"两种角色, 不需要为每个非法值写一张 ops 表。
 * ===================================================================== */

static int probe_geometry(void *priv, br_bdev_geom_t *g)
{
    if (priv == BR_NULL || g == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    *g = *(const br_bdev_geom_t *)priv;
    return 0;
}

static int probe_read(void *priv, br_u64 lba, void *buf, br_size_t n_lba)
{
    (void)priv; (void)lba; (void)buf; (void)n_lba;
    return 0;                       /* 从不被本套件调用: 校验路径不含 I/O */
}

static int probe_write(void *priv, br_u64 lba, const void *buf, br_size_t n_lba)
{
    (void)priv; (void)lba; (void)buf; (void)n_lba;
    return 0;
}

static int probe_flush(void *priv)
{
    (void)priv;
    return 0;
}

/* 四个必填槽位齐全的"好 ops"(唯一可变的部分是 geometry 经由 priv 注入) */
static const br_bdev_ops_t s_good_ops = {
    .read     = probe_read,
    .write    = probe_write,
    .flush    = probe_flush,
    .geometry = probe_geometry,
    .ioctl    = BR_NULL,
    .suspend  = BR_NULL,
    .resume   = BR_NULL,
};

/* 缺槽位的坏 ops(每张只缺一个, 用来钉"必填"逐条成立) */
static const br_bdev_ops_t s_no_read_ops = {
    .read = BR_NULL, .write = probe_write, .flush = probe_flush, .geometry = probe_geometry,
};
static const br_bdev_ops_t s_no_write_ops = {
    .read = probe_read, .write = BR_NULL, .flush = probe_flush, .geometry = probe_geometry,
};
static const br_bdev_ops_t s_no_flush_ops = {
    .read = probe_read, .write = probe_write, .flush = BR_NULL, .geometry = probe_geometry,
};
static const br_bdev_ops_t s_no_geom_ops = {
    .read = probe_read, .write = probe_write, .flush = probe_flush, .geometry = BR_NULL,
};

/* 合法的"几何基线"(512 B 扇区 / 8 扇区 / 上限 4 / 无对齐要求 / PIO) */
#define PROBE_GEOM_SECTOR   512u
#define PROBE_GEOM_SECTORS  8u
#define PROBE_GEOM_MAXRW    4u

/* =====================================================================
 * 一致性用例(自编号 TC-BDEV-*; 登记在 br-wa-test-001)
 * ===================================================================== */

static br_u32 s_bd_pass;
static br_u32 s_bd_fail;

static void bd_conf(br_bool ok, const char *tag, const char *what)
{
    if (ok == BR_TRUE) {
        s_bd_pass++;
        br_log_info("[BDEVCONF] PASS %s %s", tag, what);
    } else {
        s_bd_fail++;
        br_log_info("[BDEVCONF] FAIL %s %s", tag, what);
    }
}

/* 遍历注册表数 BR_CLASS_BDEV 条目(公开 API) */
static br_u32 bd_registry_count(void)
{
    br_u32 n = 0u;
    for (br_u32 i = 0u; i < br_dev_count(); i++) {
        const br_dev_class_entry_t *e = br_dev_entry_at(i);
        if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV) {
            n++;
        }
    }
    return n;
}

static const char *bd_first_name(void)
{
    for (br_u32 i = 0u; i < br_dev_count(); i++) {
        const br_dev_class_entry_t *e = br_dev_entry_at(i);
        if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV) {
            return br_dev_name_at(i);
        }
    }
    return BR_NULL;
}

int bdev_core_selftest(void)
{
    s_bd_pass = 0u;
    s_bd_fail = 0u;

    br_log_info("[BDEVCONF] bdev-core conformance (register/get/geometry/raw-face)");

    /* ---- TC-BDEV-001: 名字规则(合法 ops; 拒绝发生在入表之前) ---- */
    {
        br_bdev_geom_t g = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        const br_u32   n0 = br_dev_count();
        const int r_null = br_bdev_register(BR_NULL, &s_good_ops, &g);
        const int r_empty = br_bdev_register("", &s_good_ops, &g);
        const int r_upper = br_bdev_register("Bdev0", &s_good_ops, &g);
        const int r_slash = br_bdev_register("b/dev", &s_good_ops, &g);
        const int r_under = br_bdev_register("b_dev", &s_good_ops, &g);

        const br_bool ok = (r_null == BR_ERR(EINVAL)) &&
                           (r_empty == BR_ERR(EINVAL)) &&
                           (r_upper == BR_ERR(EINVAL)) &&
                           (r_slash == BR_ERR(EINVAL)) &&
                           (r_under == BR_ERR(EINVAL)) &&
                           (br_dev_count() == n0);
        bd_conf(ok, "TC-BDEV-001",
                "NULL/空/大写/斜杠/下划线名 ⇒ -EINVAL 且注册表不变");
    }

    /* ---- TC-BDEV-002: ops 必填槽位(合法空闲名 + 缺槽位 ⇒ -EINVAL, 不入表) ---- */
    {
        br_bdev_geom_t g = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        const br_u32   n0 = br_dev_count();
        const int r_ops  = br_bdev_register("bdevtest0", BR_NULL, &g);
        const int r_rd   = br_bdev_register("bdevtest0", &s_no_read_ops, &g);
        const int r_wr   = br_bdev_register("bdevtest0", &s_no_write_ops, &g);
        const int r_fl   = br_bdev_register("bdevtest0", &s_no_flush_ops, &g);
        const int r_gm   = br_bdev_register("bdevtest0", &s_no_geom_ops, &g);

        const br_bool ok = (r_ops == BR_ERR(EINVAL)) &&
                           (r_rd == BR_ERR(EINVAL)) &&
                           (r_wr == BR_ERR(EINVAL)) &&
                           (r_fl == BR_ERR(EINVAL)) &&
                           (r_gm == BR_ERR(EINVAL)) &&
                           (br_dev_count() == n0) &&
                           (br_bdev_get("bdevtest0", BR_NULL) == BR_NULL);
        bd_conf(ok, "TC-BDEV-002",
                "ops 空 / 缺 read|write|flush|geometry ⇒ -EINVAL 且名字仍空闲");
    }

    /* ---- TC-BDEV-003: 几何自洽(注册期调用 geometry 并逐条拒绝) ---- */
    {
        const br_u32 n0 = br_dev_count();
        /* 每项: {几何, 说明} —— geometry 由 priv 注入 */
        br_bdev_geom_t g_zero_sec = { 0u, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_not_pow2 = { 500u, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_zero_n   = { PROBE_GEOM_SECTOR, 0u, PROBE_GEOM_MAXRW, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_zero_rw  = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, 0u, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_rw_big   = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS,
                                      PROBE_GEOM_SECTORS + 1u, 0u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_align    = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 3u, BR_BDEV_F_PIO };
        br_bdev_geom_t g_both     = { PROBE_GEOM_SECTOR, PROBE_GEOM_SECTORS, PROBE_GEOM_MAXRW, 0u,
                                      BR_BDEV_F_DMA | BR_BDEV_F_PIO };

        const int r_sec  = br_bdev_register("bdevtest1", &s_good_ops, &g_zero_sec);
        const int r_np2  = br_bdev_register("bdevtest1", &s_good_ops, &g_not_pow2);
        const int r_n    = br_bdev_register("bdevtest1", &s_good_ops, &g_zero_n);
        const int r_rw0  = br_bdev_register("bdevtest1", &s_good_ops, &g_zero_rw);
        const int r_rwbg = br_bdev_register("bdevtest1", &s_good_ops, &g_rw_big);
        const int r_al   = br_bdev_register("bdevtest1", &s_good_ops, &g_align);
        const int r_both = br_bdev_register("bdevtest1", &s_good_ops, &g_both);

        const br_bool ok = (r_sec == BR_ERR(EINVAL)) &&
                           (r_np2 == BR_ERR(EINVAL)) &&
                           (r_n == BR_ERR(EINVAL)) &&
                           (r_rw0 == BR_ERR(EINVAL)) &&
                           (r_rwbg == BR_ERR(EINVAL)) &&
                           (r_al == BR_ERR(EINVAL)) &&
                           (r_both == BR_ERR(EINVAL)) &&
                           (br_dev_count() == n0);
        bd_conf(ok, "TC-BDEV-003",
                "sector=0/非 2 幂, n_sectors=0, max_rw=0/>n, align 非 2 幂, DMA|PIO 同置 ⇒ -EINVAL");
    }

    /* ---- TC-BDEV-004: 取回语义(未注册是 NULL, 且不改写出参) ---- */
    {
        void *sentinel = (void *)&s_bd_pass;      /* 任意非 NULL 哨兵 */
        void *priv = sentinel;

        const br_bdev_ops_t *ops = br_bdev_get("nosuchbdev", &priv);
        const br_bdev_ops_t *ops_null = br_bdev_get(BR_NULL, BR_NULL);

        const br_bool ok = (ops == BR_NULL) && (priv == sentinel) && (ops_null == BR_NULL);
        bd_conf(ok, "TC-BDEV-004",
                "未注册 ⇒ NULL 且 *priv 不变; br_bdev_get(NULL,NULL) ⇒ NULL");
    }

    /* ---- TC-BDEV-005: raw 文件面 = v2(遍历注册表: bdev 条目 open_file 恒 NULL) ----
     * 机械判据: 谁给 bdev 接上文件面(或让注册路径漏了 NULL), 这条就红(与 TC-CDEV-008 同型)。 */
    {
        br_bool all_null = BR_TRUE;
        for (br_u32 i = 0u; i < br_dev_count(); i++) {
            const br_dev_class_entry_t *e = br_dev_entry_at(i);
            if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV && e->open_file != BR_NULL) {
                all_null = BR_FALSE;
            }
        }
        bd_conf(all_null, "TC-BDEV-005",
                "所有 BR_CLASS_BDEV 条目的 open_file == NULL(D21: raw 块访问属 v2)");
    }

    /* ---- TC-BDEV-006: 活设备往返(组合含 bdev 时; 无 bdev 走"不适用") ---- */
    {
        const char *name = bd_first_name();
        if (name == BR_NULL) {
            bd_conf(BR_TRUE, "TC-BDEV-006", "组合内无 bdev ⇒ 不适用(平凡通过)");
        } else {
            const br_dev_class_entry_t *e = br_dev_lookup(name);
            void           *priv = BR_NULL;
            const br_bdev_ops_t *ops = br_bdev_get(name, &priv);
            br_bdev_geom_t  g = { 0u, 0u, 0u, 0u, 0u };
            const int       grc = (ops != BR_NULL) ? ops->geometry(priv, &g) : BR_ERR(EINVAL);
            const int       dup = br_bdev_register(name, ops, priv);

            const br_bool ok = (ops != BR_NULL) && (e != BR_NULL) &&
                               (ops == (const br_bdev_ops_t *)e->class_priv) &&
                               (grc == 0) && (g.sector_size > 0u) && (g.n_sectors > 0u) &&
                               (g.max_rw > 0u) && ((br_u64)g.max_rw <= g.n_sectors) &&
                               (dup == BR_ERR(EEXIST));
            bd_conf(ok, "TC-BDEV-006",
                    "取回 ops==条目 class_priv; 几何自洽; 重名重注册 ⇒ -EEXIST");
        }
    }

    /* ---- TC-BDEV-007: 池簿记(占用数 <= 容量, 且与注册表条目数一致) ---- */
    {
        const br_u32 cap  = bdev_internal_capacity();
        const br_u32 used = bdev_internal_slot_count();
        const br_u32 reg  = bd_registry_count();

        const br_bool ok = (cap >= 1u) && (used <= cap) && (used == reg);
        bd_conf(ok, "TC-BDEV-007",
                "slot_count <= capacity 且 slot_count == 注册表 bdev 条目数");
    }

    /* ---- TC-BDEV-008: flags 位域(活设备; 不认识的位 ⇒ 红) ---- */
    {
        const char *name = bd_first_name();
        if (name == BR_NULL) {
            bd_conf(BR_TRUE, "TC-BDEV-008", "组合内无 bdev ⇒ 不适用(平凡通过)");
        } else {
            void                *priv = BR_NULL;
            const br_bdev_ops_t *ops = br_bdev_get(name, &priv);
            br_bdev_geom_t       g = { 0u, 0u, 0u, 0u, 0u };
            const int            grc = (ops != BR_NULL) ? ops->geometry(priv, &g) : BR_ERR(EINVAL);
            const br_u32         known = BR_BDEV_F_READONLY | BR_BDEV_F_STACKED |
                                         BR_BDEV_F_DMA | BR_BDEV_F_PIO;
            const br_u32         both = BR_BDEV_F_DMA | BR_BDEV_F_PIO;

            const br_bool ok = (grc == 0) && ((g.flags & ~known) == 0u) &&
                               ((g.flags & both) != both);
            bd_conf(ok, "TC-BDEV-008",
                    "geometry.flags 只含已知位且 DMA/PIO 不同置");
        }
    }

    br_log_info("[BDEVCONF] SUMMARY pass=%u fail=%u total=%u",
                s_bd_pass, s_bd_fail, s_bd_pass + s_bd_fail);
    return (int)s_bd_fail;
}
