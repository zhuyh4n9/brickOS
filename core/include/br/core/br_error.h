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
 *
 * ★ 存储/设备域的补充(append-only; 见 `docs/decisions/0009-vfs-storage-stack.md` §3 裁定 4):
 *   `8-01` §4 的 SD-10 存储域子集是 `-EIO/-ENODEV/-ENOSPC/-EINVAL/-ENOTSUP/-EBUSY/-EROFS`;
 *   而 D23 的 lookup 链与"名字空间变更"还需要:
 *     - `-ENOENT`(lookup 未命中: "不存在"不是"参数错", 更不是"设备不在");
 *     - `-ENOTDIR` / `-EISDIR`(路径分量不是目录 / 对目录做文件操作);
 *     - `-ENOTEMPTY`(rmdir 非空目录 —— 硬塞 -EBUSY 会与"挂载点不可删"撞码)。
 *   这些都不在 SD-10 的枚举里, 但用 -EINVAL 把它们压成一个会让排障与 svc-posix 的 errno
 *   映射同时失真, 故按 Linux 编号补入。`-EROFS` 本就在 SD-10 子集里, 此处一并补齐定义。
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
#define BR_ENOENT       2   /* 存储域补充: lookup 未命中 */
#define BR_ENOTDIR     20   /* 存储域补充: 路径分量不是目录 */
#define BR_EISDIR      21   /* 存储域补充: 对目录做文件操作 */
#define BR_EINVAL      22
#define BR_ENOSPC      28
#define BR_EROFS       30   /* 只读 FS(SD-10 存储域子集; fs/erofs(v2) 用) */
#define BR_ENOTEMPTY   39   /* 存储域补充: rmdir 非空目录 */
#define BR_ENOTSUP     95
#define BR_ETIMEDOUT  110

/* 负值形式: 设计 3-01 §11 —— core 的 int 返回用负 errno */
#define BR_ERR(e)   (-(int)(e))

#endif /* BR_CORE_BR_ERROR_H */
