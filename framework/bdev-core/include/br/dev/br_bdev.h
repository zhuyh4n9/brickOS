/*
 * brickOS prototype v0.2.0 — 块设备子分类契约(framework/bdev-core 插件的对外面)
 *
 * 设计依据:
 *   - `7-storage/7-02-bdev.md` §1(`br_bdev_geom_t` / `br_bdev_ops` / `br_bdev_register` /
 *     `br_bdev_get` 逐字段对齐; 成功返回 = **传输扇区数**; 失败 = SD-10 负 errno)
 *   - `8-device/8-01-device.md` §1.3(bdev-core 依赖 dev-core: 块设备注册进 dev-core 命名空间)、
 *     §3(bdev 子分类 = 扇区 read/write/flush + 几何; D20 的形状分离)
 *   - `7-storage/7-02-bdev.md` §2(SD-9 分区映射器 = v1.x, 本件留设计不实现)、
 *     §3(page cache 是堆叠插件, 本件只提供"堆叠对上层无感"的同形状地基)
 * 逐条裁定与偏离: `docs/decisions/0021-bdev-pci-emmc.md`。
 *
 * ## 本件**不**做什么(R-S4 的防线)
 *   - 不认识任何具体介质(eMMC/NOR/virtio-blk): 那是对接本件的 `io/` 或三方插件;
 *   - 不做分区解析(MBR/GPT 与 `br_bdev_partition` 是 v1.x 的事, SD-9);
 *   - 不做缓存策略(page cache 是**堆叠插件**, SD-8, 本件只保证"同一 `br_bdev_ops`"的形状);
 *   - 不做设备生命周期策略(注册 = 上线, 无注销 —— v1 静态组合, 与 dev-core/cdev-core 同款)。
 *
 * ## raw 块访问(D21 / `7-02` §1 末段)
 *   v1 块设备经**类 API**(`br_bdev_get`)供 FS 绑定; `/dev/<name>` 的 raw 文件访问属 **v2**。
 *   因此 bdev 条目的 `open_file` 钩子**恒为 `BR_NULL`**(看得见、打不开, 与 flash 同款)。
 *   这条有机械判据(TC-BDEV-006: 遍历注册表断言 bdev 条目 open_file 恒 NULL)。
 */
#ifndef BR_DEV_BR_BDEV_H
#define BR_DEV_BR_BDEV_H

#include <br/core/br_types.h>

/* ==================================================================== 几何 */

/*
 * `flags`: 设计只写了"可堆叠/只读/..."—— 这里把位定下来。**本刀新增 DMA/PIO 两位**
 * (主控要求"数据传输需要指出 DMA/PIO"; 见 ADR-0021 D-1/D-4)。
 *
 *   READONLY 声明"介质不可写": 语义位, 不放松注册期的必填槽位(ops 形状仍要齐;
 *            写路径应当返回 -EROFS —— 访问模式执法属 v1 的 svc-posix, 见 ADR-0009 §5)。
 *   STACKED  provider 自身也是 consumer(分区映射器 / page cache 的标记, SD-8/SD-9)。
 *   DMA/PIO  数据通路的**实际**模式: `geometry` 每次回答当前值, 二者**至多置一**
 *            (都置 = 自相矛盾 ⇒ 注册期 -EINVAL; 都不置 = "未声明通路", 允许但观测面报不出)。
 */
#define BR_BDEV_F_READONLY  0x0001u
#define BR_BDEV_F_STACKED   0x0002u
#define BR_BDEV_F_DMA       0x0004u
#define BR_BDEV_F_PIO       0x0008u

typedef struct br_bdev_geom {
    br_u32 sector_size;   /* 逻辑扇区, 通常 512(设计原文) */
    br_u64 n_sectors;     /* 介质总扇区数 */
    br_u32 max_rw;        /* 单次传输上限(扇区; > 0 且 <= n_sectors) */
    br_u32 align;         /* 缓冲对齐要求, 0 = 任意(DMA 不敏感) */
    br_u32 flags;         /* BR_BDEV_F_* */
} br_bdev_geom_t;

/* ==================================================================== bdev 形状 */

/*
 * 块设备 ops(设计 `7-02` §1 原样; 类型换成本仓库的 `br_*` 拼写)。
 *
 * 调用语义:
 *   - `read/write(priv, lba, buf, n_lba)`: 成功返回**传输扇区数**(0..n_lba; 允许短读),
 *     失败返回 `BR_ERR(BR_E*)`。**lba 以扇区为单位**, 按 `geometry.sector_size` 折算字节。
 *   - `flush(priv)`: **必须落介质**(page cache 依赖, SD-8); 不允许吞掉。支持返回 0。
 *   - `geometry(priv, out)`: 每次调用回答当前几何(可随运行期模式切换而变)。
 *   - `ioctl/suspend/resume`: D22 预留槽位; `BR_NULL` ⇒ 调用方得 `-ENOTSUP`。
 *     槽位**存在**是因为 ops 布局入 golden(D14/D22): 后补字段 = 二进制不兼容。
 *
 * 并发(D22/`8-01` §4): 设备 ops 由**驱动自锁**; bdev-core 不加锁(它是注册表, 不是请求队列)。
 * 上下文: 设备/存储域 API **thread-only**(ISR 禁令, `8-01` §4); 中断驱动的驱动必须在
 * 下半部/线程里完成 ops, 而不是在 ISR 里。
 */
typedef struct br_bdev_ops {
    int (*read)    (void *priv, br_u64 lba, void *buf, br_size_t n_lba);
    int (*write)   (void *priv, br_u64 lba, const void *buf, br_size_t n_lba);
    int (*flush)   (void *priv);
    int (*geometry)(void *priv, br_bdev_geom_t *g);
    int (*ioctl)   (void *priv, br_u32 cmd, void *arg);
    int (*suspend) (void *priv);
    int (*resume)  (void *priv);
} br_bdev_ops_t;

/*
 * 登记一个块设备(设备名 = devfs 里的 `/dev/<name>`, 如 `emmc0`)。
 * 语义: 校验名字(`br_dev_name_valid`)→ 调一次 `geometry` 并校验几何自洽 →
 *       以 `class_id = BR_CLASS_BDEV` 入 dev-core 注册表(`open_file = BR_NULL`, D21)。
 * 错误: `-EINVAL`(name/ops/几何非法, 或 DMA|PIO 同置)/ `-EEXIST`(重名)/
 *       `-ENOSPC`(注册表满)。
 * `ops` 的必填槽位: `read` / `write` / `flush` / `geometry` 四者皆必填
 * (块介质没有"不落盘"这回事 —— 只读设备用 `BR_BDEV_F_READONLY` 声明, 仍须给写槽位)。
 */
int br_bdev_register(const char *name, const br_bdev_ops_t *ops, void *priv);

/* 形态 B(不链 vfs-core)的直取口(设计 `7-02` §1: "FS 插件绑定用")。
 * 未注册 ⇒ ops 返回 BR_NULL 且 `*priv` 不变。 */
const br_bdev_ops_t *br_bdev_get(const char *name, void **priv);

/* ==================================================================== 一致性用例 */

/* 本件的一致性用例(自编号 `TC-BDEV-*`; 登记在 `br-wa-test-001`)在
 * `src/bdev_selftest.c`(实现 `bdev_core_selftest()`), 由 core 的
 * `br_plugin_manager_selftest()` 经描述符的 `.selftest` 钩子统一驱动(ADR-0010)。
 * 为什么从对外头里删掉测试入口: 它不是插件的 golden 接口面(加一条用例不该是接口变更)。 */

#endif /* BR_DEV_BR_BDEV_H */
