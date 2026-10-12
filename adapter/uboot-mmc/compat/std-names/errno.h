/* adapter/uboot-mmc — **标准头名** `<errno.h>` 的单元级环境补丁(见 plugin.toml 的偏离说明)
 *
 * ============================== 为什么它在 `std-names/` 而不在 `compat/` ==============================
 * 上游 `drivers/mmc/` 的原件里有字面 `#include <errno.h>`(u-boot 的 `mmc.c` 第 15 行)。
 * 本件是裸机 unikernel, 没有 libc 的 `<errno.h>` ⇒ 必须给它一个。
 *
 * 但 `errno.h` 是**标准头名**, 而 `runtime/posix/include/errno.h` 也叫这个名字。brickie 的
 * 包含面判据(ADR-0018)是**名字级**的: 它把"每个插件 `[build].includes` 列出的目录下的
 * 文件名"建成"名字 → 归属插件"的映射, 再看谁的字面 include 命中 —— **看不到 per-unit 的
 * `-I`**。于是把本文件放进 `compat/`(声明进 `includes`)会让 `runtime/posix/src/posix.c`
 * 与 `iface/posix` 的 `<errno.h>` 被判成"include 了 adapter/uboot-mmc 的头"(7 条
 * BRV-MF-0001) —— 而它们的 `-I` 里根本没有本目录, 那是**误报**。
 *
 * ⇒ 处置(与 `plugin.toml` 的 `[build]` 注释、`ADAPTER-NOTES.md` 的"偏离"一节同一处真值):
 *   把两个**标准头名**的壳放进 `compat/std-names/`, **不列进 `includes`**, 改由
 *   `[build].cflags` 里的一条 `-Iadapter/uboot-mmc/compat/std-names` 下发 —— 那条 `-I`
 *   只对本插件的编译单元生效, 于是"名字不进声明面, 单元仍找得到头"。
 *   回归条件: brickie 的包含面判据改为按 **per-unit `-I`** 解析(或显式排除标准 C 头名)时,
 *   这两个壳搬回 `compat/` 并列入 `includes`。
 *
 * ★ 语义: 上游用到的就是那批裸名字, 而 ADR-0027 已把 errno 的名字改成 POSIX 原名且与数值
 *   同处 `br/core/br_error.h` ⇒ 这里**只做转发**(不含任何第二处真值)。
 */
#ifndef BR_ADAPTER_UBOOT_MMC_STDNAME_ERRNO_H
#define BR_ADAPTER_UBOOT_MMC_STDNAME_ERRNO_H
#include "../mmc_env.h"
#endif
