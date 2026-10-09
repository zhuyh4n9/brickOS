/*
 * brickOS prototype v0.2.0 — 字符设备子分类契约(framework/cdev-core 插件的对外面)
 *
 * 设计依据:
 *   - `8-01-device.md` §3(`br_cdev_ops` 会话式形状 + `br_cdev_register`;
 *     **flash 子型** `br_flash_ops`/`br_flash_register`/`br_flash_get` —— spi-nor/nand 对接于此)
 *   - `8-01-device.md` §3 的**统一预留槽位(D22/SD-14)**: 所有设备类别 ops 预留
 *     `ioctl`/`suspend`/`resume`(NULL ⇒ -ENOTSUP); file 面的 `poll`/`close` 由本件的
 *     **通用 br_file_ops 适配层**提供(devfs 经 open_file 钩子取得该适配)
 *   - `7-03-concrete-fs.md` §3(cdev-core 提供通用会话适配: open 建会话 / read/write /
 *     ioctl / poll / close 转发; lseek/fsync 槽位 NULL ⇒ -ENOTSUP)
 *   - `8-01-device.md` §4(SD-6 阻塞语义 / ISR 禁令 / SD-10 负 errno)
 * 逐条裁定与偏离: `docs/decisions/0009-vfs-storage-stack.md`。
 *
 * ## 本件**不**做什么
 *   - 不认识任何具体器件(uart/can/adc…): 那是对接本件的 `io/` 插件;
 *   - 不做设备生命周期策略(注册 = 上线, 无注销 —— v1 静态组合);
 *   - 不把 cdev 的"会话"概念泄漏给 dev-core(D20: 通用与形状分离)。
 */
#ifndef BR_DEV_BR_CDEV_H
#define BR_DEV_BR_CDEV_H

#include <br/core/br_types.h>
#include <br/dev/br_dev.h>
#include <br/vfs/br_vfs.h>

/* ==================================================================== cdev 形状 */

/*
 * 字符设备 ops(设计 `8-01` §3 原样)。
 *
 * 会话模型: `open(dev_priv, flags, &sess)` 建会话并给出不透明 `sess`; 之后
 * read/write/ioctl/poll 都带 `sess`; `close(sess)` 销毁它。**独占性策略归驱动**
 * (uart 类在 open 里自己返回 -EBUSY; 设计 `7-01` §1 末段)。
 * `suspend`/`resume` 是**设备级**(拿 `dev_priv`, 不是会话级)的 PM 钩子(D22 预留;
 * v1 无统一调用方 —— O-S6)。NULL ⇒ -ENOTSUP。
 */
typedef struct br_cdev_ops {
    int     (*open) (void *dev_priv, br_u32 flags, void **sess);
    br_s64  (*read) (void *sess, void *buf, br_size_t n);
    br_s64  (*write)(void *sess, const void *buf, br_size_t n);
    int     (*ioctl)(void *sess, br_u32 cmd, void *arg);
    int     (*poll) (void *sess, br_u32 *events);
    int     (*close)(void *sess);
    int     (*suspend)(void *dev_priv);
    int     (*resume) (void *dev_priv);
} br_cdev_ops_t;

/*
 * 登记一个字符设备(设备名 = devfs 里的 `/dev/<name>`)。
 * 语义: 校验名字(`br_dev_name_valid`)→ 以 `class_id = BR_CLASS_CDEV` 入 dev-core 注册表,
 *       并**自动填上本件的 open_file 钩子**(cdev 的通用会话适配层)⇒ 驱动只需实现 ops。
 * 错误: `-EINVAL`(name/ops 非法)/ `-EEXIST`(重名)/ `-ENOSPC`(注册表满)。
 * `ops` 的必填槽位: open/close(缺 ⇒ -EINVAL); read/write/ioctl/poll 可空(⇒ 调用得 -ENOTSUP)。
 */
int br_cdev_register(const char *name, const br_cdev_ops_t *ops, void *dev_priv);

/* 形态 B(不链 vfs-core)的直取口(设计 `8-01` §1.2): 按名取 ops 与驱动私有。
 * 未注册 ⇒ ops 返回 BR_NULL 且 `*dev_priv` 不变。 */
const br_cdev_ops_t *br_cdev_get(const char *name, void **dev_priv);

/* ==================================================================== flash 子型 */
/*
 * flash 是**字符型介质**(按地址 program/erase, 无磁盘式扇区抽象, 设计 SD-2)⇒ 归 cdev
 * 子分类的**子型**; 其形状与 littlefs 的 `lfs_config` 1:1(零胶水绑定, `7-03` §4)。
 */
typedef struct br_flash_geom {
    br_u32 read_size;     /* 最小读粒度 */
    br_u32 prog_size;     /* 编程页大小 */
    br_u32 block_size;    /* 擦除块 */
    br_u32 block_count;
} br_flash_geom_t;

typedef struct br_flash_ops {
    int (*read)   (void *priv, br_u32 addr, void *buf, br_size_t n);
    int (*program)(void *priv, br_u32 addr, const void *buf, br_size_t n);
    int (*erase)  (void *priv, br_u32 block_idx);
    int (*sync)   (void *priv);
    /* D22 预留: ioctl(NOR 深度下电/特性控制)+ suspend/resume(电源管理) */
    int (*ioctl)  (void *priv, br_u32 cmd, void *arg);
    int (*suspend)(void *priv);
    int (*resume) (void *priv);
} br_flash_ops_t;

/*
 * 登记一个 flash 设备。
 * ★ **v1 不填 open_file 钩子**(BR_NULL): raw flash 的文件面不是 v1 的目标
 *   —— 设计 `7-03` §3 明说"bdev 的 raw 块访问(/dev/blk0)钩子 = v2(v1 置 NULL,
 *   FS 经 `br_bdev_get` 类 API 绑定)"。于是 `/dev/nor0` 会**出现但打不开**(-ENOTSUP):
 *   看得见、打不开比"看不见"诚实(ADR-0009 §3 裁定 6)。
 */
int br_flash_register(const char *name, const br_flash_ops_t *ops,
                      const br_flash_geom_t *geom, void *priv);

/* 按名取 flash ops 与几何(形态 B / littlefs 的绑定口)。未注册 ⇒ BR_NULL。 */
const br_flash_ops_t *br_flash_get(const char *name, void **priv);
int br_flash_geom_get(const char *name, br_flash_geom_t *out);

/* ==================================================================== 一致性用例 */

/* 本件的一致性用例(自编号 `TC-CDEV-*`; 登记在 `br-wa-test-001`)已移到
 * `src/cdev_selftest.c`(实现 `cdev_core_selftest()`), 由 core 的
 * `br_plugin_manager_selftest()` 经描述符的 `.selftest` 钩子统一驱动(ADR-0010)。
 * 为什么从对外头里删掉: 测试入口**不是**插件的 golden 接口面 —— 它是"本件自证"的机制,
 * 消费者不该依赖它, 它也不该参与接口 hash/版本治理(加一条用例不该是接口变更)。 */

#endif /* BR_DEV_BR_CDEV_H */
