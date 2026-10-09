/*
 * brickOS prototype v0.2.0 — POSIX 基本类型(runtime/posix 插件提供)
 *
 * ============================== 这是什么 ==============================
 * `runtime/posix` 是设计 D18 里的 **POSIX 运行时服务**(fd 表唯一主人 + POSIX 符号面 +
 * libc stub), 本目录是它抛给消费者的 **POSIX 头文件面**。
 *
 * ★ 与 `br_*` 的关系: 这里的类型**全部是 `br_*` 的别名或薄包装** —— 没有第二套语义。
 *   例如 `typedef long ssize_t` 与 core 的 `br_s64` 同宽, 于是 `read()` 的返回值直接
 *   就是 `br_file_read()` 的返回值; `errno = -ret` 是**零转换**(设计 `11-01` §2)。
 *   这一层存在的唯一理由是"标准的名字", 不是为了再包一层抽象。
 *
 * ★ 子集诚实义务(`1-01` §7.6 的代价 2): **本目录只声明实现了的东西**。没有 `gettimeofday`,
 *   因为原型没有墙钟源(见 `docs/11-service/11-02-svc-posix-subset.md` 的 P-1); 没有
 *   `fork`/`exec`/信号, 因为设计前提是单地址空间(`1-01` §2.1)。写了却链不上比不写更糟。
 */
#ifndef BR_POSIX_SYS_TYPES_H
#define BR_POSIX_SYS_TYPES_H

/* size_t / ptrdiff_t 走编译器内建: 本原型是 -ffreestanding, 没有 <stddef.h> 的依赖
 * (GCC/Clang 都提供这两个宏), 于是"我们的 size_t 与编译器的 size_t 是不是同一个"
 * 这个问题根本不出现。 */
#ifndef _BR_POSIX_SIZE_T_DEFINED
#define _BR_POSIX_SIZE_T_DEFINED
typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#endif

#ifndef _BR_POSIX_SSIZE_T_DEFINED
#define _BR_POSIX_SSIZE_T_DEFINED
typedef long ssize_t;      /* 与 core 的 br_s64 同宽(64 位) */
typedef long off_t;        /* 与 br_s64 同宽 */
#endif

typedef unsigned int  mode_t;
typedef int           pid_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned int  dev_t;
typedef unsigned long ino_t;
typedef unsigned long nlink_t;
typedef unsigned int  blksize_t;
typedef long          blkcnt_t;
typedef long          time_t;          /* 秒(见 time.h 的"没有墙钟"说明) */
typedef long          suseconds_t;
typedef unsigned int  useconds_t;
typedef unsigned long id_t;

/* `SEEK_*`: POSIX 把它们分别声明在 <stdio.h> 与 <unistd.h>; 这里放在**基类型头**里是
 * 刻意的 —— 本原型是 freestanding, 少一处"某个头恰好包含了另一个头"的隐含依赖,
 * 就少一类"换个包含顺序编不过"。值照 Linux(0/1/2)。 */
#ifndef SEEK_SET
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#endif

#endif /* BR_POSIX_SYS_TYPES_H */
