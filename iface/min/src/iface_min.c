/*
 * iface/min - minimal core-native alias skin (implementation; hand-written)
 *
 * Three empty hooks: interface plugins are **strict leaves with no state** (design `1-01` §7.4).
 * It implements no core function - those live in core (single source of truth); this only
 * re-exports declarations. The descriptor instance is not here but in the generated
 * `build/gen/iface/min/plugin_desc.c` (ADR-0005 §8.4).
 */
#include <iface/min/min.h>

/* EARLY phase (no heap / no threads / interrupts off): nothing to do. */
int iface_min_early_init(void)
{
    return 0;
}

/* LATE phase (the interface second completion point): stateless, nothing to do. */
int iface_min_init(void)
{
    return 0;
}

/* START phase (after global interrupts are enabled): nothing to do. */
int iface_min_start(void)
{
    return 0;
}
