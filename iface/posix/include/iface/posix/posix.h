/*
 * iface/posix -- POSIX thin skin: re-exports the runtime/posix headers verbatim (brick.toml
 * form = "skin", reexport_of = ["runtime/posix#posix"]). It declares no POSIX symbol itself;
 * implementations live in runtime/posix. Consumers include only this header and declare
 * `[[dep]] iface/posix`.
 */
#ifndef BR_IFACE_POSIX_POSIX_H
#define BR_IFACE_POSIX_POSIX_H

/* Lifecycle hooks (referenced by the generated build/gen/iface/posix/plugin_desc.c). Symbol
 * prefix is overridden to `iface_posix_` in brick.toml: the short name `posix` is shared with
 * runtime/posix, so default derivation would collide on `posix_*`. */
int iface_posix_early_init(void);
int iface_posix_init(void);
int iface_posix_start(void);

/* Re-exported POSIX surface: every public header of runtime/posix. The skin follows the
 * provider's surface; trimming is a matter of selection, not of the skin. */
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
