/*
 * Core-private panic channel interface (not part of the public contract headers).
 * Callable from ISR/fault context: no allocation, no locks, no stack reliance.
 * br_panic_in_progress() lets the fault path halt silently instead of recursing when it
 * faults again during panic output (unbounded recursion exhausts the 64 KiB boot stack).
 */
#ifndef BR_CORE_PANIC_INTERNAL_H
#define BR_CORE_PANIC_INTERNAL_H

#include <br/core/br_types.h>

/* Whether panic output is in progress (set at panic entry, never cleared -- terminal). */
br_bool br_panic_in_progress(void);

/* In-place halt: no printing, no locking, no stack resources. Does not return. */
BR_NORETURN void br_panic_halt(void);

#endif /* BR_CORE_PANIC_INTERNAL_H */
