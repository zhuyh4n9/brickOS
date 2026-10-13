/* iface/posix -- POSIX thin skin: three empty hooks, stateless, implements no POSIX function
 * (those live in runtime/posix). Descriptor is generated in build/gen/iface/posix/plugin_desc.c. */
#include <iface/posix/posix.h>

/* EARLY (no heap/threads, interrupts disabled): no action. */
int iface_posix_early_init(void)
{
    return 0;
}

/* LATE (interface 2nd completion point): stateless. Provider order is guaranteed by the
 * `[[dep]] kind = "init"` topological order, not by probing runtime/posix state. */
int iface_posix_init(void)
{
    return 0;
}

/* START (after global interrupt enable): no action. */
int iface_posix_start(void)
{
    return 0;
}
