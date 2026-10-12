/*
 * brickOS prototype v0.2.0 — 块设备子分类(framework/bdev-core)
 *
 * 设计依据: `docs/7-storage/7-02-bdev.md`
 *   §1(块设备类: 几何 + 扇区 read/write/flush + `br_bdev_register`/`br_bdev_get`;
 *     "bdev-core API: 块设备注册进 dev-core 的命名空间" —— 依赖 dev-core 的由来)
 *   §2(SD-9 分区映射器 = v1.x → 本刀只留设计)
 *   §3(SD-8 page cache 是**堆叠插件**, 本件只提供"上层看到同一 `br_bdev_ops`"的地基)
 *   §4(DMA/对齐/cache 约定: `align` 非零时调用方负责对齐, 否则驱动内部 bounce)
 * 逐条裁定与偏离(含 `BR_BDEV_F_DMA/PIO` 两位的来历)见 `docs/decisions/0021-bdev-pci-emmc.md`。
 *
 * ## 本文件的三件职责(别的都不做)
 *   ① `br_bdev_register`: 校验(名字/ops 必填槽位/几何自洽)→ 在 dev-core 登记一条
 *      `BR_CLASS_BDEV` 条目(`open_file = BR_NULL` —— D21: raw 块访问属 v2);
 *   ② `br_bdev_get`: 按名取回 `{ops, priv}`(FS/堆叠插件的绑定口);
 *   ③ 静态池与簿记(原型口径: 无动态分配, 与 dev-core/cdev-core 同款)。
 *
 * ## 为什么"注册期调一次 geometry"
 *   块设备的必填槽位里 `flush` 是**语义前提**(page cache 依赖它落介质, SD-8), 而"是否是
 *   只读介质"这种自洽性只有几何答得出来。与其把一堆半成品放进来等第一次 I/O 才炸, 不如
 *   在注册期把驱动叫一次: 几何自洽是**注册的前置条件**(与 cdev 的 flash 子型校验几何同型,
 *   只是它的几何是形参、这里是个 op)。
 *   代价: 驱动的 `geometry` 必须能在 CORE 相(注册时)回答 —— 这对"设备先初始化、再注册"
 *   的时序是自然的, 也是 v1 唯一允许的时序。
 *
 * ## 为什么 dmA/PIO 是**几何**里的位而不是 ops 里的字段
 *   主控要求"数据传输需要指出 DMA/PIO"(ADR-0021 D-4)。几何是**每次调用都可重新回答**的
 *   观测面 ⇒ 运行期切换 DMA/PIO 之后, 消费者下一次 `geometry` 就能看到新值; 若做成 ops 的
 *   常量字段, 切换就得改 ops 表(而 ops 是 const、布局入 golden)。这也让"当前实际通路"
 *   与"几何"在同一处被读到, 不会出现两个真值。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_types.h>

#include <br/dev/br_bdev.h>
#include <br/dev/br_dev.h>

#include "bdev_internal.h"     /* 自检用的内部视图(见该头; 生产路径不用它) */

/* 生命周期钩子原型(名 = symbol_prefix + 相; 生成物 `plugin_desc.c` 引用; 不进 [[export]]) */
int bdev_core_early_init(void);
int bdev_core_init(void);
int bdev_core_start(void);

/* ==================================================================== 静态池 */

#define BR_BDEV_MAX   8u   /* 块设备上界(原型: 静态上界, 与 BR_CDEV_MAX 同款"够用即停") */

/*
 * 一条 bdev 的注册记录。交给 dev-core 的 `dev_priv` 是**槽位指针**(不是驱动私有本身)——
 * 这样"条目属于本件"这件事有指针同一性的判据(与 cdev-core 同款), 且将来给 bdev 加
 * open_file 钩子时钩子也能同时看到 ops 与驱动私有。
 */
typedef struct bdev_slot {
    br_bool             used;
    const br_bdev_ops_t *ops;
    void                *priv;
} bdev_slot_t;

static bdev_slot_t s_bdevs[BR_BDEV_MAX];
static br_u32      s_bdev_n;

/* ==================================================================== 几何校验 */

static br_bool bdev_is_pow2(br_u32 v)
{
    return ((v != 0u) && ((v & (v - 1u)) == 0u)) ? BR_TRUE : BR_FALSE;
}

/*
 * 几何自洽的判据(注册期执法; 每一条都对应一种"看起来能算、其实算错"的表):
 *   - sector_size 是 2 的幂(非 2 的幂几乎必然是单位写错) —— 且非 0;
 *   - n_sectors > 0(0 容量没有可读的 LBA);
 *   - 0 < max_rw <= n_sectors(上限大于介质长度 = 有一条永远失败的路径);
 *   - align 为 0 或 2 的幂(0 = 任意, 设计 `7-02` §4);
 *   - DMA/PIO 两位**不可同置**(自相矛盾); 都不置 = "未声明通路", 放行。
 */
static br_bool bdev_geom_ok(const br_bdev_geom_t *g)
{
    if (g->sector_size == 0u || bdev_is_pow2(g->sector_size) == BR_FALSE) {
        return BR_FALSE;
    }
    if (g->n_sectors == (br_u64)0u) {
        return BR_FALSE;
    }
    if (g->max_rw == 0u || (br_u64)g->max_rw > g->n_sectors) {
        return BR_FALSE;
    }
    if (g->align != 0u && bdev_is_pow2(g->align) == BR_FALSE) {
        return BR_FALSE;
    }
    if ((g->flags & (BR_BDEV_F_DMA | BR_BDEV_F_PIO)) == (BR_BDEV_F_DMA | BR_BDEV_F_PIO)) {
        return BR_FALSE;
    }
    return BR_TRUE;
}

/* ==================================================================== 槽位查找 */

static bdev_slot_t *bdev_find(const char *name)
{
    const br_dev_class_entry_t *e = br_dev_lookup(name);

    if (e == BR_NULL || e->class_id != (br_u16)BR_CLASS_BDEV) {
        return BR_NULL;
    }

    /* 同一个 class_id 下将来可能有子型(如分区映射器)⇒ 用"槽位是否落在本件池里"区分,
     * 而不是拿 class_priv 比较(两个驱动的 ops 恰好同址的理论可能)。 */
    bdev_slot_t *slot = (bdev_slot_t *)e->dev_priv;
    for (br_u32 i = 0u; i < s_bdev_n; i++) {
        if (&s_bdevs[i] == slot && s_bdevs[i].used == BR_TRUE) {
            return slot;
        }
    }
    return BR_NULL;
}

/* ==================================================================== 注册与取回 */

int br_bdev_register(const char *name, const br_bdev_ops_t *ops, void *priv)
{
    if (ops == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    /* 必填槽位(设计 `7-02` §1 的四个动作; ioctl/suspend/resume 可空 ⇒ -ENOTSUP) */
    if (ops->read == BR_NULL || ops->write == BR_NULL ||
        ops->flush == BR_NULL || ops->geometry == BR_NULL) {
        return BR_ERR(EINVAL);
    }
    if (br_dev_name_valid(name) == BR_FALSE) {
        return BR_ERR(EINVAL);   /* 含 name == BR_NULL / 空 / 大写 / 含斜杠 */
    }

    /* 几何是注册的前置条件(见文件头): 驱动自己的错误原样上传, 自洽性由本件判。 */
    br_bdev_geom_t geom;
    const int grc = ops->geometry(priv, &geom);
    if (grc != BR_OK) {
        return grc;
    }
    if (bdev_geom_ok(&geom) == BR_FALSE) {
        return BR_ERR(EINVAL);
    }

    if (s_bdev_n >= (br_u32)BR_BDEV_MAX) {
        return BR_ERR(ENOSPC);
    }

    bdev_slot_t *slot = &s_bdevs[s_bdev_n];
    slot->used = BR_TRUE;
    slot->ops  = ops;
    slot->priv = priv;

    const br_dev_class_entry_t e = {
        .class_id   = (br_u16)BR_CLASS_BDEV,
        .class_priv = (void *)ops,     /* 形状解释权在本件; dev-core 只透传 */
        .open_file  = BR_NULL,         /* ★ D21: v1 的 raw 块访问属 v2(看得见打不开) */
        .dev_priv   = slot,
    };

    const int rc = br_dev_add(name, &e);
    if (rc != BR_OK) {
        slot->used = BR_FALSE;         /* 回滚: 失败不留"半注册"的槽位 */
        return rc;                     /* -EINVAL / -EEXIST / -ENOSPC 原样上传 */
    }

    s_bdev_n++;
    br_log_info("bdev: registered /dev/%s (sector=%u, %lu sectors, max_rw=%u, %s%s)",
                name, geom.sector_size, (unsigned long)geom.n_sectors, geom.max_rw,
                ((geom.flags & BR_BDEV_F_DMA) != 0u) ? "DMA" :
                (((geom.flags & BR_BDEV_F_PIO) != 0u) ? "PIO" : "path?"),
                ((geom.flags & BR_BDEV_F_READONLY) != 0u) ? ", ro" : "");
    return BR_OK;
}

const br_bdev_ops_t *br_bdev_get(const char *name, void **priv)
{
    bdev_slot_t *slot = bdev_find(name);
    if (slot == BR_NULL) {
        return BR_NULL;                /* 未注册: 不改写 *priv(与 cdev-core 同契约) */
    }
    if (priv != BR_NULL) {
        *priv = slot->priv;
    }
    return slot->ops;
}

/* ==================================================================== 生命周期 */

/* EARLY: 无动作(静态池是 BSS; 此刻无堆无线程)。设备注册发生在各驱动的 CORE init。 */
int bdev_core_early_init(void)
{
    return 0;
}

/* CORE: 无动作。bdev-core 自己没有要初始化的状态 —— 它的"上场"是驱动调 `br_bdev_register`。
 * 刻意不造空 API(R-S4: 框架件往里的每一行都要有消费者)。 */
int bdev_core_init(void)
{
    return 0;
}

/* START 相: 本件的一致性用例**不在这里跑** —— ADR-0010 之后由 core 的
 * `br_plugin_manager_selftest()` 经描述符的 `.selftest` 钩子统一驱动(见 src/bdev_selftest.c)。
 * 这里只留一行注册表摘要作启动证据。 */
int bdev_core_start(void)
{
    br_u32 n = 0u;
    for (br_u32 i = 0u; i < br_dev_count(); i++) {
        const br_dev_class_entry_t *e = br_dev_entry_at(i);
        if (e != BR_NULL && e->class_id == (br_u16)BR_CLASS_BDEV) {
            n++;
        }
    }
    br_log_info("bdev: %u block device(s) in registry", n);
    return 0;
}

/* ==================================================================== 自检用的内部视图
 *
 * 见 `src/bdev_internal.h` 的边界纪律。这里**只**暴露自检真的用到的两样:
 * 容量与槽位占用数(用来断言"表不溢出"的结构性不变量)。
 * 一律**转调**生产实现的同一份状态, 不复制常量 —— 否则用例比的是副本。
 */

br_u32 bdev_internal_capacity(void)
{
    return (br_u32)BR_BDEV_MAX;
}

br_u32 bdev_internal_slot_count(void)
{
    return s_bdev_n;
}
