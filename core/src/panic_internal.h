/*
 * brickOS prototype v0.1.0 — core-private interface of the panic channel (not part of
 * the public contract headers)
 *
 * Why a private header: `br_panic_bare`/`br_panic` are core's public observation contract
 * (`br_fault.h`), while these two symbols serve only **one reentrancy guard inside core**
 * (the fault path asks "am I currently in panic output"). Putting them in `br/core/`
 * would make them plugin-visible, and they have no consumers.
 *
 * Background (measured, see ADR-0003 §5): panic output goes through the polled console;
 * if the console itself is already inaccessible, "panic → fault → panic again" becomes
 * **unbounded recursion**, pushing a 0x140 B exception frame each time; after the 64 KiB
 * boot stack is consumed the frames land in .bss (measured: SP falls into the page
 * tables, and once those are overwritten even instruction fetch cannot be translated) —
 * the result is that the **original fault context is completely lost**. With these two
 * symbols, the fault path can **halt silently** when it faults again during panic output:
 * if the first panic statement gets out, it prints; if not, it stops quietly.
 */
#ifndef BR_CORE_PANIC_INTERNAL_H
#define BR_CORE_PANIC_INTERNAL_H

#include <br/core/br_types.h>

/* Whether panic output is in progress (set at panic entry, never cleared — it is
 * terminal). */
br_bool br_panic_in_progress(void);

/* In-place halt with no printing, no locking, no reliance on stack resources (the
 * simplest form of panic). Does not return. */
BR_NORETURN void br_panic_halt(void);

#endif /* BR_CORE_PANIC_INTERNAL_H */
