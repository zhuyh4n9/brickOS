/*
 * brickOS prototype v0.1.0 — 日志契约
 *
 * 与设计文档的关系(设计在 tangramOS-Design 分支):
 *   设计侧的"日志"是 **Service 插件**(11-01: 日志/OTA/crypto...; trace 是观测设施, 5-01)。
 *   v0.1.0 还没有插件管理器, 所以这里是一个**core 内的最小打印设施**, 不是那个服务。
 *   等 M0/M2 落地后, 本文件的职责应收敛为:
 *     - trace 事件(`BR_TRACE_EVT`, 5-01 §1)的调用点保留
 *     - 文本行输出移交 `service/log` 与 platform 早期 console 的正式契约
 *   见 WORKAROUNDS.md 的 br-wa-boot-001。
 *
 * 格式化支持子集(不拉 libc 的 printf): %c %s %d %i %u %x %X %p %% 与长度修饰 l/ll/z,
 * 以及宽度与 '0' 填充(如 %08x / %6u)。未识别的转换符原样丢弃并打出其字符。
 */
#ifndef BR_CORE_TG_LOG_H
#define BR_CORE_TG_LOG_H

#include <br/core/br_types.h>

typedef enum {
    BR_LOG_DEBUG = 0,
    BR_LOG_INFO  = 1,
    BR_LOG_WARN  = 2,
    BR_LOG_ERROR = 3,
    BR_LOG_LEVEL_COUNT
} br_log_level_t;

/*
 * 记录日志起点时间戳。必须在 br_clock_init() 之后调用。
 * 调用前 br_log_write 仍然可用(时间戳从 0 起), 便于早期 init 链打印。
 */
void br_log_init(void);

/* 低于该等级的日志被丢弃。缺省 BR_LOG_INFO。 */
void br_log_set_level(br_log_level_t level);
br_log_level_t br_log_get_level(void);

/*
 * 归 core 所有的格式化输出入口。
 * `level` 既决定是否输出, 也决定行首标签; `fmt` 支持上文列出的子集。
 */
void br_log_write(br_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define br_log_debug(...) br_log_write(BR_LOG_DEBUG, __VA_ARGS__)
#define br_log_info(...)  br_log_write(BR_LOG_INFO,  __VA_ARGS__)
#define br_log_warn(...)  br_log_write(BR_LOG_WARN,  __VA_ARGS__)
#define br_log_error(...) br_log_write(BR_LOG_ERROR, __VA_ARGS__)

#endif /* BR_CORE_TG_LOG_H */
