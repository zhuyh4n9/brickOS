/* adapter/uboot-mmc — `<dm/uclass-id.h>` 的适配壳(→ **vendor 原件**)。
 *
 * ★ `struct blk_desc` 的第一个字段就是 `enum uclass_id uclass_id;`, 而 `mmc_legacy.c`
 *   会写 `UCLASS_MMC`。这是一个**纯枚举、零 include 闭合**的上游接口类型 ⇒ 按
 *   ADR-0026 D-3 它就是"该 vendor 的上游原件", 抄一份 140 个枚举值到 `adapter/`
 *   反而是"把上游源码抄进 adapter/"(任务书 §1 禁止)。所以这里只做路径转发。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_SHIM_DM_UCLASS_ID_H
#define BR_ADAPTER_UBOOT_MMC_SHIM_DM_UCLASS_ID_H
#include "../../../../third_party/uboot-imx/include/dm/uclass-id.h"
#endif
