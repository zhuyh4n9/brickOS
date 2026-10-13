/*
 * In-core logging contract: no plugin manager yet (WORKAROUND(br-wa-boot-001)), so this is not
 * the design's log service. Format subset without libc printf: %c %s %d %i %u %x %X %p %%
 * with l/ll/z, width and '0' padding (e.g. %08x). Unrecognized conversions are dropped and
 * their character printed.
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

/* Record the logging start timestamp; call after br_clock_init(). Before that,
 * output still works (timestamp 0) for early init chains. */
void br_log_init(void);

/* Logs below this level are dropped. Default BR_LOG_INFO. */
void br_log_set_level(br_log_level_t level);
br_log_level_t br_log_get_level(void);

/* Formatted output entry point. `level` selects whether to emit and the prefix tag;
 * `fmt` supports the subset above. */
void br_log_write(br_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define br_log_debug(...) br_log_write(BR_LOG_DEBUG, __VA_ARGS__)
#define br_log_info(...)  br_log_write(BR_LOG_INFO,  __VA_ARGS__)
#define br_log_warn(...)  br_log_write(BR_LOG_WARN,  __VA_ARGS__)
#define br_log_error(...) br_log_write(BR_LOG_ERROR, __VA_ARGS__)

#endif /* BR_CORE_BR_LOG_H */
