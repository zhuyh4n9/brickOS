/*
 * Fault (synchronous exception) path: slotting, EC classification, extable fixup, panic. Fault
 * stubs mask all of DAIF. Fault/ISR paths always use br_panic_bare() (no lock/heap/scheduler/trace
 * ring); br_panic() is the thread-side entry. Both are terminal.
 */
#ifndef BR_CORE_BR_FAULT_H
#define BR_CORE_BR_FAULT_H

#include <br/core/br_types.h>
#include <br/hal/br_exc.h>

/* ---- ESR_EL1.EC (exception class) values ---- */
#define BR_EC_UNKNOWN        0x00u   /* undefined instruction (corrupted/jumped code) => fatal */
#define BR_EC_FP_SIMD        0x07u   /* FP/SIMD access trap (CPACR_EL1.FPEN off) => fatal */
#define BR_EC_SVC64          0x15u   /* SVC64 at EL1 without syscall => fatal */
#define BR_EC_HVC64          0x16u
#define BR_EC_SMC64          0x17u
#define BR_EC_MSR_MRS        0x18u   /* trapped system register access => fatal (print encoding) */
#define BR_EC_IABT_LOWER     0x20u   /* instruction fetch to illegal address => fatal */
#define BR_EC_IABT_SAME      0x21u
#define BR_EC_DABT_LOWER     0x24u   /* illegal/permission/alignment access => extable first */
#define BR_EC_DABT_SAME      0x25u
#define BR_EC_BRK64          0x3Cu   /* breakpoint => v1 fatal; v2 forwarded to debug bridge */

/* ESR_EL1.EC field position */
#define BR_ESR_EC_SHIFT      26u
#define BR_ESR_EC_MASK       0x3Fu
#define BR_ESR_ISS_MASK      0x01FFFFFFu

/* ---- extable entries (ISA-layer mechanism, not part of the native API surface) ----
 * `reg` and `errno_val` are required: with only {pc, fixup} the runtime fault path cannot know
 * which register receives the error code ("=r"(ret) is a compiler constraint only). */
typedef struct {
    br_uintptr_t pc;        /* address of the possibly faulting instruction (compared with ELR_EL1) */
    br_uintptr_t fixup;     /* fixup label (written back to ELR_EL1 to continue) */
    br_u8        reg;       /* which general register receives the value (0..30; 31 = xzr means no write) */
    br_u8        rsv[3];
    br_s32       errno_val; /* value written (negative errno by convention) */
} br_extable_t;             /* 24 B */

_Static_assert(sizeof(br_extable_t) == 24u, "extable entry layout drift");

/*
 * Look up `pc` in the extable. On a hit the fault path rewrites ELR and writes the register
 * named by the entry. v0.1 scans the link-time-collected `.br_extable` section linearly: link
 * order is the only order and sorting in fault context is forbidden.
 */
const br_extable_t *br_extable_lookup(br_uintptr_t pc);

/*
 * Emit one extable entry. Must be used in the **same asm template** as the possibly faulting
 * access, with `%=` unique labels, e.g.:
 *   ".Lprobe" "%=:\n\t" ... ".Lfixup" "%=:\n\t" ... BR_EXTABLE_ENTRY(.Lprobe%=, .Lfixup%=, 0, -14)
 * `.quad <label>` refers to a `.L` local label inside this template, so there is no global
 * symbol (near-zero plugin export surface) and no runtime initialization.
 */
#define BR_EXTABLE_ENTRY(insn, fixup, regno, errval)                       \
    ".pushsection .br_extable, \"a\", %%progbits\n\t"                      \
    ".balign 8\n\t"                                                       \
    ".quad " #insn "\n\t"                                                 \
    ".quad " #fixup "\n\t"                                                \
    ".byte " #regno "\n\t"                                                \
    ".zero 3\n\t"                                                         \
    ".long " #errval "\n\t"                                               \
    ".popsection\n\t"

/* ---- entry and panic ---- */

/*
 * Full fault flow: in_fault++ (>=2 => double fault to bare panic) -> EC classification -> extable
 * fixup (hit => adjust frame and goto OUT) -> otherwise fatal panic. in_fault must be decremented
 * on every non-fatal exit or ramdump is ruined; single entry/single exit rules that out.
 */
void br_fault_enter(br_exc_frame_t *f);

/* Bare panic: usable in any context (ISR/fault/double fault). Polls the console for minimal
 * output then halts; no lock/heap/scheduler/trace ring write. Does not return. */
/* Panic channel details the fault path needs (implemented by `core/c_core/src/base/panic.c`):
 * `br_panic_in_progress()` lets a second fault halt silently instead of recursing through the
 * console (unbounded recursion exhausts the 64 KiB boot stack); `br_panic_halt()` is the in-place
 * stop with no printing. Declared here, not in a core-private header: `hal/int` owns the fault path
 * and must not reach into another plugin's `src/`. */
br_bool br_panic_in_progress(void);
BR_NORETURN void br_panic_halt(void);

BR_NORETURN void br_panic_bare(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* Thread-only panic (full path; v0.1 differs from bare by one banner line). Does not return. */
BR_NORETURN void br_panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* BR_CORE_BR_FAULT_H */
