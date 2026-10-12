/*
 * iface/posix — POSIX thin skin (Interface; hand-written)
 *
 * Design source: `1-03` §1 (iface-posix: thin skin, re-exports svc-posix), `1-01` §7.4 (skin),
 * §7.3 (APP goes only through Interface). Prototype-side implementation = `runtime/posix`
 * (unit `posix`).
 *
 * ★ The skin does **not declare its own POSIX**: two declarations = "two truths for one symbol".
 *   It only **re-exports** the `runtime/posix` headers verbatim — declaration-level re-export is
 *   expressed by brick.toml's `form = "skin"` + `reexport_of = ["runtime/posix#posix"]`
 *   (§3.5 invariant 3). All implementations live in runtime/posix (single truth): the skin has
 *   no function body.
 *
 * ★ Consumers (APPs) include only this header (`<iface/posix/posix.h>`) + declare `[[dep]]
 *   iface/posix`. Compile-time visibility holds by "declared dependency closure = include
 *   surface" (app → iface/posix → runtime/posix; ADR-0018): the APP sees POSIX declarations but
 *   has **no** direct declaration edge to runtime/posix.
 */
#ifndef BR_IFACE_POSIX_POSIX_H
#define BR_IFACE_POSIX_POSIX_H

/* Lifecycle hooks (referenced by the generated build/gen/iface/posix/plugin_desc.c).
 * ★ The symbol prefix is explicitly overridden to `iface_posix_` in brick.toml — this unit shares
 *   the short name (`posix`) with runtime/posix, and default derivation would collide on the same
 *   `posix_*` (duplicate symbols at link time). */
int iface_posix_early_init(void);
int iface_posix_init(void);
int iface_posix_start(void);

/*
 * The re-exported POSIX surface (all public headers provided by runtime/posix).
 * Why the whole surface and not just what the APP currently uses: the skin's shape should follow
 * the provider's surface, otherwise the "interface surface" drifts with one consumer. Trimming is
 * a matter of **selection** (not selecting this unit), not of the skin.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#endif /* BR_IFACE_POSIX_POSIX_H */
