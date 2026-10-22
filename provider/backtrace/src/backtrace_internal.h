/*
 * provider/backtrace plugin-private contract, not exported and not under include/. Audience: this
 * plugin's own src/backtrace_selftest.c.
 *
 * TC-DBG-011 narrows the stack bounds, then restores them verbatim; unset means (0, 0).
 * The public br_bt_set_stack_bounds rejects a 0 bottom / top <= bottom, so "restore" cannot go
 * through it. Both entries read/write the production state itself, with no second copy.
 */
#ifndef BR_DEBUG_BR_BT_INTERNAL_H
#define BR_DEBUG_BR_BT_INTERNAL_H

#include <br/core/br_types.h>

/* Read current stack bounds (unset => *bottom = *top = 0); the case saves the prior state. */
void br_bt_internal_read_stack_bounds(br_uintptr_t *bottom, br_uintptr_t *top);

/* Write stack bounds back verbatim, no validation (0/0 = unset); restores the saved state. */
void br_bt_internal_restore_stack_bounds(br_uintptr_t bottom, br_uintptr_t top);

#endif /* BR_DEBUG_BR_BT_INTERNAL_H */
