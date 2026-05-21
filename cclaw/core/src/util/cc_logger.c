/**
 * 学习导读：cclaw/core/src/util/cc_logger.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里实现跨模块日志端口，重点看互斥锁保护、日志级别过滤、
 *           stderr 输出边界和可裁剪环境下的同步行为。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_logger.c — 日志系统
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Ports 层，是整个应用程序的诊断和调试中枢。
 *   Ports 层是连接 Core 层与 Platform 层的桥梁，提供横切关注点
 *   （如日志、加密、网络）的抽象接口。本模块为各业务模块提供
 *   统一的日志输出能力，所有模块通过 cc_logger_t 接口输出运行信息，
 *   无需关心底层输出目标；当前同步写入 stderr。
 *
 * 核心设计：
 *   - 日志级别过滤：支持 TRACE/DEBUG/INFO/WARN/ERROR/FATAL 六个级别，
 *     借鉴了 log4j/slf4j 的级别体系
 *   - 级别阈值：运行时可通过 cc_logger_set_level 动态调整最低输出级别，
 *     无需重新编译或重启程序
 *   - 结构化输出格式：[YYYY-MM-DD HH:MM:SS] [级别] [模块名] 消息体
 *     固定的格式便于日志解析工具（如 grep、awk）处理
 *   - 输出目标 stderr：将应用业务数据（stdout）与日志数据（stderr）分离，
 *     支持 shell 管道独立处理（如 `./app 2>app.log`）
 *   - 即时刷新：每次日志后调用 fflush(stderr)，确保崩溃前日志不丢失，
 *     对性能有轻微影响但对调试场景是可接受的权衡
 *   - 零外部依赖：不依赖第三方日志库（如 spdlog/zlog），仅使用标准 C 库，
 *     保持代码库的最小依赖原则
 *
 * 设计决策：
 *   - 每个模块可拥有独立的 cc_logger_t 实例（通过 cc_logger_create 创建），
 *     便于按模块设置不同日志级别：调试时只降低目标模块的级别，减少日志噪音
 *   - 使用 stderr 而非文件：简单可靠，不引入文件轮转、权限等复杂度；
 *     用户可通过 shell 重定向自行决定日志存储方式
 *   - 不支持日志格式自定义（如 JSON 格式）：保持实现简洁，
 *     如果需要结构化日志，可在上层做 JSON 包装
 *   - locatime 而非 gmtime：日志以本地时间显示，便于人类快速定位问题时间点
 *
 * 依赖：
 *   - cc/core/cc_result.h — 统一结果类型（cc_result_t / cc_result_ok / cc_result_error）
 *   - 标准 C 库 — stdio（fprintf/vfprintf/stderr）、stdlib（malloc/free/calloc）、
 *     string（strdup）、stdarg（va_list/va_start/va_end）、time（time/localtime/strftime）
 */

#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

/*
 * struct cc_logger — 日志器实例
 *
 * 字段：
 *   name  — 模块名称，用于日志输出中标识来源（如 "c-claw"、"agent"、"sandbox"）
 *   level — 当前最低输出级别阈值，低于此级别的日志消息将被静默丢弃
 *           线程安全：通过 mutex 保护读写，多个线程可安全地同时访问
 *   mutex — 互斥锁，保护 level 字段的读写一致性
 *           WHY 需要 mutex：在运行时可能通过 cc_logger_set_level 动态调整
 *           日志级别（如 CLI 用户切换调试模式），而此时另一个线程可能正在
 *           调用 cc_logger_log 检查 level 阈值。mutex 防止数据竞争和
 *           部分写入（torn write），虽然 int 在大多数平台天然原子，
 *           但 mutex 提供了明确的内存序保证（happens-before语义）。
 *
 * 线程安全性保证范围：
 *   互斥锁保护的是对 logger 内部元数据（level）的访问，而非日志内容的顺序。
 *   多条日志的输出顺序取决于 mutex 的获取顺序（先到先得），不保证
 *   与调用时间严格一致。如果需要精确的日志顺序，调用方应自行串行化调用。
 */
struct cc_logger {
    char *name;            /* 模块名称，通过 strdup 分配 */
    cc_log_level_t level;  /* 最低输出级别阈值，mutex 保护 */
    cc_mutex_t mutex;      /* 保护 level 字段读写的互斥锁 */
};

/*
 * cc_logger_create — 创建日志器实例
 *
 * 功能：分配并初始化一个新的 cc_logger_t 实例。
 *       如果 name 为 NULL，默认使用 "c-claw" 作为模块名。
 *
 * 参数：
 *   name       — 模块名称（可为 NULL）
 *   level      — 初始日志级别阈值
 *   out_logger — 输出参数，创建成功时指向新日志器实例
 *
 * 返回值：
 *   cc_result_ok() — 创建成功
 *   cc_result_error(CC_ERR_OUT_OF_MEMORY) — 内存分配失败
 *
 * 使用示例：
 *   cc_logger_t *log;
 *   cc_logger_create("agent", CC_LOG_DEBUG, &log);
 */
cc_result_t cc_logger_create(const char *name, cc_log_level_t level, cc_logger_t **out_logger)
{
    cc_logger_t *logger = calloc(1, sizeof(cc_logger_t));
    if (!logger) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create logger");

    logger->name = name ? strdup(name) : strdup("c-claw");
    logger->level = level;
    cc_result_t rc = cc_mutex_create(&logger->mutex);
    if (rc.code != CC_OK) {
        free(logger->name);
        free(logger);
        return rc;
    }
    *out_logger = logger;
    return cc_result_ok();
}

/*
 * cc_logger_destroy — 销毁日志器实例
 *
 * 功能：释放日志器实例及其内部资源（模块名称字符串）。
 *       如果 logger 为 NULL，则静默返回。
 *
 * 参数：
 *   logger — 待销毁的日志器实例（可为 NULL）
 *
 * 返回值：无
 */
void cc_logger_destroy(cc_logger_t *logger)
{
    if (!logger) return;
    cc_mutex_lock(logger->mutex);
    free(logger->name);
    cc_mutex_unlock(logger->mutex);
    cc_mutex_destroy(logger->mutex);
    free(logger);
}

/*
 * level_string — 将日志级别枚举转换为可读字符串（内部辅助函数）
 *
 * 功能：将 cc_log_level_t 枚举值映射为对应的字符串表示，
 *       用于日志格式化输出中的级别字段。
 *
 * 参数：
 *   level — 日志级别枚举值
 *
 * 返回值：
 *   返回级别对应的字符串（如 "INFO"、"ERROR" 等）。
 *   对于未知级别返回 "UNKNOWN"。
 */
static const char *level_string(cc_log_level_t level)
{
    switch (level) {
    case CC_LOG_TRACE: return "TRACE";
    case CC_LOG_DEBUG: return "DEBUG";
    case CC_LOG_INFO: return "INFO";
    case CC_LOG_WARN: return "WARN";
    case CC_LOG_ERROR: return "ERROR";
    case CC_LOG_FATAL: return "FATAL";
    default: return "UNKNOWN";
    }
}

/*
 * cc_logger_log — 输出一条日志消息
 *
 * 功能：根据给定的日志级别和格式化字符串输出日志。
 *       首先检查日志级别是否满足当前阈值（logger->level），
 *       低于阈值的消息将被静默丢弃。
 *       日志格式为：[YYYY-MM-DD HH:MM:SS] [LEVEL] [MODULE_NAME] message
 *       输出到 stderr 并立即刷新。
 *
 * 参数：
 *   logger — 日志器实例（若为 NULL，静默返回）
 *   level  — 本条日志的级别
 *   fmt    — printf 风格的格式化字符串
 *   ...    — 格式化参数
 *
 * 返回值：无
 *
 * 设计决策：
 *   - 输出到 stderr 而非 stdout：将日志流与程序主输出分离，
 *     便于通过 shell 重定向分别处理（如 `./app 2>app.log`）
 *   - 每次 fflush：确保在程序崩溃或异常退出时日志不丢失，
 *     对性能有轻微影响但对于调试场景来说是可接受的权衡
 *   - 时间戳使用 localtime：直接输出本地时间，便于人类阅读
 */
void cc_logger_log(cc_logger_t *logger, cc_log_level_t level, const char *fmt, ...)
{
    if (!logger) return;

    /*
     * 获取互斥锁——保护对 level 字段的读取以及整个日志输出操作的
     * 原子性。从检查 level 阈值到 fflush 完成，整个过程在锁内执行，
     * 防止多线程并发输出导致日志行交错。
     *
     * 为什么整个输出路径都在锁内：
     *   如果只在检查 level 时加锁、实际输出时不加锁，则可能出现：
     *     线程A: [2026-05-14 10:00:00] [INFO] [moduleA] 日志A的开头...
     *     线程B: [2026-05-14 10:00:00] [WARN] [moduleB] 日志B的完整行
     *     线程A: ...日志A的结尾
     *   这会导致日志交错不可读。全程加锁保证每条日志独立成行。
     *
     * 性能考虑：
     *   fprintf + fflush 在锁内执行，对于高频日志场景可能有性能影响。
     *   但这是调试/诊断工具，而非高性能数据管道；当前实现选择同步写入，
     *   让崩溃前的最后几条日志更容易落盘，也让多线程输出顺序更直观。
     */
    cc_mutex_lock(logger->mutex);

    /*
     * 日志级别过滤（Log Level Filtering）
     *
     * 这是日志系统的核心优化——低于阈值的日志消息被静默丢弃，
     * 避免不必要的字符串格式化和 I/O 开销。
     *
     * 过滤发生在格式化之前（而非之后），这是关键的性能优化：
     *   - 跳过 vsnprintf 格式化（CPU 开销）
     *   - 跳过 fprintf 系统调用（内核态切换开销）
     *   - 跳过 fflush 强制刷盘（磁盘 I/O 开销）
     *
     * 级别的语义（从低到高）：
     *   TRACE (0) — 细粒度的函数调用追踪，用于深度调试
     *   DEBUG (1) — 开发调试信息，生产环境通常关闭
     *   INFO  (2) — 常规运行信息，默认级别
     *   WARN  (3) — 警告信息，不影响运行但需要关注
     *   ERROR (4) — 错误信息，功能受损但系统仍在运行
     *   FATAL (5) — 致命错误，系统即将崩溃或退出
     *
     * 级别阈值的工作原理：
     *   如果 logger->level = CC_LOG_INFO (2)，则：
     *     TRACE(0) 和 DEBUG(1) → 丢弃（level < threshold）
     *     INFO(2), WARN(3), ERROR(4), FATAL(5) → 输出
     *
     * 为什么 filter 在 lock 内而非 lock 外：
     *   读取 logger->level 需要内存序保证，若在 lock 外读取，
     *   可能读到另一个线程正在写入的中间状态
     *   （虽然 int 通常原子，但 C 标准不保证）。
     */
    if (level < logger->level) {
        cc_mutex_unlock(logger->mutex);
        return;
    }

    /*
     * 时间戳格式化（Timestamp Formatting）
     *
     * 使用 localtime_r 而非 localtime 以保证线程安全：
     *   - localtime_r 将结果写入调用方提供的 struct tm 缓冲区（栈上）
     *   - localtime 返回指向内部静态缓冲区的指针，多线程不安全
     *
     * 时间格式：YYYY-MM-DD HH:MM:SS
     *   示例：2026-05-14 15:30:00
     *
     * 选择此格式的原因：
     *   - 人类可读，不需要额外工具解析
     *   - 自然排序：字符串按字典序排序 = 按时间顺序排序
     *     （因为年-月-日-时-分-秒 从大到小排列）
     *   - 兼容 ISO 8601 的日期部分，便于 grep/awk 按日期过滤
     *   - 不包含时区和毫秒：简化实现，对单机日志够用
     */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    /*
     * 结构化日志前缀
     *
     * 输出格式：[时间戳] [级别] [模块名] 消息体
     * 示例：   [2026-05-14 15:30:00] [INFO] [agent] Starting ReAct loop
     *
     * 方括号用于分隔各个字段，便于正则表达式解析：
     *   grep '\[ERROR\]' app.log          → 只看错误
     *   grep '\[agent\]' app.log          → 只看 agent 模块
     *   grep '2026-05-14.*\[WARN\]' app.log → 看某天的警告
     */
    fprintf(stderr, "[%s] [%s] [%s] ", time_buf, level_string(level), logger->name);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    /*
     * 输出到 stderr 并立即刷新
     *
     * 为什么输出到 stderr 而非 stdout：
     * ────────────────────────────────
     *   将应用业务数据（stdout）与诊断日志（stderr）分离：
     *     - ./c-claw           → stdout 显示 Agent 回复，stderr 显示日志
     *     - ./c-claw 2>/dev/null → 只看 Agent 回复，完全隐藏日志
     *     - ./c-claw 2>app.log  → 将日志重定向到文件，stdout 仍正常输出
     *   这是 Unix 管道哲学的标准实践。
     *
     * 为什么调用 fflush(stderr)：
     * ───────────────────────────
     *   stderr 默认是无缓冲的，但某些平台（如通过管道重定向时）可能
     *   变为全缓冲模式。显式 fflush 确保日志在程序崩溃时不会丢失——
     *   这是"容错日志"的关键：崩溃前最后的日志往往是诊断的线索。
     *
     * 性能权衡：
     *   每次 fflush 都触发 write 系统调用。对于高频日志（如 TRACE 级别
     *   的循环内日志），这是可观的性能开销。但日志系统的设计目标是
     *   "可靠性优先于性能"——丢失关键调试信息比微秒级延迟更严重。
     */
    fprintf(stderr, "\n");
    fflush(stderr);

    cc_mutex_unlock(logger->mutex);
}

/*
 * cc_logger_set_level — 动态调整日志级别阈值
 *
 * 功能：在运行时修改日志器的最低输出级别。
 *       例如，在调试时可将级别从 CC_LOG_INFO 降至 CC_LOG_DEBUG，
 *       以获取更详细的输出；在生产环境中可提高至 CC_LOG_WARN 以减少日志量。
 *
 * 参数：
 *   logger — 日志器实例（若为 NULL，静默返回）
 *   level  — 新的日志级别阈值
 *
 * 返回值：无
 *
 * 使用场景：
 *   - 程序启动时根据命令行参数或配置文件设置日志级别
 *   - 运行时通过信号（如 SIGUSR1）切换日志详细程度
 *   - 调试特定模块时临时降低该模块的日志级别阈值
 */
void cc_logger_set_level(cc_logger_t *logger, cc_log_level_t level)
{
    if (!logger) return;
    cc_mutex_lock(logger->mutex);
    logger->level = level;
    cc_mutex_unlock(logger->mutex);
}
