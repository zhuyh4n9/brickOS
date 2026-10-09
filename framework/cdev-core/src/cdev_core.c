/*
 * brickOS prototype v0.2.0 — 字符设备子分类 + 通用会话适配(framework/cdev-core)
 *
 * 设计依据: `docs/8-device/8-01-device.md`
 *   §3(`br_cdev_ops` 会话式形状 + `br_cdev_register`; **flash 子型** `br_flash_ops` ——
 *      spi-nor/nand 对接于此; D22/SD-14 的统一预留槽位)
 *   §2(open_file 钩子: 返回 {fops, fpriv} —— D21/D23 的"设备 → 文件"适配入口)
 *   `docs/7-storage/7-03-concrete-fs.md` §3("cdev-core 提供通用会话适配 `br_file_ops`
 *      (open 建会话/read/write/ioctl/poll/close 转发; lseek/fsync 槽位 NULL → -ENOTSUP)")
 * 逐条裁定见 `docs/decisions/0009-vfs-storage-stack.md`。
 *
 * ## 本文件的三件职责(别的都不做)
 *   ① `br_cdev_register`: 校验 → 在 dev-core 登记一条 `BR_CLASS_CDEV` 条目,
 *      **并自动填上本件的 open_file 钩子**(驱动只实现 ops, 不需要认识 VFS);
 *   ② **通用 br_file_ops 适配层**: 把"会话式 cdev"翻译成"文件面"(`7-03` §3 原文);
 *   ③ flash 子型的注册/取回(形状对齐 littlefs 的 lfs_config, 零胶水)。
 *
 * ## 为什么适配层在 cdev-core 而不是 devfs
 *   设计 D21/SD-13: **devfs 不依赖任何子分类的 ops 形状**(它只经 open_file 钩子拿
 *   {fops, fpriv}); vfs-core 更是零设备知识。若把适配放进 devfs, devfs 就得认识
 *   `br_cdev_ops` —— 那正是 D21 撤销的那条依赖。所以"设备语义 → 文件语义"的翻译只能
 *   在**子分类框架**里发生: 它既认识设备形状, 也认识 `br_file_ops`(类型依赖, O-S7)。
 *
 * ## 每文件私有状态放哪(ADR-0009 §3 裁定 2)
 *   vfs-core 把 inode 携带的 `fpriv`(这里 = 本件的**设备槽位指针**)交给 `br_file_t`;
 *   适配层的 `open` 把它换成**自己分配**的 `cdev_file_t`(含会话指针), `close` 释放。
 *   于是"每文件一份会话"有落点, 不需要改 `br_file_t` 的布局, 也不需要 FS 报"私有尾大小"。
 */
#include <br/core/br_error.h>
#include <br/core/br_log.h>
#include <br/core/br_mem.h>
#include <br/core/br_types.h>

#include <br/dev/br_cdev.h>
#include <br/dev/br_dev.h>

#include "cdev_internal.h"     /* 自检用的内部视图(见该头; 生产路径不用它) */

/* 生命周期钩子原型(名 = symbol_prefix + 相; 生成物 `plugin_desc.c` 引用; 不进 [[export]]) */
int cdev_core_early_init(void);
int cdev_core_init(void);
int cdev_core_start(void);

/* ==================================================================== 静态池 */

#define BR_CDEV_MAX    8u   /* 字符设备上界(原型: 静态上界, 与 BR_DEV_MAX 同样的"够用即停") */
#define BR_FLASH_MAX   4u   /* flash 子型上界 */

/*
 * 一条 cdev 的注册记录。交给 dev-core 的 `dev_priv` 是**槽位指针**(不是驱动私有本身)——
 * 因为适配层要同时找到 ops 与驱动私有, 而 open_file 钩子只拿到一个 `void *`。
 */
typedef struct cdev_slot {
    br_bool              used;
    const br_cdev_ops_t *ops;
    void                *dev_priv;
} cdev_slot_t;

typedef struct flash_slot {
    br_bool               used;
    char                  name[BR_DEV_NAME_MAX];   /* 供 br_flash_get 反查(注册表只给条目) */
    const br_flash_ops_t *ops;
    br_flash_geom_t       geom;
    void                 *priv;
} flash_slot_t;

static cdev_slot_t  s_cdevs[BR_CDEV_MAX];
static br_u32       s_cdev_n;
static flash_slot_t s_flash[BR_FLASH_MAX];
static br_u32       s_flash_n;

/* ==================================================================== 字符串小工具
 * 无 libc(-ffreestanding): 自持比较/拷贝三行。不跨件借 core/string.c 的符号 —— 插件
 * 的近零导出面(CA-10)与"谁依赖谁"的声明面都不该被一个字符串比较弄乱。 */

static br_bool cdev_str_eq(const char *a, const char *b)
{
    if (a == BR_NULL || b == BR_NULL) {
        return BR_FALSE;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (*a == *b) ? BR_TRUE : BR_FALSE;
}

static void cdev_str_copy(char *dst, const char *src, br_size_t cap)
{
    br_size_t i = 0u;
    if (cap == 0u) {
        return;
    }
    for (; src[i] != '\0' && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* ==================================================================== 适配层
 *
 * `br_file_ops` 的槽位语义(见 br_vfs.h): 不支持的槽位 = BR_NULL ⇒ 调用方得 -ENOTSUP。
 * 本适配层**故意**留空三个槽位:
 *   - lseek: 字符设备无偏移语义(设计 `7-03` §3 明说 "lseek/fsync 槽位 NULL → -ENOTSUP");
 *   - fsync: 字符设备没有"落介质"这回事;
 *   - poll_attach: v2 的 wait-queue 槽位(D22/D14 预留即免布局破坏)。
 */
typedef struct cdev_file {
    const cdev_slot_t *slot;
    void              *sess;   /* 驱动会话(ops->open 产出) */
} cdev_file_t;

static int cf_open(br_file_t *f, br_u32 flags)
{
    /* inode 携带的 fpriv = 本件的设备槽位(见 cdev_open_file) */
    const cdev_slot_t *slot = (const cdev_slot_t *)br_file_fpriv(f);
    if (slot == BR_NULL || slot->used == BR_FALSE || slot->ops == BR_NULL) {
        return BR_ERR(BR_ENODEV);
    }

    cdev_file_t *cf = (cdev_file_t *)br_malloc((br_size_t)sizeof(cdev_file_t));
    if (cf == BR_NULL) {
        return BR_ERR(BR_ENOMEM);
    }
    cf->slot = slot;
    cf->sess = BR_NULL;

    const int rc = slot->ops->open(slot->dev_priv, flags, &cf->sess);
    if (rc != BR_OK) {
        br_free(cf);
        return rc;      /* 原样上传: 独占设备在这里回 -EBUSY(策略归驱动, 设计 `7-01` §1) */
    }

    br_file_set_fpriv(f, cf);   /* ★ 覆盖: 从"设备槽位"变成"每文件会话" */
    return BR_OK;
}

static int cf_close(br_file_t *f)
{
    cdev_file_t *cf = (cdev_file_t *)br_file_fpriv(f);
    if (cf == BR_NULL) {
        return BR_OK;   /* 幂等: 会话没建起来就没什么可关 */
    }
    int rc = BR_OK;
    if (cf->slot->ops->close != BR_NULL) {
        rc = cf->slot->ops->close(cf->sess);
    }
    br_free(cf);
    br_file_set_fpriv(f, BR_NULL);
    return rc;
}

/* 会话视图(三个转发槽位共用): 拿不到会话 ⇒ BR_NULL */
static const cdev_file_t *cf_of(br_file_t *f)
{
    const cdev_file_t *cf = (const cdev_file_t *)br_file_fpriv(f);
    if (cf == BR_NULL || cf->slot == BR_NULL || cf->slot->ops == BR_NULL) {
        return BR_NULL;
    }
    return cf;
}

static br_s64 cf_read(br_file_t *f, void *buf, br_size_t n)
{
    const cdev_file_t *cf = cf_of(f);
    if (cf == BR_NULL || cf->slot->ops->read == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return cf->slot->ops->read(cf->sess, buf, n);
}

static br_s64 cf_write(br_file_t *f, const void *buf, br_size_t n)
{
    const cdev_file_t *cf = cf_of(f);
    if (cf == BR_NULL || cf->slot->ops->write == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return cf->slot->ops->write(cf->sess, buf, n);
}

static int cf_ioctl(br_file_t *f, br_u32 cmd, void *arg)
{
    const cdev_file_t *cf = cf_of(f);
    if (cf == BR_NULL || cf->slot->ops->ioctl == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return cf->slot->ops->ioctl(cf->sess, cmd, arg);
}

static int cf_poll(br_file_t *f, br_u32 *events)
{
    const cdev_file_t *cf = cf_of(f);
    if (cf == BR_NULL || cf->slot->ops->poll == BR_NULL) {
        return BR_ERR(BR_ENOTSUP);
    }
    return cf->slot->ops->poll(cf->sess, events);
}

/* 槽位用**具名初始化器**一一对应(位置初始化会让"漏了一个/放错位置"在汇编层面看不出来)。 */
static const br_file_ops_t s_cdev_file_ops = {
    .open        = cf_open,
    .read        = cf_read,
    .write       = cf_write,
    .lseek       = BR_NULL,
    .ioctl       = cf_ioctl,
    .fsync       = BR_NULL,
    .poll        = cf_poll,
    .poll_attach = BR_NULL,
    .close       = cf_close,
};

/* open_file 钩子(dev-core 在 devfs 设备节点 lookup 时调用): 返回 {fops, fpriv}。
 * `fpriv` = 设备槽位; 适配层的 `open` 会把它换成每文件会话。 */
static int cdev_open_file(void *dev_priv, const br_file_ops_t **fops, void **fpriv)
{
    if (dev_priv == BR_NULL || fops == BR_NULL || fpriv == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    *fops  = &s_cdev_file_ops;
    *fpriv = dev_priv;
    return BR_OK;
}

/* ==================================================================== cdev 注册与取回 */

int br_cdev_register(const char *name, const br_cdev_ops_t *ops, void *dev_priv)
{
    if (ops == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    /* open/close 是**必填**(设计 `8-01` §3: 会话模型的两个端点); 其余槽位可空。 */
    if (ops->open == BR_NULL || ops->close == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    if (s_cdev_n >= (br_u32)BR_CDEV_MAX) {
        return BR_ERR(BR_ENOSPC);
    }

    cdev_slot_t *slot = &s_cdevs[s_cdev_n];
    slot->used     = BR_TRUE;
    slot->ops      = ops;
    slot->dev_priv = dev_priv;

    const br_dev_class_entry_t e = {
        .class_id   = (br_u16)BR_CLASS_CDEV,
        .class_priv = (void *)ops,      /* 形状解释权在本件; dev-core 只透传 */
        .open_file  = cdev_open_file,
        .dev_priv   = slot,             /* ★ 交给钩子的是槽位, 不是驱动私有 */
    };

    const int rc = br_dev_add(name, &e);
    if (rc != BR_OK) {
        slot->used = BR_FALSE;          /* 回滚: 失败不留"半注册"的槽位 */
        return rc;                      /* -EINVAL / -EEXIST / -ENOSPC 原样上传 */
    }

    s_cdev_n++;
    return BR_OK;
}

const br_cdev_ops_t *br_cdev_get(const char *name, void **dev_priv)
{
    const br_dev_class_entry_t *e = br_dev_lookup(name);
    if (e == BR_NULL || e->class_id != (br_u16)BR_CLASS_CDEV) {
        return BR_NULL;
    }
    /* 同一个 class_id 下还有 flash 子型: 用"槽位指针是否落在本件的 cdev 池里"区分。
     * 不用 class_priv 比较(那没法排除"两个驱动的 ops 恰好同址"的理论可能)。 */
    const cdev_slot_t *slot = (const cdev_slot_t *)e->dev_priv;
    br_bool in_pool = BR_FALSE;
    for (br_u32 i = 0u; i < s_cdev_n; i++) {
        if (&s_cdevs[i] == slot && s_cdevs[i].used == BR_TRUE) {
            in_pool = BR_TRUE;
            break;
        }
    }
    if (in_pool == BR_FALSE) {
        return BR_NULL;
    }
    if (dev_priv != BR_NULL) {
        *dev_priv = slot->dev_priv;
    }
    return slot->ops;
}

/* ==================================================================== flash 子型 */
/*
 * flash 属 cdev 子分类的**子型**(设计 SD-2), 所以 `class_id` 同样是 BR_CLASS_CDEV;
 * 形状解释权仍在本件(`br_flash_ops` 与 `br_cdev_ops` 是两个形状)。
 * ★ 本件**不填** open_file 钩子(BR_NULL): raw flash 的文件面不是 v1 的目标
 *   (设计 `7-03` §3: "bdev 的 raw 块访问钩子 = v2")⇒ `/dev/nor0` 会出现但 `br_open`
 *   得 -ENOTSUP。看得见打不开, 比"看不见"诚实(ADR-0009 §3 裁定 6)。
 */
static flash_slot_t *flash_find(const char *name)
{
    for (br_u32 i = 0u; i < s_flash_n; i++) {
        if (s_flash[i].used == BR_TRUE && cdev_str_eq(s_flash[i].name, name) == BR_TRUE) {
            return &s_flash[i];
        }
    }
    return BR_NULL;
}

int br_flash_register(const char *name, const br_flash_ops_t *ops,
                      const br_flash_geom_t *geom, void *priv)
{
    if (ops == BR_NULL || geom == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    /* 几何必须自洽: 0 尺寸的读/编程粒度或 0 块数 ⇒ 任何 FS 都算不出地址。
     * 在注册期拒绝, 免得 littlefs 侧拿到一张"看起来能算、其实除以 0"的表。 */
    if (geom->read_size == 0u || geom->prog_size == 0u ||
        geom->block_size == 0u || geom->block_count == 0u) {
        return BR_ERR(BR_EINVAL);
    }
    if (ops->read == BR_NULL || ops->program == BR_NULL || ops->erase == BR_NULL) {
        return BR_ERR(BR_EINVAL);   /* flash 子型的三个必填动作(设计 `8-01` §3) */
    }
    if (flash_find(name) != BR_NULL) {
        return BR_ERR(BR_EEXIST);
    }
    if (s_flash_n >= (br_u32)BR_FLASH_MAX) {
        return BR_ERR(BR_ENOSPC);
    }

    flash_slot_t *slot = &s_flash[s_flash_n];
    slot->used = BR_TRUE;
    slot->ops  = ops;
    slot->geom = *geom;
    slot->priv = priv;
    cdev_str_copy(slot->name, name, (br_size_t)BR_DEV_NAME_MAX);

    const br_dev_class_entry_t e = {
        .class_id   = (br_u16)BR_CLASS_CDEV,
        .class_priv = (void *)ops,
        .open_file  = BR_NULL,      /* v1: raw flash 无文件面(v2 的 bdev/raw 钩子) */
        .dev_priv   = slot,
    };
    const int rc = br_dev_add(name, &e);
    if (rc != BR_OK) {
        slot->used = BR_FALSE;
        return rc;
    }

    s_flash_n++;
    return BR_OK;
}

const br_flash_ops_t *br_flash_get(const char *name, void **priv)
{
    flash_slot_t *slot = flash_find(name);
    if (slot == BR_NULL) {
        return BR_NULL;
    }
    if (priv != BR_NULL) {
        *priv = slot->priv;
    }
    return slot->ops;
}

int br_flash_geom_get(const char *name, br_flash_geom_t *out)
{
    if (out == BR_NULL) {
        return BR_ERR(BR_EINVAL);
    }
    flash_slot_t *slot = flash_find(name);
    if (slot == BR_NULL) {
        return BR_ERR(BR_ENODEV);
    }
    *out = slot->geom;
    return BR_OK;
}

/* ==================================================================== 生命周期 */

/* EARLY: 无动作(静态池是 BSS; 此刻无堆无线程)。设备注册发生在各驱动的 CORE init。 */
int cdev_core_early_init(void)
{
    return 0;
}

/* CORE: 无动作。cdev-core 自己没有要初始化的状态 —— 它的"上场"是驱动调 `br_cdev_register`。
 * 刻意不造空 API(R-S4: 框架件往里的每一行都要有消费者)。 */
int cdev_core_init(void)
{
    return 0;
}

/* START 相: 本件的一致性用例**不在这里跑** —— ADR-0010 之后由 core 的
 * `br_plugin_manager_selftest()` 经描述符的 `.selftest` 钩子统一驱动(见 src/cdev_selftest.c)。
 * 这里只留一行注册表摘要作启动证据。 */
int cdev_core_start(void)
{
    br_log_info("cdev: %u device(s) in registry", br_dev_count());
    return 0;
}

/* ==================================================================== 自检用的内部视图
 *
 * 见 `src/cdev_internal.h` 的说明: 这两个访问器**只**为 `cdev_selftest.c` 存在 ——
 * 被测对象就是这里的机制本身(本件填的 open_file 钩子 / 适配层那张 `br_file_ops` 表),
 * 用公开 API 表达不出来。
 * ★ 一律**转调**生产实现(返回它们的地址), 不复制逻辑: 用例比的必须是同一份真相,
 *   否则"注册时填的钩子就是适配层入口"这件事验证的是副本。
 */

cdev_open_file_hook_t cdev_internal_open_file_hook(void)
{
    return cdev_open_file;
}

const br_file_ops_t *cdev_internal_file_ops(void)
{
    return &s_cdev_file_ops;
}
