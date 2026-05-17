/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_logger.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_logger.h — 日志记录器端口（Port）
 *
 * @file    cc/ports/cc_logger.h
 * @brief   提供分级日志记录接口，是整个应用的基础设施组件。
 *
 * 日志系统是所有组件的横切关注点（Cross-cutting Concern）。
 * 它提供 6 个日志级别和格式化输出能力，在设计上采用不透明指针
 * 隐藏具体实现（文件输出 vs 控制台输出等）。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 日志实例在堆上分配，由 create/destroy 管理生命周期
 *   - 所有日志输出通过 cc_logger_log() 统一入口
 *   - cc_logger_set_level() 可运行时调整最低输出级别
 *   - 线程安全由实现保证（当前为单线程模型）
 *   - 日志格式： [LEVEL] [name] message
 *
 * ─── 日志级别 ─────────────────────────────────────────────────────────
 *
 *   TRACE < DEBUG < INFO < WARN < ERROR < FATAL
 *   设置某个级别后，低于该级别的日志被静默丢弃。
 *   例如设置为 INFO 时，TRACE 和 DEBUG 日志不会输出。
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h。
 */

#ifndef CC_LOGGER_PORT_H
#define CC_LOGGER_PORT_H

#include "cc/core/cc_result.h"

/**
 * cc_log_level_t — 日志级别枚举
 *
 * 定义了日志输出的 6 个严重级别，从最不重要到最严重排列。
 * 这些级别帮助在开发、调试和生产环境间平衡信息量和噪音。
 */
typedef enum cc_log_level {
    CC_LOG_TRACE, /**< 跟踪级别：最详细的调试信息，用于追踪代码执行路径。
                   *   仅在深入调试时启用，生产环境应关闭。 */
    CC_LOG_DEBUG, /**< 调试级别：开发时期的诊断信息，如变量值、状态变化。
                   *   生产环境建议关闭。 */
    CC_LOG_INFO,  /**< 信息级别：正常运行时的重要事件，如启动、配置加载、
                   *   会话创建等。生产环境的默认级别。 */
    CC_LOG_WARN,  /**< 警告级别：潜在问题但不影响当前运行，如配置缺失使用默认值、
                   *   即将达到资源上限等。值得关注但无需立即处理。 */
    CC_LOG_ERROR, /**< 错误级别：操作失败但不影响应用继续运行，如单个请求失败、
                   *   文件读取错误等。需要调查但不会导致应用退出。 */
    CC_LOG_FATAL  /**< 致命级别：无法恢复的错误，通常会导致应用退出。
                   *   如存储初始化失败、关键资源缺失等。 */
} cc_log_level_t;

/**
 * cc_logger_t — 日志记录器（不透明类型）
 *
 * 封装具体的日志输出实现（控制台、文件、syslog 等）。
 * 具体实现在 .c 文件中定义，对调用方透明。
 */
typedef struct cc_logger cc_logger_t;

/**
 * cc_logger_create — 创建日志记录器
 *
 * 在堆上分配日志实例。name 用于在日志输出中标识来源，
 * level 指定初始的最低输出级别。
 *
 * @param name        日志记录器名称（显示在日志输出中，不可为 NULL）
 * @param level       初始最低输出级别
 * @param out_logger  输出：指向新日志实例的指针（调用者负责 cc_logger_destroy）
 * @return            CC_OK 表示成功
 */
cc_result_t cc_logger_create(const char *name, cc_log_level_t level, cc_logger_t **out_logger);

/**
 * cc_logger_destroy — 销毁日志记录器
 *
 * 释放日志实例持有的所有资源（文件句柄等）。
 * 传入 NULL 是安全的（无操作）。
 *
 * @param logger  要销毁的日志实例
 */
void cc_logger_destroy(cc_logger_t *logger);

/**
 * cc_logger_log — 记录一条日志消息
 *
 * 按 printf 风格格式化消息并输出。如果消息的级别低于
 * 当前设定的 level，消息将被静默丢弃（无输出开销）。
 *
 * @param logger  日志实例（不可为 NULL）
 * @param level   消息级别：低于当前设定级别的消息会被丢弃
 * @param fmt     格式化字符串（printf 风格）
 * @param ...     变长参数列表
 */
void cc_logger_log(cc_logger_t *logger, cc_log_level_t level, const char *fmt, ...);

/**
 * cc_logger_set_level — 运行时调整日志级别
 *
 * 允许在运行时动态改变日志输出粒度，无需重启应用。
 * 例如排查问题时临时调低到 DEBUG，问题解决后恢复 INFO。
 *
 * @param logger  日志实例（不可为 NULL）
 * @param level   新的最低输出级别
 */
void cc_logger_set_level(cc_logger_t *logger, cc_log_level_t level);

#endif