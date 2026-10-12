/*
 * brickOS prototype v0.2.0 - version identification
 *
 * Version policy (design doc 1-03 §4, on the brickOS-Design branch): the major version is a
 * capability generation and promises no schedule. The prototype version and the OS version
 * (v1.0) move on different axes - the prototype is the vehicle that validates the design.
 *
 * v0.2.0 differs from v0.1.0 only on the "capability generation" axis, hence 0.2.0 rather
 * than 1.0.0: the generation covers **the runtime half** (plugin manager + declaration-driven
 * build + scheduler/sync/POSIX/VFS/device frameworks + ADR-0011..0022). There is **no ABI
 * promise** here and no implication of a "stable interface" - even interface-unit freezing
 * exists only as a shadow in `iface/posix` (`brickie iface freeze`).
 */
#ifndef BR_CORE_BR_VERSION_H
#define BR_CORE_BR_VERSION_H

#define BR_PROTOTYPE_NAME     "brickOS-prototype"

#define BR_VERSION_MAJOR      0
#define BR_VERSION_MINOR      2
#define BR_VERSION_PATCH      0
#define BR_VERSION_STRING     "v0.2.0"

#endif /* BR_CORE_BR_VERSION_H */
