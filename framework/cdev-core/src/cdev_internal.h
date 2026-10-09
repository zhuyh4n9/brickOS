/*
 * brickOS prototype v0.2.0 — cdev-core **插件私有**内部契约(不导出, 不在 include/)
 *
 * 给谁看: 本插件自己的 `src/cdev_selftest.c`。放在 `src/`(而不是 `include/`)是刻意的 ——
 * `include/` 下的一切都是**插件对外面**(会被 `brickie check` 的接口域治理, 也进
 * `descriptor_include` 的选用范围); 而这里的东西**只有自检**需要。
 *
 * ## 为什么自检要读内部
 *   有两条判据的**被测对象就是内部机制本身**, 用公开 API 表达不出来:
 *     - TC-CDEV-004/005 要按**指针同一性**判断"这条 dev 条目是不是本件登记的 cdev":
 *       公开面只给 `br_cdev_get`(按名取回 ops), 它回答不了"条目上的 open_file 钩子
 *       是不是本件填的那个" —— 而那正是"cdev 与 flash 子型同 class_id 却不同文件面"
 *       这条区分的判据;
 *     - TC-CDEV-006 要逐槽位查看**适配层那张 `br_file_ops` 表**(lseek/fsync/poll_attach
 *       必须为空 ⇒ 调用得 -ENOTSUP 的机制), 而 `s_cdev_file_ops` 是 `cdev_core.c` 的 static。
 *   为了这些把它们做成**公开 API** 会更糟: 适配层表的内部视图进了 golden 接口面,
 *   以后改槽位就是接口变更(而它明明是实现细节)。
 *
 * ## 边界纪律(与 core 的 `*_internal.h`、vfs-core 的 `src/vfs_internal.h` 同源)
 *   这里**只**暴露自检真的用到的两样, 逐条在下面写明用途; **不**暴露整个槽位池
 *   (`s_cdevs`/`s_flash` 与它们的条数都不在这里): 自检对注册表的观察一律走公开 API。
 */
#ifndef BR_CDEV_CORE_INTERNAL_H
#define BR_CDEV_CORE_INTERNAL_H

#include <br/core/br_types.h>
#include <br/vfs/br_vfs.h>      /* br_file_ops_t(适配层表指针的类型) */

/* open_file 钩子的形状(= `br_dev_class_entry_t.open_file` 的签名)。
 * 为什么单独给个 typedef 而不是让访问器返回 `void *`: 对象指针与函数指针的互转在
 * ISO C 里没有定义, 而这里要做的正是**函数指针的同一性比较** —— 类型必须逐字对上。 */
typedef int (*cdev_open_file_hook_t)(void *dev_priv, const br_file_ops_t **fops, void **fpriv);

/* 本件在 `br_cdev_register` 里填给 cdev 条目的 open_file 钩子(生产实现是 static
 * `cdev_open_file`)。
 * 用途: TC-CDEV-004/005 —— 按指针同一性认出"自家条目", 再对它跑取回/重名往返。
 * ★ 直接**转调**生产实现(返回它的地址), 不复制一个新函数: 否则用例比的是副本, 而
 *   "注册时填的钩子就是适配层入口"这件事根本没被验证(见该头文件的边界纪律)。 */
cdev_open_file_hook_t cdev_internal_open_file_hook(void);

/* 通用会话适配层的 `br_file_ops` 表(生产实现是 static `s_cdev_file_ops`)。
 * 用途: TC-CDEV-006 —— 逐槽位断言"转发槽位齐备, lseek/fsync/poll_attach 留空"。
 * ★ 同样**转调**: 返回 `&s_cdev_file_ops`, 用例看到的就是 `cdev_open_file` 交给 devfs
 *   的那张表本身(不是复制品)。 */
const br_file_ops_t *cdev_internal_file_ops(void);

#endif /* BR_CDEV_CORE_INTERNAL_H */
