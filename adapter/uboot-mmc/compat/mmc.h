/* adapter/uboot-mmc — `<mmc.h>` 的适配壳(→ **vendor 原件**)。
 *
 * ★ 为什么转发到 vendor 而不是自己写一份"接口类型壳": `struct mmc` 是**上游协议核心
 *   自己的状态**(约 30 个字段, 受 6 个配置开关裁剪), 上游 `mmc.c` 与 `sdhci.c` 都直接
 *   读写它 —— 它不是"我们和上游之间的接口", 而是上游的**内部数据类型**。抄一份就等于
 *   抄上游(且必须与配置矩阵逐条同步)。⇒ 逐字节用 vendor 那份。
 *
 * ★ 它自己要 include `<linux/bitops.h> <linux/list.h> <linux/dma-direction.h>
 *   <cyclic.h> <part.h>` —— 前/后各由 `compat/` 的同名壳兑现, `linux/list.h` 转发 vendor。
 *
 * ★ 依赖方向: 本文件属 `compat/`(给上游环境), 却 include 上游头 —— 这是允许的
 *   (`compat/` 的职责就是"把上游原件接到我们的环境上")。判据是**不出现尖括号/引号直写
 *   third_party 路径**的那种直连形式(门禁 `check-third-party-pristine.sh` 判据⑤);
 *   本插件一律走相对路径转发壳 —— 与 `adapter/uboot-erofs/compat/u-boot/lz4.h` 同款。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_MMC_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_MMC_H
#include "../../../third_party/uboot-imx/include/mmc.h"
#endif
