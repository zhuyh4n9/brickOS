/*
 * bdev-core plugin-private internal contract (not exported): the pool view needed by this plugin's
 * own src/bdev_selftest.c. Capacity is an implementation detail, so it is not public API.
 */
#ifndef BR_BDEV_CORE_INTERNAL_H
#define BR_BDEV_CORE_INTERNAL_H

#include <br/core/br_types.h>

/* Static capacity (BR_BDEV_MAX). TC-BDEV-007: occupied <= capacity, capacity >= 1. */
br_u32 bdev_internal_capacity(void);

/* Occupied slot count. TC-BDEV-007 cross-checks it against the registry's BR_CLASS_BDEV count. */
br_u32 bdev_internal_slot_count(void);

#endif /* BR_BDEV_CORE_INTERNAL_H */
