/* adapter/uboot-erofs — `<u-boot/lz4.h>` 的适配壳。
 *
 * 上游 `fs/erofs/decompress.c` 写 `#include <u-boot/lz4.h>`。本文件把它**转到
 * third_party/ 里的那份上游原件**上 —— 于是我们既不复制它的内容, 也不改动它。
 *
 * ★ 为什么用相对路径而不是 -I 到 third_party/uboot-imx/include:
 *   brickie 对每个编译单元的 `-I` 是**按字典序**排的(见 build.rs 的 unit_includes),
 *   而 `adapter/uboot-erofs/../../third_party/...` 里的 `..` 排在 `compat` 前面 ⇒
 *   third_party/include 会**盖住**我们的 compat 壳(`<fs.h>`/`<part.h>` 会被解析到
 *   u-boot 的真头, 而那份还要 rtc_def.h 之类 → 编不过)。显式相对路径没有这个歧义。
 */
#include "../../../../third_party/uboot-imx/include/u-boot/lz4.h"
