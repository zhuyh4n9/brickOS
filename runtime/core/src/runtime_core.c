/* runtime/core — core-layer specification skin. Stateless: three empty lifecycle hooks and the
 * re-exported declaration surface in include/runtime/core/core.h. Descriptor is generated in
 * build/gen/runtime/core/plugin_desc.c. */
#include <runtime/core/core.h>

/* EARLY (no heap/threads, interrupts off): a skin has nothing to do. */
int runtime_core_early_init(void)
{
    return 0;
}

/* LATE: still stateless — the units re-exported here are declarations, not services. */
int runtime_core_init(void)
{
    return 0;
}

/* START (interrupts enabled): nothing to do. */
int runtime_core_start(void)
{
    return 0;
}
