/*
 * Core basic types (br_ prefix). Freestanding build (-ffreestanding -nostdlib): self-contained
 * so no host glibc headers are dragged into the target image. Widths are asserted below.
 */
#ifndef BR_CORE_BR_TYPES_H
#define BR_CORE_BR_TYPES_H

typedef unsigned char      br_u8;
typedef signed char        br_s8;
typedef unsigned short     br_u16;
typedef signed short       br_s16;
typedef unsigned int       br_u32;
typedef signed int         br_s32;
typedef unsigned long      br_u64;   /* aarch64 LP64: unsigned long = 64 bits */
typedef signed long        br_s64;
typedef unsigned long      br_size_t;
typedef unsigned long      br_uintptr_t;
typedef signed long        br_intptr_t;
typedef int                br_bool;

#define BR_TRUE   1
#define BR_FALSE  0

#define BR_NULL   ((void *)0)

/* A wrong type width breaks everything downstream. */
_Static_assert(sizeof(br_u8)  == 1, "br_u8 must be 8-bit");
_Static_assert(sizeof(br_u16) == 2, "br_u16 must be 16-bit");
_Static_assert(sizeof(br_u32) == 4, "br_u32 must be 32-bit");
_Static_assert(sizeof(br_u64) == 8, "br_u64 must be 64-bit");

#define BR_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#if defined(__GNUC__)
#  define BR_NORETURN   __attribute__((noreturn))
#  define BR_UNUSED     __attribute__((unused))
#  define BR_PACKED     __attribute__((packed))
#  define BR_ALIGN(n)   __attribute__((aligned(n)))
#else
#  define BR_NORETURN
#  define BR_UNUSED
#  define BR_PACKED
#  define BR_ALIGN(n)
#endif

#endif /* BR_CORE_BR_TYPES_H */
