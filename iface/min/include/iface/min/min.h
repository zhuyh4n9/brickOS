/*
 * iface/min -- minimal alias skin: re-exports the core native log/time/version headers the APP
 * needs, verbatim. Adds no implementation and renames nothing; declarations have a single truth
 * in core. Consumers include only this header and declare `[[dep]] iface/min`.
 */
#ifndef BR_IFACE_MIN_MIN_H
#define BR_IFACE_MIN_MIN_H

/* Lifecycle hooks (referenced by the generated build/gen/iface/min/plugin_desc.c). */
int iface_min_early_init(void);
int iface_min_init(void);
int iface_min_start(void);

/* Direct core native surface (extended as needed, not a full core alias). Types
 * (br_u32/br_u64/br_bool/br_time_t) arrive via the transitive include of br_time.h. */
#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_version.h>

#endif /* BR_IFACE_MIN_MIN_H */
