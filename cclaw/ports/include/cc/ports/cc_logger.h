



#ifndef CC_LOGGER_PORT_H
#define CC_LOGGER_PORT_H

#include "cc/core/cc_result.h"

/*
 * 日志级别。
 *
 * 从 TRACE 到 FATAL 递增，logger 实现可以按当前 level 过滤。日志 payload 进入 event
 * bus 或外部系统前应通过 redaction，避免泄漏 token/password。
 */
typedef enum cc_log_level {
    CC_LOG_TRACE,

    CC_LOG_DEBUG,

    CC_LOG_INFO,

    CC_LOG_WARN,

    CC_LOG_ERROR,

    CC_LOG_FATAL

} cc_log_level_t;

/* 不透明 logger 句柄；内部实现负责并发保护和输出目标。 */
typedef struct cc_logger cc_logger_t;

/* 创建 logger；name 会被实现复制或借用，取决于 adapter，但调用方只持有句柄。 */
cc_result_t cc_logger_create(const char *name, cc_log_level_t level, cc_logger_t **out_logger);

/* 销毁 logger；允许 NULL。 */
void cc_logger_destroy(cc_logger_t *logger);

/* printf 风格日志输出；实现应是线程安全的，并统一做敏感字段脱敏。 */
void cc_logger_log(cc_logger_t *logger, cc_log_level_t level, const char *fmt, ...);

/* 动态调整日志级别；常用于调试开关或测试降低噪声。 */
void cc_logger_set_level(cc_logger_t *logger, cc_log_level_t level);

#endif
