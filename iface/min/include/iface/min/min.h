/*
 * iface/min — core native minimal alias skin (Interface; hand-written)
 *
 * Design source: `1-03` §1 (iface-min: minimal alias layer, straight to native), `1-01` §7.4
 * (skin classification), §7.3 (A-2: APPs depend only on Interface plugins).
 *
 * ★ This unit **adds no implementation and renames nothing**: it only re-exports the core native
 *   headers the APP needs, verbatim, giving "logging/clock" a dependable Interface identity too.
 *   Declarations have a single truth — the core headers.
 *
 * ★ Consumers (APPs) include only this header (`<iface/min/min.h>`) + declare `[[dep]] iface/min`;
 *   then APP sources no longer contain `#include <br/core/...>`, satisfying "A-2: APPs do not
 *   depend on interfaces below the iface layer". `brickie check`/`build` enforce this via the
 *   "APP directly includes a core header" check (see ADR-0020).
 */
#ifndef BR_IFACE_MIN_MIN_H
#define BR_IFACE_MIN_MIN_H

/* Lifecycle hooks (referenced by the generated build/gen/iface/min/plugin_desc.c). */
int iface_min_early_init(void);
int iface_min_init(void);
int iface_min_start(void);

/*
 * The direct core native surface (extended as needed, not a full core alias):
 *   - `br_log.h`     log entry point and the `br_log_info/warn/error/debug` level macros
 *   - `br_time.h`    monotonic clock (`br_clock_now`) and tick count (`br_clock_tick_count`)
 *   - `br_version.h` prototype/version identification
 * Types (`br_u32/br_u64/br_bool/br_time_t`) arrive via the transitive include of `br_time.h`.
 */
#include <br/core/br_log.h>
#include <br/core/br_time.h>
#include <br/core/br_version.h>

#endif /* BR_IFACE_MIN_MIN_H */
