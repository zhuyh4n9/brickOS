/*
 * brickOS prototype v0.2.0 — core 错误码(errno; **与 Linux 内核逐值对齐**)
 *
 * 设计依据(3-01 §11「错误码」): core 的 int 返回一律用**负 errno**。
 * 各域的**可用子集**分别成文(SD-10: core 域见 3-01 §11; 设备/存储域见 8-01 §4),
 * 本头给的是**值的全集**, 不是"每个域都能返回所有这些码"。
 *
 * ## 真值来源(改本文件前先读这一条)
 *   数值**逐字取自 Linux 内核**(`include/uapi/asm-generic/errno-base.h` 与
 *   `include/uapi/asm-generic/errno.h`)—— 两者在 aarch64 与 x86-64 上同值,
 *   因此宿主 `<errno.h>` 是本表的**可执行对照物**: 宿主门禁 `ft-test`
 *   (`tests/host/ft_test.c`)逐码断言"本表的值 == 宿主的值", 任何一处抄错都会在那里红。
 *   (★ 该用例必须**先**捕获宿主的值、再 `#undef` 宿主同名宏、最后才包含本头 —— 因为
 *    本表的**名字与 POSIX 同名**(见下), 顺序反了就成了"自己跟自己比"。)
 *   ⇒ **新增值只能从内核表里抄, 不许自造编号**; 抄完把对照断言加进该门禁。
 *
 * ## 为什么自带一份而不是 <errno.h>
 *   与 br_types.h 同理 —— 本原型是 `-ffreestanding -nostdlib`, 不把宿主 libc 的
 *   错误码表拖进目标镜像。
 *
 * ## 收录口径(两段, 与内核头文件的分段一致)
 *   - **第一段 = `errno-base.h` 全集(1–34)**: 整表收录。理由不是"用得到", 而是
 *     "域子集是按语义裁的, 值表是按内核抄的" —— 只抄用到的会让"下一次需要某个码"
 *     变成一次抄写 + 一次对账; 整表收录则一次到位且不可能抄错一半。
 *   - **第二段 = `errno.h` 的 generic 部分, 只取本仓用到的**(35 起): 这一段有 100+
 *     个值, 其中大半属网络/内核模块域(v2 的 socket 面再按需补)。
 *
 * ## 与设计文档的既有差异(仍成立)
 *   8-01 §4 的 SD-10 设备/存储域子集是 `-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/`
 *   `-EBUSY/-EROFS`; 而 D23 的 lookup 链与"名字空间变更"还需要 `-ENOENT`(未命中不是
 *   参数错)、`-ENOTDIR`/`-EISDIR`、`-ENOTEMPTY`(硬塞 -EBUSY 会与"挂载点不可删"撞码)。
 *   这些都不在 SD-10 的枚举里, 但用 -EINVAL 把它们压成一个会让排障与 errno 映射同时
 *   失真, 故按内核编号补入(`docs/decisions/0009-…` §3 裁定 4)。
 *   `runtime/posix` 的 `errno = -ret` 是**零转换**取负(11-01 §2, ADR-0014)⇒ 本表的编号
 *   **就是**用户态看到的编号 —— 这一条是"对齐内核"的最终理由。
 *
 * ## 名字就是 POSIX 原名, 且与数值同处一地(ADR-0027)
 *   本头**同时**给出**名字**与**数值**, 且名字就是 POSIX 原名 —— `#define EIO 5`,
 *   **没有 `BR_` 前缀**。为什么不带前缀(这是本刀的关键):
 *     - 三方件按 ADR-0026 改为**原样编译上游**, 而上游到处是裸的 `-EIO`/`-EFSCORRUPTED`
 *       ⇒ 只有同名同值, 那些代码才**一个字都不用改**; 带前缀时唯一出路是往上游文件里塞
 *       一张 `#define EIO BR_EIO` 映射表 —— 那既是第二处真值, **也是对上游代码的修改**;
 *     - 名字与数值同处一地 ⇒ 不再有"名字表 + 数值表"两张表(ADR-0023 时是分开的)。
 *   `BR_ERR(e)`(`-(int)(e)`)保留: 它是"取负"的宏, 不是 errno 名; `BR_OK` 同理。
 *   `br/core/br_errno.h` 已退化为**转发头**(只 include 本头), 只为不改既有包含点。
 *   ★ `BR_` 前缀从 errno 名字上**撤掉**, 但本仓自有的标识符仍用它(`BR_EC_*` 异常类、
 *     `BR_EMMC_*`、`BR_EROFS_*`、`BR_ERR` 等)—— "编号是人家的, 名字也该是人家的;
 *     `BR_` 留给本仓自己发明的东西"。
 */
#ifndef BR_CORE_BR_ERROR_H
#define BR_CORE_BR_ERROR_H

/* 成功 */
#define BR_OK   0

/* ==================================================================== 第一段
 * errno-base.h 全集(1–34; 逐字取自内核)。 */
#define EPERM        1   /* Operation not permitted */
#define ENOENT       2   /* No such file or directory */
#define ESRCH        3   /* No such process */
#define EINTR        4   /* Interrupted system call */
#define EIO          5   /* I/O error */
#define ENXIO        6   /* No such device or address */
#define E2BIG        7   /* Argument list too long */
#define ENOEXEC      8   /* Exec format error */
#define EBADF        9   /* Bad file number */
#define ECHILD      10   /* No child processes */
#define EAGAIN      11   /* Try again */
#define ENOMEM      12   /* Out of memory */
#define EACCES      13   /* Permission denied */
#define EFAULT      14   /* Bad address(extable fixup 的约定错误码, 3-02 §10.4) */
#define ENOTBLK     15   /* Block device required */
#define EBUSY       16   /* Device or resource busy */
#define EEXIST      17   /* File exists */
#define EXDEV       18   /* Cross-device link */
#define ENODEV      19   /* No such device */
#define ENOTDIR     20   /* Not a directory */
#define EISDIR      21   /* Is a directory */
#define EINVAL      22   /* Invalid argument */
#define ENFILE      23   /* File table overflow */
#define EMFILE      24   /* Too many open files */
#define ENOTTY      25   /* Not a typewriter */
#define ETXTBSY     26   /* Text file busy */
#define EFBIG       27   /* File too large */
#define ENOSPC      28   /* No space left on device */
#define ESPIPE      29   /* Illegal seek */
#define EROFS       30   /* Read-only file system */
#define EMLINK      31   /* Too many links */
#define EPIPE       32   /* Broken pipe */
#define EDOM        33   /* Math argument out of domain of func */
#define ERANGE      34   /* Math result not representable */

/* ==================================================================== 第二段
 * errno.h 的 generic 部分(只取本仓用到的; 逐字取自内核)。 */
#define EDEADLK       35   /* Resource deadlock would occur */
#define ENAMETOOLONG  36   /* File name too long */
#define ENOLCK        37   /* No record locks available */
#define ENOSYS        38   /* Invalid system call number */
#define ENOTEMPTY     39   /* Directory not empty */
#define ELOOP         40   /* Too many symbolic links encountered */
#define EOVERFLOW     75   /* Value too large for defined data type */
#define ENOTSUP       95   /* Operation not supported(= EOPNOTSUPP) */
#define ETIMEDOUT    110   /* Connection timed out(阻塞超时统一, INV-1/CA-4) */
#define EUCLEAN      117   /* Structure needs cleaning —— EROFS 的 EFSCORRUPTED 用它:
                               * ADR-0023 D-4 "缺的 POSIX 码从内核表里补", 而不是映射到别的码 */
#define EDQUOT       122   /* Quota exceeded */
#define ECANCELED    125   /* Operation Canceled */
#define EOWNERDEAD   130   /* Owner died */
#define ENOTRECOVERABLE 131 /* State not recoverable */

/* ---- 同值别名(内核里就是两个名字; 保留两套以免消费者改码) ---- */
#define EWOULDBLOCK  EAGAIN      /* 11 */
#define EOPNOTSUPP   ENOTSUP     /* 95 */

/* 负值形式: 设计 3-01 §11 —— core 的 int 返回用负 errno */
#define BR_ERR(e)   (-(int)(e))

#endif /* BR_CORE_BR_ERROR_H */
