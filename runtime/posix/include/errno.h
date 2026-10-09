/*
 * brickOS prototype v0.2.0 — POSIX 错误码(runtime/posix 插件提供)
 *
 * ★★ 本头不做任何数值定义: 每个 `E*` 都是 core `br_error.h` 里同名 `BR_E*` 的**别名**。
 *    于是"errno 值与 Linux 内核一致"这件事**由构造保证** —— 不可能出现"core 改了值、
 *    POSIX 面没跟上"的漂移(那是两份真值)。core 那张表的真值来源与对拍判据见 ADR-0012:
 *    编号逐字取自内核 `asm-generic/errno-base.h` / `errno.h`, 并由宿主门禁 `ft-test`
 *    拿宿主 `<errno.h>` 逐码对拍。
 *
 * ★ `errno` 是**宏**, 指向**每线程**槽位(v0.2 起): 槽位在本服务自己的线程记录里
 *   (`runtime/posix/src/posix.c` 的 `svc_thread_t`), 由 `br_posix_errno()` 取地址。
 *   于是"线程 A 的失败被线程 B 读走"这个全局 errno 的经典错误不再存在。core 仍没有
 *   通用 TLS 槽位(`11-02` P-4), 但 pthread 层本来就有"每线程记录"表 ⇒ 不必动 core
 *   (ADR-0019)。`pthread_key_*` 用的是同一张表的槽位。
 *   ⚠ 因此**不能**再写 `extern int errno;` —— POSIX 本来就把 `errno` 规定成宏。
 *
 * ★ 用法照 POSIX: 函数失败返回 -1 并设 `errno`; 成功**不保证**清零 errno。
 */
#ifndef BR_POSIX_ERRNO_H
#define BR_POSIX_ERRNO_H

#include <br/core/br_error.h>

/* 每线程 errno 槽位的地址(定义在 src/posix.c; 未注册的线程会惰性登记一条记录)。 */
int *br_posix_errno(void);

/* POSIX 规定 `errno` 是可修改左值 ⇒ 宏展开成解引用。 */
#define errno   (*br_posix_errno())

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

/* ---- generic 部分(按需; 未实现的域不在此列)---- */
#define EDEADLK          BR_EDEADLK
#define ENAMETOOLONG     BR_ENAMETOOLONG
#define ENOLCK           BR_ENOLCK
#define ENOSYS           BR_ENOSYS
#define ENOTEMPTY        BR_ENOTEMPTY
#define ELOOP            BR_ELOOP
#define EOVERFLOW        BR_EOVERFLOW
#define ENOTSUP          BR_ENOTSUP
#define EOPNOTSUPP       BR_EOPNOTSUPP
#define ETIMEDOUT        BR_ETIMEDOUT
#define EDQUOT           BR_EDQUOT
#define ECANCELED        BR_ECANCELED
#define EOWNERDEAD       BR_EOWNERDEAD
#define ENOTRECOVERABLE  BR_ENOTRECOVERABLE
#define EWOULDBLOCK      BR_EWOULDBLOCK

/* 成功码不叫 E*: 本原型用 0(POSIX 亦然) */
#define BR_POSIX_OK   0

/* `strerror` 的最大缓冲(含结尾 NUL)—— 调用方给出的缓冲至少这么大就不会被截断。 */
#define BR_POSIX_STRERROR_MAX  40

#endif /* BR_POSIX_ERRNO_H */
