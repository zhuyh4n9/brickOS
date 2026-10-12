/*
 * brickOS — POSIX errno 名字的**转发头**(历史入口; 真值已归 `br_error.h`)
 *
 * ============================== 本文件为什么还在 ==============================
 * ADR-0023 曾把"POSIX 名字"单独放在这里(`#define EPERM BR_EPERM` 一张别名表),
 * 而**数值**在 `br_error.h`。ADR-0027 把两者合并了:
 *
 *   - 名字与数值现在**同在 `br_error.h`**, 且名字就是 **POSIX 原名**(`EPERM`/`EIO`/…),
 *     **没有 `BR_` 前缀** —— 于是:
 *       ① 三方移植件里的裸 `-EIO`/`-EFSCORRUPTED` **原样成立**(不必再写映射表);
 *       ② "errno 的名字"与本项目自己的符号前缀不再混在一起(设计 §11 的口径);
 *   - 本文件保留为**兼容入口**: 既有 `#include <br/core/br_errno.h>` 的代码不用改。
 *
 * ★ 单一真值: 本文件**不定义任何名字**(转发而已)。判据见 `tools/check-errno.sh`。
 */
#ifndef BR_CORE_BR_ERRNO_H
#define BR_CORE_BR_ERRNO_H

#include <br/core/br_error.h>

#endif /* BR_CORE_BR_ERRNO_H */
