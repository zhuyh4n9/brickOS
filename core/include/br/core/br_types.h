/*
 * brickOS prototype v0.1.0 - core basic types
 *
 * Why not <stdint.h>: this prototype is a freestanding build (-ffreestanding -nostdlib) and
 * depends only on compiler-provided headers. Types are self-contained to avoid dragging the
 * host's glibc headers into the target image.
 *
 * Naming follows the design-side discipline: core owns the `br_` prefix (design doc 1-01 §7.2
 * rule 4). The type table ultimately belongs to `docs/3-os-core/3-01-core-api-list.md` (on the
 * brickOS-Design branch); v0.1.0 takes only the minimal subset used by MainLoop.
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

/* Compile-time self-check: if a type width is wrong, everything is wrong */
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
