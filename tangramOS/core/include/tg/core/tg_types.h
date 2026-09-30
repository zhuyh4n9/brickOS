/*
 * TangramOS prototype v0.1.0 — core 基础类型
 *
 * 为什么不用 <stdint.h>: 本原型是 freestanding 构建(-ffreestanding -nostdlib),
 * 只依赖编译器自带头文件。类型自持, 避免把宿主的 glibc 头文件拖进目标镜像。
 *
 * 命名遵循设计侧纪律: core 独占 `tg_` 前缀(设计文档 1-01 §7.2 规则 4)。
 * 类型表最终归属: `docs/3-os-core/3-01-core-api-list.md`(该文档在 tangramOS-Design
 * 分支上); v0.1.0 只取其中被 MainLoop 用到的最小子集。
 */
#ifndef TG_CORE_TG_TYPES_H
#define TG_CORE_TG_TYPES_H

typedef unsigned char      tg_u8;
typedef signed char        tg_s8;
typedef unsigned short     tg_u16;
typedef signed short       tg_s16;
typedef unsigned int       tg_u32;
typedef signed int         tg_s32;
typedef unsigned long      tg_u64;   /* aarch64 LP64: unsigned long = 64 位 */
typedef signed long        tg_s64;
typedef unsigned long      tg_size_t;
typedef unsigned long      tg_uintptr_t;
typedef signed long        tg_intptr_t;
typedef int                tg_bool;

#define TG_TRUE   1
#define TG_FALSE  0

#define TG_NULL   ((void *)0)

/* 编译期自检: 类型宽度错了一切都错 */
_Static_assert(sizeof(tg_u8)  == 1, "tg_u8 must be 8-bit");
_Static_assert(sizeof(tg_u16) == 2, "tg_u16 must be 16-bit");
_Static_assert(sizeof(tg_u32) == 4, "tg_u32 must be 32-bit");
_Static_assert(sizeof(tg_u64) == 8, "tg_u64 must be 64-bit");

#define TG_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#if defined(__GNUC__)
#  define TG_NORETURN   __attribute__((noreturn))
#  define TG_UNUSED     __attribute__((unused))
#  define TG_PACKED     __attribute__((packed))
#  define TG_ALIGN(n)   __attribute__((aligned(n)))
#else
#  define TG_NORETURN
#  define TG_UNUSED
#  define TG_PACKED
#  define TG_ALIGN(n)
#endif

#endif /* TG_CORE_TG_TYPES_H */
