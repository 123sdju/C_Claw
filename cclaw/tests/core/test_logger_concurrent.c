/**
 * 学习导读：cclaw/tests/core/test_logger_concurrent.c
 *
 * 所属层次：测试层。
 * 阅读重点：这里用小型 Given/When/Then 场景固定行为，阅读时重点看每个断言防止哪类回归。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * test_logger_concurrent.c
 *
 * 测试目标：验证 Logger 模块在多线程并发写入时的线程安全性。
 *
 * 测试方法：
 * - 创建一个日志记录器，日志级别设为 CC_LOG_INFO。
 * - 启动 4 个线程，每个线程循环 LOOPS（200）次写入 INFO 级别日志。
 * - 日志内容包含线程序号 i，格式为 "thread log %d"。
 * - 所有线程完成后销毁 logger。
 *
 * 边界条件与验证点：
 * - 并发写入：多线程同时调用 cc_logger_log，验证内部缓冲区和 I/O 的线程安全性。
 * - 崩溃检测：此测试不验证日志内容的正确性（因日志可能输出到文件或 stderr 且乱序），
 *   主要验证并发日志操作不会导致程序崩溃、死锁或内存错误。
 * - 格式化安全性：日志消息使用可变参数格式化（printf 风格），
 *   验证并发格式化不会产生缓冲区溢出。
 * - 销毁安全：在所有线程结束后销毁 logger，验证 logger 的资源清理不依赖线程计数。
 *
 * 通过标准：所有线程正常结束，logger 成功销毁，程序返回 0（无崩溃即通过）。
 */

#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_thread.h"

#define THREADS 4
#define LOOPS 200

/* 日志线程上下文：持有共享的 logger 引用 */
typedef struct {
    cc_logger_t *logger;
} log_ctx_t;

/*
 * 工作线程函数
 * 循环 LOOPS 次向共享 logger 写入 INFO 级别日志。
 * 每条日志包含线程循环序号，用于区分不同线程的输出。
 */
static void *worker(void *arg)
{
    log_ctx_t *ctx = (log_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_logger_log(ctx->logger, CC_LOG_INFO, "thread log %d", i);
    }
    return NULL;
}

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时以非零退出码暴露问题。
 *
 * 位置：核心数据模型层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @return 0 通常表示成功完成，非 0 表示失败或应向进程层传播的状态。
 */
int main(void)
{
    cc_logger_t *logger = NULL;
    /* 创建名为 "test" 的日志记录器，级别为 INFO */
    if (cc_logger_create("test", CC_LOG_INFO, &logger).code != CC_OK) return 1;

    /* 启动并发日志写入 */
    log_ctx_t ctx = { logger };
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) cc_thread_create(worker, &ctx, &threads[i]);
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);

    /* 销毁 logger 并退出 - 不崩溃即视为测试通过 */
    cc_logger_destroy(logger);
    return 0;
}
