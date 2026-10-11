/*
 * brickOS prototype v0.2.0 — POSIX 错误码**名字**(core 提供; ADR-0023)
 *
 * 设计依据(3-01 §11「错误码」): core 的 int 返回一律用**负 errno**; 各域的可用子集
 * 分别成文(SD-10)。本头补的是**另一套拼写**, 不是另一张表。
 *
 * ============================== 这一件要解决什么 ==============================
 * 在 ADR-0023 之前, POSIX 的裸名字(`EIO`/`ENOENT`/`EROFS`/…)定义在
 * `runtime/posix/include/errno.h` 里。于是有两个后果:
 *
 *   ① **产品不选 POSIX 运行时就一个 POSIX 名字都没有**。而"内部错误码沿用 POSIX 编号"
 *      这条纪律不该依赖某个可选插件在不在场 —— 编号的真值是 core 的(`br_error.h`),
 *      名字也该在 core。
 *   ② **三方移植件只好各自抄一份映射表**。u-boot 的 `fs/fat` 与 `fs/erofs` 里到处是
 *      裸的 `-EIO`/`-ENOENT`/`-EFSCORRUPTED`, 而移植件的硬约束是"**尽量不改上游代码**"
 *      ⇒ 每个插件的 compat 层都写了一串 `#define EIO BR_EIO`。那串东西既是**第二处真值**
 *      (谁都能改错一个), 又是**对上游代码的多余改动**(本该只是"环境里就有这些名字")。
 *
 * ⇒ 本头把那份名字表**上收到 core**: 与 `br_error.h` 的数值**一一对应**(逐条写成
 *   `BR_E*` 的别名, 值由构造保证相同), 于是
 *     - 有没有 `runtime/posix` 都拿得到 POSIX 编号(要求 ①);
 *     - 三方插件只要 `#include <br/core/br_errno.h>`, 上游代码里那些裸名字**原样成立**
 *       (要求 ②)。`EFSCORRUPTED`(内核 `EUCLEAN = 117`)这类码见 D-4: **从内核表里补**,
 *       不许映射到别的码上。
 *
 * ============================== 名字与数值的关系 ==============================
 *   本头**不定义任何数值**。值只有一处真值: `br_error.h`(逐字取自 Linux 内核
 *   `asm-generic/errno-base.h` / `errno.h`)。因此 "POSIX 名字 == POSIX 编号" 这条性质
 *   由两条判据合起来保证(都不靠人读):
 *     - **值**: 宿主门禁 `ft-test` 拿宿主 `<errno.h>` 逐码对拍 `BR_E*`(ADR-0012);
 *     - **名**: 脚本门禁 `check-errno` 断言本头每个 `E*` 都恰好绑定到同名 `BR_E*`,
 *       且 `br_error.h` 的每个 `BR_E*` 都在这里有名字(**全集**, 没有"漏一个名字"),
 *       并且**全树没有第二处 errno 表**。
 *   两条合起来 ⇒ 每个 `E*` 的值都等于宿主 POSIX 的值。改本头前请先读这两条判据。
 *
 * ============================== 与 <errno.h> 的关系 ==============================
 *   `runtime/posix/include/errno.h` 现在**只**做三件事: 再导出本头、给 `errno` 这个
 *   宏(每线程槽位)、给 `strerror` 的缓冲上界。数值/名字那一大段不再有两份。
 *   ★ POSIX 规定 `errno` 是宏 ⇒ 那件事**只能**由 POSIX 运行时给: 它需要"每线程槽位",
 *   而 core 没有通用 TLS(`11-02` P-4)。本头只给**码**, 不给 `errno` 变量 —— 这条分工
 *   正是"内部错误码与 POSIX 运行时解耦"的落点。
 */
#ifndef BR_CORE_BR_ERRNO_H
#define BR_CORE_BR_ERRNO_H

#include <br/core/br_error.h>

/* ---- errno-base.h 全集(1–34)---- */
#define EPERM        BR_EPERM
#define ENOENT       BR_ENOENT
#define ESRCH        BR_ESRCH
#define EINTR        BR_EINTR
#define EIO          BR_EIO
#define ENXIO        BR_ENXIO
#define E2BIG        BR_E2BIG
#define ENOEXEC      BR_ENOEXEC
#define EBADF        BR_EBADF
#define ECHILD       BR_ECHILD
#define EAGAIN       BR_EAGAIN
#define ENOMEM       BR_ENOMEM
#define EACCES       BR_EACCES
#define EFAULT       BR_EFAULT
#define ENOTBLK      BR_ENOTBLK
#define EBUSY        BR_EBUSY
#define EEXIST       BR_EEXIST
#define EXDEV        BR_EXDEV
#define ENODEV       BR_ENODEV
#define ENOTDIR      BR_ENOTDIR
#define EISDIR       BR_EISDIR
#define EINVAL       BR_EINVAL
#define ENFILE       BR_ENFILE
#define EMFILE       BR_EMFILE
#define ENOTTY       BR_ENOTTY
#define ETXTBSY      BR_ETXTBSY
#define EFBIG        BR_EFBIG
#define ENOSPC       BR_ENOSPC
#define ESPIPE       BR_ESPIPE
#define EROFS        BR_EROFS
#define EMLINK       BR_EMLINK
#define EPIPE        BR_EPIPE
#define EDOM         BR_EDOM
#define ERANGE       BR_ERANGE

/* ---- errno.h 的 generic 部分(本仓收录的那些; 按内核编号序)---- */
#define EDEADLK          BR_EDEADLK
#define ENAMETOOLONG     BR_ENAMETOOLONG
#define ENOLCK           BR_ENOLCK
#define ENOSYS           BR_ENOSYS
#define ENOTEMPTY        BR_ENOTEMPTY
#define ELOOP            BR_ELOOP
#define EOVERFLOW        BR_EOVERFLOW
#define ENOTSUP          BR_ENOTSUP
#define ETIMEDOUT        BR_ETIMEDOUT
#define EUCLEAN          BR_EUCLEAN
#define EDQUOT           BR_EDQUOT
#define ECANCELED        BR_ECANCELED
#define EOWNERDEAD       BR_EOWNERDEAD
#define ENOTRECOVERABLE  BR_ENOTRECOVERABLE

/* ---- 同值别名(内核里就是两个名字; 两套都留, 免得消费者改码) ---- */
#define EOPNOTSUPP       BR_EOPNOTSUPP
#define EWOULDBLOCK      BR_EWOULDBLOCK

#endif /* BR_CORE_BR_ERRNO_H */
