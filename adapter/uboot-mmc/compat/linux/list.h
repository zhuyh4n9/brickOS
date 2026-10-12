/* adapter/uboot-mmc — `<linux/list.h>` 的适配壳(→ **vendor 原件**)。
 *
 * ★ 为什么转发而不是自己给一个 `struct list_head`: `mmc_legacy.c`(上游原件)真的**遍历**
 *   那张设备链 —— `mmc_list_init()`/`mmc_list_add()`/`find_mmc_device()` 用
 *   `INIT_LIST_HEAD`/`list_add_tail`/`list_for_each`/`list_entry`。这不是"只为了布局成立"
 *   的字段(erofs 那边是), 所以必须给**真的实现**; 而上游那份就是权威实现。
 *   (与 `adapter/uboot-erofs/compat/erofs_env.h` 里"只给类型不给遍历"的处置相反 ——
 *    区别在于**这里真的有人遍历**。)
 *
 * ★ 它的 include 闭合集: `<linux/stddef.h>` 与 `<linux/poison.h>`。前者由 `compat/`
 *   给(`NULL`/`offsetof`/`size_t`), 后者转发到 vendor 原件。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_LIST_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_LINUX_LIST_H
#include "../../../../third_party/uboot-imx/include/linux/list.h"
#endif
