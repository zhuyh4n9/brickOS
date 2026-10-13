/*
 * Public surface of APP plugin `app/hello`: lifecycle hooks only, called by the plugin manager
 * via the `.br_plugins` section + generated code. Hooks stay out of `brick.toml`'s `[[export]]`
 * (a hook is a composition-time contract, not an outward plugin capability); APP is a pure
 * consumer and exports no `br_*` API.
 */
#ifndef BR_APP_HELLO_HELLO_H
#define BR_APP_HELLO_HELLO_H

/* EARLY phase (no heap/threads, interrupts off): APP does nothing, returns 0. */
int hello_early_init(void);

/* CORE phase: APP setup (log level etc.). */
int hello_init(void);

/* START phase (interrupts on, last start): selftests + MainLoop. Does not return. */
int hello_start(void);

/* Mount-plan application entry, defined in the generated code. APP may not include core headers,
 * so the mount table walk lives there. Returns 0 or -1 (errno set); call in the start phase,
 * before any file operation. */
int hello_mount_plan_apply(void);

#endif /* BR_APP_HELLO_HELLO_H */
