/*
 * brickOS prototype v0.1.0 — logging contract
 *
 * Relation to the design docs (design lives on the brickOS-Design branch):
 *   On the design side "logging" is a **Service plugin** (11-01: log/OTA/crypto...; trace is an
 *   observability facility, 5-01). v0.1.0 has no plugin manager yet, so this is a **minimal
 *   in-core printing facility**, not that service. After M0/M2 land, this file's responsibility
 *   should converge to:
 *     - keep the call sites for trace events (`BR_TRACE_EVT`, 5-01 §1)
 *     - hand text-line output to `service/log` and the platform early console contract
 *   See WORKAROUNDS.md, br-wa-boot-001.
 *
 * Format subset supported (no libc printf): %c %s %d %i %u %x %X %p %% with length modifiers
 * l/ll/z, plus width and '0' padding (e.g. %08x / %6u). Unrecognized conversions are dropped and
 * their character is printed.
 */
#ifndef BR_CORE_BR_LOG_H
#define BR_CORE_BR_LOG_H

#include <br/core/br_types.h>

typedef enum {
    BR_LOG_DEBUG = 0,
    BR_LOG_INFO  = 1,
    BR_LOG_WARN  = 2,
    BR_LOG_ERROR = 3,
    BR_LOG_LEVEL_COUNT
} br_log_level_t;

/*
 * Record the logging start timestamp. Must be called after br_clock_init().
 * br_log_write still works before that call (timestamp starts at 0), for early init chains.
 */
void br_log_init(void);

/* Logs below this level are dropped. Default BR_LOG_INFO. */
void br_log_set_level(br_log_level_t level);
br_log_level_t br_log_get_level(void);

/*
 * The core-owned formatted output entry point.
 * `level` decides both whether to emit and the line prefix tag; `fmt` supports the subset above.
 */
void br_log_write(br_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define br_log_debug(...) br_log_write(BR_LOG_DEBUG, __VA_ARGS__)
#define br_log_info(...)  br_log_write(BR_LOG_INFO,  __VA_ARGS__)
#define br_log_warn(...)  br_log_write(BR_LOG_WARN,  __VA_ARGS__)
#define br_log_error(...) br_log_write(BR_LOG_ERROR, __VA_ARGS__)

#endif /* BR_CORE_BR_LOG_H */
