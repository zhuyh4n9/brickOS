/*
 * brickOS prototype v0.2.0 — POSIX 错误码面(runtime/posix 插件提供)
 *
 * ★★ 本头**不再定义任何名字或数值** —— 两样都在 core:
 *      - 数值(唯一真值): `br/core/br_error.h`(逐字取自 Linux 内核, 宿主 `ft-test` 逐码对拍);
 *      - POSIX 名字:     `br/core/br_errno.h`(逐条写成 `BR_E*` 的别名)。
 *    本头只负责 POSIX 运行时**独有**的那两件:
 *      ① `errno` 这个**宏**(每线程槽位);
 *      ② 本服务自己的呈现上界(`BR_POSIX_STRERROR_MAX`)。
 *
 * ★ 为什么"名字上收到 core"是对的(ADR-0023): 在那一版之前, POSIX 名字表挂在本插件上 ⇒
 *   ① 产品不选 `runtime/posix` 就一个 POSIX 名字都没有(而"内部错误码沿用 POSIX 编号"
 *      这条纪律不该依赖可选插件在不在场); ② 三方移植件(uboot-fat / uboot-erofs)只好各自
 *      抄一份 `#define EIO EIO` —— 既是第二处真值, 也是对上游代码的多余改动。
 *
 * ★ `errno` 是**宏**, 指向**每线程**槽位(v0.2 起): 槽位在本服务自己的线程记录里
 *   (`runtime/posix/src/posix.c` 的 `svc_thread_t`), 由 `br_posix_errno()` 取地址。
 *   于是"线程 A 的失败被线程 B 读走"这个全局 errno 的经典错误不再存在。core 仍没有
 *   通用 TLS 槽位(`11-02` P-4), 但 pthread 层本来就有"每线程记录"表 ⇒ 不必动 core
 *   (ADR-0019)。`pthread_key_*` 用的是同一张表的槽位。
 *   ⚠ 因此**不能**再写 `extern int errno;` —— POSIX 本来就把 `errno` 规定成宏。
 *   ★ 这一件**必须**留在本插件: 它要"每线程槽位", 而那是本服务的状态(core 无 TLS)。
 *     码归 core、槽位归 POSIX 运行时 —— 这条分工就是"内部错误码与 POSIX 运行时解耦"。
 *
 * ★ 用法照 POSIX: 函数失败返回 -1 并设 `errno`; 成功**不保证**清零 errno。
 */
#ifndef BR_POSIX_ERRNO_H
#define BR_POSIX_ERRNO_H

/* ★ 名字与数值都来自 core(一句再导出 ⇒ 与本插件在不在场无关)。 */
#include <br/core/br_errno.h>

/* 每线程 errno 槽位的地址(定义在 src/posix.c; 未注册的线程会惰性登记一条记录)。 */
int *br_posix_errno(void);

/* POSIX 规定 `errno` 是可修改左值 ⇒ 宏展开成解引用。 */
#define errno   (*br_posix_errno())

/* 成功码不叫 E*: 本原型用 0(POSIX 亦然)。保留旧名(它是本插件对外面的既有符号)。 */
#define BR_POSIX_OK   0

/* `strerror` 的最大缓冲(含结尾 NUL)—— 调用方给出的缓冲至少这么大就不会被截断。 */
#define BR_POSIX_STRERROR_MAX  40

#endif /* BR_POSIX_ERRNO_H */
