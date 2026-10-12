/* adapter/uboot-mmc — `<cyclic.h>` 的适配壳(u-boot 头名的**忠实最小**形态)。
 *
 * ★ 为什么**不** vendor 上游 `include/cyclic.h`: 它 `#include <u-boot/schedule.h>`,
 *   而后者 `#include <uthread.h>` —— 整族线程调度设施。本件不启用 `CONFIG_CYCLIC`
 *   (矩阵里有意关闭), 上游那份头在关闭时**本来**就退化成"空结构 + 三个 static inline
 *   空函数"; 本文件就是那个形态的逐条重述(不是发明)。
 *
 * 语义(与上游 `#else` 分支一字对应):
 *   - `struct cyclic_info` 是**空结构** —— 但 `struct mmc` 里**有它一个字段**
 *     (`cyclic`), 所以类型必须存在(大小 0 的成员在 GCC 里让结构体偏移不变);
 *   - `cyclic_register()`/`cyclic_unregister()` 是 no-op: 上游 `mmc.c` 的调用点在
 *     `if (CONFIG_IS_ENABLED(CYCLIC, (!mmc->cyclic.func), (NULL)))` 里, 该条件恒为
 *     假(展开成 `if (NULL)`), 于是"卡检测轮询"这条上游功能在本件**不参与**。
 *     它们必须是 `static inline`, 否则"恒假的调用点"仍会在链接期要符号。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_CYCLIC_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_CYCLIC_H

#include "mmc_env.h"

/* 与上游 `include/cyclic.h` 的 `!CONFIG_CYCLIC` 形态一致 */
struct cyclic_info {
};

/** Function type for cyclic functions */
typedef void (*cyclic_func_t)(struct cyclic_info *c);

static inline void cyclic_register(struct cyclic_info *cyclic, cyclic_func_t func,
                                   uint64_t delay_us, const char *name)
{
    (void)cyclic;
    (void)func;
    (void)delay_us;
    (void)name;
}

static inline void cyclic_unregister(struct cyclic_info *cyclic)
{
    (void)cyclic;
}

static inline int cyclic_unregister_all(void)
{
    return 0;
}

#endif /* BR_ADAPTER_UBOOT_MMC_SHIM_CYCLIC_H */
