/* iface/min -- minimal core-native alias skin: stateless interface leaf, no core function; it only
 * re-exports declarations. Descriptor is generated in build/gen/iface/min/plugin_desc.c. */
#include <iface/min/min.h>

/* EARLY (no heap/threads, interrupts off): nothing to do. */
int iface_min_early_init(void)
{
    return 0;
}

/* LATE (interface second completion point): stateless. */
int iface_min_init(void)
{
    return 0;
}

/* START (interrupts enabled): nothing to do. */
int iface_min_start(void)
{
    return 0;
}
