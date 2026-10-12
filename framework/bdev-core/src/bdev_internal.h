/*
 * brickOS prototype v0.2.0 — bdev-core **plugin-private** internal contract (not exported, not in include/)
 *
 * Audience: this plugin's own `src/bdev_selftest.c`. Living in `src/` (rather than `include/`) is
 * deliberate — everything under `include/` is **plugin-outward surface** (governed by `brickie check`'s
 * interface domain, and part of `descriptor_include` selection); the things here are needed **only by
 * the selftest**.
 *
 * ## Why the selftest reads internals
 *   One criterion's **subject is the internal bookkeeping**: TC-BDEV-007 asserts "occupied slots do not
 *   exceed the static bound" and "occupied slots match the number of BR_CLASS_BDEV entries in the
 *   registry". The public surface offers only `br_bdev_get` (fetch by name), which cannot answer "how
 *   many are used in the pool, what is the bound" — and that is exactly the observable premise of the
 *   structural guard "table full ⇒ -ENOSPC".
 *   Making it public API would be worse: capacity is an **implementation detail** (changing the bound
 *   should not be an interface change). So expose it only here.
 *
 * ## Boundary discipline (same lineage as cdev-core's `cdev_internal.h` and core's `*_internal.h`)
 *   Expose **only** the two things the selftest really uses, each with its purpose stated; and always
 *   **forward to** the same state in the production implementation, never copy constants (otherwise the
 *   case compares a copy and "the bound is that bound" is never verified).
 */
#ifndef BR_BDEV_CORE_INTERNAL_H
#define BR_BDEV_CORE_INTERNAL_H

#include <br/core/br_types.h>

/* This unit's static capacity (BR_BDEV_MAX; a compile-time constant in the production impl).
 * Use: TC-BDEV-007 — assert occupied <= capacity, and capacity >= 1. */
br_u32 bdev_internal_capacity(void);

/* Currently occupied slot count (static `s_bdev_n` in the production impl).
 * Use: TC-BDEV-007 — cross-check against the registry's BR_CLASS_BDEV entry count (bookkeeping does not drift). */
br_u32 bdev_internal_slot_count(void);

#endif /* BR_BDEV_CORE_INTERNAL_H */
