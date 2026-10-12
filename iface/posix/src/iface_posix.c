/*
 * iface/posix — POSIX thin skin (implementation; hand-written)
 *
 * This file holds only three empty hooks: an Interface plugin is a **strict leaf +
 * stateless** (design D18 / `1-01` §7.4); it implements no POSIX function — those live in
 * `runtime/posix` (unit `posix`). The descriptor instance is not here but in the generated
 * `build/gen/iface/posix/plugin_desc.c` (ADR-0005 §8.4).
 */
#include <iface/posix/posix.h>

/* EARLY phase (no heap/threads/interrupts disabled): no action. */
int iface_posix_early_init(void)
{
    return 0;
}

/* LATE phase (the interface's 2nd completion point): the skin is stateless, nothing to do.
 * ★ Here we do **not** "check whether runtime/posix was initialized" — that would require
 *   reading the provider's private state; the order is guaranteed by
 *   `[[dep]] kind = "init"` + topological order, not by runtime probing. */
int iface_posix_init(void)
{
    return 0;
}

/* START phase (after global interrupt enable): no action. */
int iface_posix_start(void)
{
    return 0;
}
