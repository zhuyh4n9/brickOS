/*
 * brickOS prototype v0.1.0 — core 错误码
 *
 * 设计依据(3-01 §11「错误码」): core 的 int 返回一律用**负 errno**;
 * 域用子集 `-EIO/-EAGAIN/-EINVAL/-ENOMEM/-ENODEV/-ENOTSUP/-EBUSY/-EEXIST/-ETIMEDOUT`。
 * 中断组在此之上用到 `-EFAULT`(extable fixup 的约定错误码, 3-02 §10.4)。
 *
 * 为什么自带一份而不是 <errno.h>: 与 br_types.h 同理 —— 本原型是
 * -ffreestanding -nostdlib, 不把宿主 libc 的错误码表拖进目标镜像。
 * 数值沿用 Linux/aarch64 的 errno 编号, 便于与 svc-posix(D18)对齐。
 */
#ifndef BR_CORE_BR_ERROR_H
#define BR_CORE_BR_ERROR_H

/* 成功 */
#define BR_OK   0

/* errno 数值(Linux/aarch64 口径; 只取 core 域用到的子集) */
#define BR_EPERM        1
#define BR_EIO          5
#define BR_ENXIO        6
#define BR_EAGAIN      11
#define BR_ENOMEM      12
#define BR_EFAULT      14
#define BR_EBUSY       16
#define BR_EEXIST      17
#define BR_ENODEV      19
#define BR_EINVAL      22
#define BR_ENOSPC      28
#define BR_ENOTSUP     95
#define BR_ETIMEDOUT  110

/* 负值形式: 设计 3-01 §11 —— core 的 int 返回用负 errno */
#define BR_ERR(e)   (-(int)(e))

#endif /* BR_CORE_BR_ERROR_H */
