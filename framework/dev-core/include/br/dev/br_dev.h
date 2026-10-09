/*
 * brickOS prototype v0.2.0 — 通用设备契约(framework/dev-core 插件的对外面)
 *
 * 设计依据:
 *   - `8-01-device.md` §1.1/§1.3(体系分层与框架件归属: **dev-core 只管"是个设备"** ——
 *     唯一扁平命名空间/命名规则/调用语义与错误模型/ioctl 编码 / **子分类注册协议**;
 *     形状归子分类框架)
 *   - `8-01-device.md` §2(注册表条目 `br_dev_class_entry_t` + **open_file 钩子**(D21/D23):
 *     "设备 → 文件"的适配入口, 返回 {fops, fpriv}; 钩子签名引用 `br_file_ops` ⇒
 *     dev-core 对 vfs-core 是**类型依赖**(仅头文件, 无 init/call 依赖, O-S7))
 *   - `8-01-device.md` §3(dev-core **不定义任何具体 ops 形状** —— 新增子分类不动本件)
 *   - `8-01-device.md` §4(错误模型 = 负 errno)/§5(ioctl 用 Linux 兼容 32 位编码)
 * 逐条裁定与偏离: `docs/decisions/0009-vfs-storage-stack.md`。
 *
 * ## 本件**不**做什么(R-S4 的防线)
 *   - 不定义 cdev/bdev/flash 的 ops 形状(那是 cdev-core/bdev-core 的事);
 *   - 不拥有设备生命周期策略(设备在驱动 `*_register` 时出现、无显式注销 —— v1 静态组合);
 *   - 不做任何"策略": 命名唯一性/上限/错误码是**机制**, 就这些。
 */
#ifndef BR_DEV_BR_DEV_H
#define BR_DEV_BR_DEV_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>      /* br_file_ops_t(open_file 钩子的返回类型; 类型依赖, 见 O-S7) */

/* ==================================================================== 命名与上限 */

/* 设备名规则(设计 `8-01` §2): `[a-z][a-z0-9]*`, 无斜杠 —— devfs 节点名 = 路径分量。
 * 静态上界复用 vfs 的 `BR_NAME_MAX`(同一命名空间, 不该有两个长度真值)。 */
#define BR_DEV_NAME_MAX   BR_NAME_MAX
#define BR_DEV_MAX        16u   /* 注册表静态上界(设计 3-02 §14.6 的静态上界手法) */

/* ==================================================================== 子分类 id */
/* `class_id`: 形状解释权在**子分类框架**(append-only, D14)。dev-core 只透传。 */
#define BR_CLASS_NONE     0u
#define BR_CLASS_CDEV     1u   /* framework/cdev-core(br_cdev_ops / br_flash_ops) */
#define BR_CLASS_BDEV     2u   /* framework/bdev-core(未落地; id 先占免得漂移) */

/* ==================================================================== 注册表条目 */

/*
 * 一条设备(设计 `8-01` §2 原样 + 一个 `name` 不进结构体的理由):
 * 名字是注册表的**键**, 由 `br_dev_add(name, entry)` 单独给出(设计如此)。
 *
 * `open_file`(D21/D23 的钩子): devfs 在设备节点 lookup 时调用, 返回文件面 ops 与该
 * 文件面的私有数据。**返回值为 0 时** devfs 把 {*fops, *fpriv} 塞进节点 inode。
 *   - cdev 子分类由 cdev-core 填(通用会话适配层);
 *   - `BR_NULL` = 本子分类**不提供**文件面(raw flash 与 bdev 的裸块访问属 v2)⇒
 *     devfs 仍列出节点, 但 `br_open` 它会得 `-ENOTSUP`(诚实: 看得见, 打不开)。
 */
typedef struct br_dev_class_entry {
    br_u16 class_id;      /* BR_CLASS_* */
    void  *class_priv;    /* 子分类框架的 ops 表指针(如 `const br_cdev_ops_t *`) */
    int  (*open_file)(void *dev_priv, const br_file_ops_t **fops, void **fpriv);
    void  *dev_priv;      /* 驱动私有 */
} br_dev_class_entry_t;

/* ==================================================================== 注册与查找 */

/*
 * 登记一个设备(子分类框架的 `*_register` 内部调用; 驱动不直接调它)。
 * 错误: `-EINVAL`(name/entry 非法, 或名字不合规)/ `-EEXIST`(重名 —— 唯一性在此执法)/
 *       `-ENOSPC`(表满)。
 * 为什么重名是**运行期**错误而不是只靠组合期: `[[res]]` 域能查资源冲突, 但设备名是
 * 运行期字符串(驱动自己拼的), 组合期看不见 —— 与 `br_service_publish` 的 `-EEXIST` 同型。
 */
int br_dev_add(const char *name, const br_dev_class_entry_t *entry);

/* 按名查找(未注册 ⇒ BR_NULL)。返回的是**表内副本**, 生命周期同镜像。 */
const br_dev_class_entry_t *br_dev_lookup(const char *name);

/* 枚举(devfs 的 readdir 用它; 顺序 = 注册顺序)。
 * `br_dev_count()` 条数; `br_dev_name_at(i)` 名字(越界 ⇒ BR_NULL);
 * `br_dev_entry_at(i)` 条目(越界 ⇒ BR_NULL)。 */
br_u32 br_dev_count(void);
const char *br_dev_name_at(br_u32 index);
const br_dev_class_entry_t *br_dev_entry_at(br_u32 index);

/* 名字合规性(`[a-z][a-z0-9]*`, 长度 1..BR_DEV_NAME_MAX-1)。返回 BR_TRUE/BR_FALSE。
 * 对外导出是为了让**子分类框架**在调用 br_dev_add 之前就能给驱动一个明确的错误,
 * 而不是各自再写一份正则。 */
br_bool br_dev_name_valid(const char *name);

/* ==================================================================== ioctl 编码 */
/*
 * 设计 `8-01` §5: 与 Linux `_IOC` 位布局一致(零学习成本 + 可静态查表):
 *   dir[31:30] size[29:16] type[15:8] nr[7:0]
 * `type` = 设备/驱动族魔数字母(每族一个, 全局唯一分配): 'u' uart / 'c' can /
 * 'b' bdev / 'f' flash / 'd' display …
 */
#define BR_IOC_NRBITS      8u
#define BR_IOC_TYPEBITS   8u
#define BR_IOC_SIZEBITS   14u
#define BR_IOC_DIRBITS    2u

#define BR_IOC_NRSHIFT    0u
#define BR_IOC_TYPESHIFT  (BR_IOC_NRSHIFT + BR_IOC_NRBITS)      /* 8 */
#define BR_IOC_SIZESHIFT  (BR_IOC_TYPESHIFT + BR_IOC_TYPEBITS)  /* 16 */
#define BR_IOC_DIRSHIFT   (BR_IOC_SIZESHIFT + BR_IOC_SIZEBITS)  /* 30 */

#define BR_IOC_NONE       0u
#define BR_IOC_WRITE      1u
#define BR_IOC_READ       2u

#define BR_IOC(dir, type, nr, size) \
    (((br_u32)(dir)  << BR_IOC_DIRSHIFT)  | \
     ((br_u32)(type) << BR_IOC_TYPESHIFT) | \
     ((br_u32)(nr)   << BR_IOC_NRSHIFT)   | \
     ((br_u32)(size) << BR_IOC_SIZESHIFT))

#define BR_IOR(type, nr, size)   BR_IOC(BR_IOC_READ,  (type), (nr), (br_u32)sizeof(size))
#define BR_IOW(type, nr, size)   BR_IOC(BR_IOC_WRITE, (type), (nr), (br_u32)sizeof(size))
#define BR_IOWR(type, nr, size)  BR_IOC(BR_IOC_READ | BR_IOC_WRITE, (type), (nr), (br_u32)sizeof(size))
#define BR_IO(type, nr)          BR_IOC(BR_IOC_NONE,  (type), (nr), 0u)

#define BR_IOC_DIR(cmd)   (((cmd) >> BR_IOC_DIRSHIFT)  & ((1u << BR_IOC_DIRBITS)  - 1u))
#define BR_IOC_TYPE(cmd)  (((cmd) >> BR_IOC_TYPESHIFT) & ((1u << BR_IOC_TYPEBITS) - 1u))
#define BR_IOC_NR(cmd)    (((cmd) >> BR_IOC_NRSHIFT)   & ((1u << BR_IOC_NRBITS)   - 1u))
#define BR_IOC_SIZE(cmd)  (((cmd) >> BR_IOC_SIZESHIFT) & ((1u << BR_IOC_SIZEBITS) - 1u))

/* ==================================================================== 一致性用例 */

/* 本件的一致性用例(自编号 `TC-DEV-*`; 登记在 `br-wa-test-001`)已移到
 * `src/dev_selftest.c`(实现 `dev_core_selftest()`), 由 core 的
 * `br_plugin_manager_selftest()` 经描述符的 `.selftest` 钩子统一驱动(ADR-0010)。
 * 为什么从对外头里删掉: 测试入口**不是**插件的 golden 接口面 —— 它是"本件自证"的机制,
 * 消费者不该依赖它, 它也不该参与接口 hash/版本治理(加一条用例不该是接口变更)。 */

#endif /* BR_DEV_BR_DEV_H */
