/* adapter/uboot-fat — `<linux/compiler.h>` 的适配壳(不是转发)。
 *
 * ★ 这里**刻意不**转发 vendor 的 `include/linux/compiler.h`, 理由是一条被编译器逼出来的
 *   事实(探针实测): 那份头 include `<linux/compiler_types.h>` → `<linux/compiler_attributes.h>`
 *   → `<linux/compiler-gcc.h>`/`<asm/compiler.h>` → …, 以及 `<linux/types.h>` →
 *   `<linux/posix_types.h>`/`<asm/types.h>`/`<stdbool.h>`。后者会把 u-boot 的**类型层**
 *   (u8/u16/uintN_t/bool/ulong)整族拖进来, 与本环境(`fat_env.h`, 适配层给的 u-boot 运行
 *   环境)正面冲突 —— 而 ADR-0026 D-3 把"类型/宏"这一层明确归给 `compat/`。
 *
 *   ⇒ 边界划在这里: **盘上格式与结构**(`include/fat.h`/`include/fs.h`/`include/rtc_def.h`)
 *     转发 vendor 原件(那才是"内容只有一处真值"的地方); **类型与编译期属性宏**由本环境
 *     提供(上游从 `<linux/compiler.h>` 真正用到的只有 `__maybe_unused` 这一个)。
 *     这是"适配 = 给上游它依赖的环境"的直接后果, 已登记在 NOTICE 的偏离清单。
 */
#ifndef BR_ADAPTER_UBOOT_FAT_SHIM_LINUX_COMPILER_H
#define BR_ADAPTER_UBOOT_FAT_SHIM_LINUX_COMPILER_H
#include "../fat_env.h"
#endif
