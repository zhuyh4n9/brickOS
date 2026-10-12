/*
 * brickOS prototype v0.1.0 — fault (synchronous exception) path: slotting / classing / extable / panic
 *
 * Design basis: `docs/3-os-core/3-02-int.md` §10 ——
 *   §10.2 vector table slotting and **mask handling** (fault stubs always mask all of DAIF explicitly, no reliance on hardware behavior);
 *   §10.3 EC classification and decision table;
 *   §10.4 extable (fixup: illegal access downgraded to an error return code);
 *   §10.5 handler chain (**registration API is v2**, not implemented here; signatures fixed in 3-02 §14.5);
 *   §10.6 double fault fallback (calls no handler, goes straight to `br_panic_bare`).
 *
 * ★ Two panic entries (3-02 §8.3.1): ISR/fault paths **always** use `br_panic_bare()`
 *   (no lock/heap/scheduler/trace ring); `br_panic()` is the thread-side convenience entry.
 *   Both are terminal and do not return.
 */
#ifndef BR_CORE_BR_FAULT_H
#define BR_CORE_BR_FAULT_H

#include <br/core/br_types.h>
#include <br/core/br_exc.h>

/* ---- ESR_EL1.EC (exception class) values (decision table of 3-02 §10.3) ---- */
#define BR_EC_UNKNOWN        0x00u   /* undefined instruction (corrupted/jumped code) ⇒ fatal */
#define BR_EC_FP_SIMD        0x07u   /* FP/SIMD access trap (CPACR_EL1.FPEN off) ⇒ fatal, **must be named** */
#define BR_EC_SVC64          0x15u   /* SVC64 at EL1 without syscall ⇒ fatal */
#define BR_EC_HVC64          0x16u
#define BR_EC_SMC64          0x17u
#define BR_EC_MSR_MRS        0x18u   /* trapped system register access ⇒ fatal (print encoding) */
#define BR_EC_IABT_LOWER     0x20u   /* instruction fetch to illegal/unprivileged address ⇒ fatal */
#define BR_EC_IABT_SAME      0x21u
#define BR_EC_DABT_LOWER     0x24u   /* illegal/permission/alignment data access ⇒ **check extable first**, fatal only if miss */
#define BR_EC_DABT_SAME      0x25u
#define BR_EC_BRK64          0x3Cu   /* breakpoint ⇒ v1 fatal; v2 forwarded to debug bridge */

/* ESR_EL1.EC field position */
#define BR_ESR_EC_SHIFT      26u
#define BR_ESR_EC_MASK       0x3Fu
#define BR_ESR_ISS_MASK      0x01FFFFFFu

/* ---- extable (3-02 §10.4; ISA-layer mechanism, **not part of the native API surface**) ----
 *
 * ★ Why entries must carry `reg` and `errno_val`: with only `{pc, fixup}`, the fixup path
 *   has **no way to know** which register to put the error code in — `"=r"(ret)` is only a
 *   compiler constraint, invisible to the runtime fault path. Without these two fields the
 *   two named consumers (debug bridge MEMRD, driver probe self-test) **cannot exist**.
 */
typedef struct {
    br_uintptr_t pc;        /* address of the possibly faulting instruction (compared with ELR_EL1) */
    br_uintptr_t fixup;     /* fixup label (written back to ELR_EL1 to continue) */
    br_u8        reg;       /* which general register receives the value (0..30; 31 = xzr means no write) */
    br_u8        rsv[3];
    br_s32       errno_val; /* value written (negative errno by convention) */
} br_extable_t;             /* 24 B */

_Static_assert(sizeof(br_extable_t) == 24u, "extable entry layout drift");

/*
 * Look up pc in the extable. Hit ⇒ caller (fault path) rewrites ELR and writes the register per entry.
 * v0.1 implementation: **linear scan** over the link-time-collected `.br_extable` section (entry count is single digits).
 * Design §10.4 expects "ordered by pc ⇒ binary search": the section is collected by the linker,
 * order = link order, and there is **no runtime-initialization-free sort point** (sorting in fault
 * context is forbidden), so v0.1 picks linear; once entries grow, the ISA layer will emit an ordered
 * table at link time (implementation decision P-IRQ-2 in the README).
 */
const br_extable_t *br_extable_lookup(br_uintptr_t pc);

/*
 * Emit one extable entry.
 *
 * Usage (must be in the **same asm template** as the possibly faulting access; use `%=` for unique labels):
 *
 *   __asm__ volatile(
 *       ".Lprobe" "%=:\n\t"                 // insn label
 *       "   ldr %w[tmp], [%[addr]]\n\t"
 *       "   mov %w[ret], wzr\n\t"
 *       "   b   .Lfixdone" "%=\n\t"
 *       ".Lfixup" "%=:\n\t"                 // fixup label
 *       "   mov %w[ret], %w[baderr]\n\t"
 *       ".Lfixdone" "%=\n\t"
 *       BR_EXTABLE_ENTRY(.Lprobe%=, .Lfixup%=, 0, -14)   // reg=0(x0), errno=-EFAULT
 *       : ... );
 *
 * `.quad <label>` is an assembly-time reference to a `.L` local label **inside this asm template**
 * ⇒ no global symbol (CA-10: near-zero plugin export surface) and no runtime initialization.
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

/* ---- entry and panic (3-02 §14.2 / §8.3.1) ---- */

/*
 * Full fault flow: `in_fault++` (≥2 ⇒ double fault goes to bare panic) → EC classification →
 * extable fixup (hit ⇒ adjust frame + `goto OUT`) → otherwise fatal panic.
 * ★ `in_fault` must be decremented on **every non-fatal exit path** (3-02 §10.6): a leak ruins ramdump.
 *   The implementation uses a single entry/single exit structure to rule out errors (all non-fatal branches `goto OUT`).
 */
void br_fault_enter(br_exc_frame_t *f);

/*
 * bare panic: usable in any context (including ISR/fault/double fault).
 * Only polls the console for minimal output + halts; **no lock/heap/scheduler/trace ring write**
 * (same principle as 5-01 §2 "panic channel is independent"). Does not return.
 */
BR_NORETURN void br_panic_bare(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* thread-only panic (full path; in v0.1 differs from bare only by one banner line). Does not return. */
BR_NORETURN void br_panic(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif /* BR_CORE_BR_FAULT_H */
