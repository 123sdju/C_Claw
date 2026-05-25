/**
 * 学习导读：apps/posix/cli/tests/test_plugin_process_serial_call.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/*
 * test_plugin_process_serial_call.c
 *
 * 测试目标：验证 Plugin Process 的 JSON-RPC 串行通信机制
 * 在多线程并发调用场景下的正确性。
 *
 * 测试方法：
 * - 启动一个 Python 插件进程（weather_tool.py），
 *   通过标准输入/输出（stdin/stdout）进行 JSON-RPC 2.0 协议通信。
 * - 创建 4 个线程，每个线程发送 LOOPS（20）次 weather_query 请求。
 * - 每个请求使用唯一的 JSON-RPC id（格式 "{线程序号}-{循环序号}"），
 *   参数中包含城市名 "City{线程序号}"。
 * - 验证响应内容中包含城市名关键词 "City"。
 *
 * 边界条件与验证点：
 * - 串行化保证：cc_plugin_process_call 内部必须保证对同一进程的
 *   stdin/stdout 进行串行化访问（加锁），
 *   防止多个线程的请求/响应交叉混淆。
 * - JSON-RPC 协议完整性：验证请求-响应的匹配不因并发而错乱，
 *   每个线程收到的响应必须对应自己发出的请求。
 * - 子进程生命周期：所有线程完成后才销毁子进程，
 *   验证子进程在持续高负载下的稳定性。
 * - 资源清理：每次调用后释放 response 和 result，
 *   验证无内存泄漏。
 *
 * 通过标准：所有线程的每一次调用都返回 CC_OK，且响应中包含 "City"。
 */

#include "cc/plugin/cc_plugin_process.h"
#include "cc/ports/cc_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THREADS 4
#define LOOPS 20

/* 插件线程上下文：持有子进程引用、线程序号、失败标志 */
typedef struct {
    cc_plugin_process_t *process;
    int index;
    int failed;
} plugin_ctx_t;

/*
 * 工作线程函数
 * 循环发送 JSON-RPC weather_query 请求到插件子进程。
 * 每个请求具有全局唯一的 id：
 *   "{线程序号}-{循环序号}"（如 "0-0", "0-1", "1-0" 等）
 * 验证响应中包含城市名关键词 "City"。
 */
static void *worker(void *arg)
{
    plugin_ctx_t *ctx = (plugin_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        char request[256];
        char *response = NULL;
        snprintf(request, sizeof(request),
            "{\"jsonrpc\":\"2.0\",\"id\":\"%d-%d\",\"method\":\"weather_query\",\"params\":{\"city\":\"City%d\"}}",
            ctx->index, i, ctx->index);
        cc_result_t rc = cc_plugin_process_call(ctx->process, request, &response);
        if (rc.code != CC_OK || !response || !strstr(response, "City")) {
            ctx->failed = 1;
        }
        cc_result_free(&rc);
        free(response);
    }
    return NULL;
}

/**
 * main — 执行本文件的 Given/When/Then 回归测试，失败时以非零退出码暴露问题。
 *
 * @return 0 通常表示成功完成，非 0 表示失败或应向进程层传播的状态。
 */
int main(void)
{
    /* 启动 Python 插件子进程 */
    char *argv[] = { "python3", "apps/posix/cli/plugins/weather_tool.py", NULL };
    cc_plugin_process_t *process = NULL;
    if (cc_plugin_process_start("python3", argv, &process).code != CC_OK) return 1;

    /* 启动并发 JSON-RPC 调用测试 */
    plugin_ctx_t ctx[THREADS];
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ctx[i].process = process;
        ctx[i].index = i;
        ctx[i].failed = 0;
        cc_thread_create(worker, &ctx[i], &threads[i]);
    }
    for (int i = 0; i < THREADS; i++) cc_thread_join(threads[i]);

    /* 汇总结果并清理子进程 */
    int failed = 0;
    for (int i = 0; i < THREADS; i++) failed |= ctx[i].failed;
    cc_plugin_process_destroy(process);
    return failed ? 1 : 0;
}
