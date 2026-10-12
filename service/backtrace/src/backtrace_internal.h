/*
 * brickOS prototype v0.2.0 — service/backtrace **plugin-private** internal contract (not exported, not in include/)
 *
 * Audience: this plugin's own `src/backtrace_selftest.c`. Living in `src/` (rather than `include/`) is
 * deliberate — everything under `include/` is **plugin-outward surface** (governed by `brickie check`'s
 * interface domain, and part of `descriptor_include` selection); the two entries here are needed **only
 * by the selftest**.
 *
 * ## Why the selftest reads internals
 *   TC-DBG-011 has to build the "very narrow stack bounds ⇒ capture clipped to 0 frames" scenario, then
 *   must **restore the bounds verbatim** — when unset they are (0, 0). The public
 *   `br_bt_set_stack_bounds` rejects a 0 bottom / top<=bottom, so "restore" cannot be expressed through
 *   the public surface; and making "read bounds / write them back verbatim" public API would push the
 *   plugin's internal state (who set the stack range and to what) into the golden interface surface,
 *   which is exactly what we want to avoid.
 *
 * ## Boundary discipline (same lineage as framework/vfs-core's `src/vfs_internal.h`)
 *   Expose only the two the selftest really uses, and always **read/write the production state itself**
 *   (`s_stack_bottom`/`s_stack_top` in the production impl), keeping no second copy elsewhere —
 *   otherwise the case would modify a copy's bounds. The rest of the production code (`backtrace.c`)
 *   stays `static`.
 */
#ifndef BR_DEBUG_BR_BT_INTERNAL_H
#define BR_DEBUG_BR_BT_INTERNAL_H

#include <br/core/br_types.h>

/* Read current stack bounds (unset ⇒ *bottom = *top = 0). The case saves state before changing bounds. */
void br_bt_internal_read_stack_bounds(br_uintptr_t *bottom, br_uintptr_t *top);

/* Write stack bounds back verbatim (**no validation at all**; 0/0 = unset). The case restores state at
 * the end — it cannot use public `br_bt_set_stack_bounds`: that rejects bottom==0 / top<=bottom. */
void br_bt_internal_restore_stack_bounds(br_uintptr_t bottom, br_uintptr_t top);

#endif /* BR_DEBUG_BR_BT_INTERNAL_H */
