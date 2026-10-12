/*
 * brickOS prototype v0.2.0 — public surface of APP plugin `app/hello`
 *
 * Layout per `4-02` §2: a plugin's public header lives at `include/<short>/<short>.h`
 * (this plugin = `hello/hello.h`).
 * This file is the **declaration-surface anchor**: the generated
 * `build/gen/app/hello/plugin_desc.c` `#include`s it (generator rule: the header under the
 * plugin's `include/` that declares the most of that plugin's `[[export.entries]]`).
 *
 * Only **lifecycle hooks** are declared here — they are the entries the plugin manager
 * calls via the `.br_plugins` section + generated code (design 1-01 §9 / 3-05 §2). Names
 * derive from `symbol_prefix(short)`, they are not an optional API surface: hooks do
 * **not** go into `brick.toml`'s `[[export]]` (that would change the interface hash, and a
 * hook is a composition-time contract, not an outward plugin capability).
 *
 * Why not an "exported API": APP is a pure consumer (design §7.3: app depends only via
 * interface) and has nothing worth being called by other plugins — so this file has only
 * these three hooks, no `br_*`.
 */
#ifndef BR_APP_HELLO_HELLO_H
#define BR_APP_HELLO_HELLO_H

/* EARLY phase (no heap/no threads/interrupts off): APP does nothing, returns 0. */
int hello_early_init(void);

/* CORE phase (the class-determined ② completion point): APP setup (log level etc.). */
int hello_init(void);

/* START phase (after global interrupts on, the **last** start): selftest sequence + MainLoop. Does not return. */
int hello_start(void);

/*
 * Mount-plan application entry (ADR-0030). **Defined in the generated code**
 * (`build/gen/app/hello/plugin_desc.c`).
 *
 * ★ Why one function instead of letting APP walk that table itself: the mount-plan element
 *   type `br_mount_plan_t` is part of the core contract, and **APP may not include core
 *   headers** (ADR-0020: APP depends only on Interface). So the whole "table + walk + call
 *   POSIX mount()" goes into the generated code and APP calls this single entry — the plan
 *   is still **declarative** (this plugin's `brick.toml` `[[mount]]`), single source of
 *   truth unchanged.
 *
 * Returns 0 or -1 (errno set). APP should call it in the start phase, **before any file
 * operation**.
 */
int hello_mount_plan_apply(void);

#endif /* BR_APP_HELLO_HELLO_H */
