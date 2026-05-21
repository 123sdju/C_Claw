/**
 * cc_tool_executor_pool.h — 工具执行并发池。
 *
 * 所属层次：核心 SDK。
 *
 * 本模块只管理“能不能开始执行”和“本 lane 应使用什么 timeout”。真正的执行
 * 仍由 cc_tool_executor 或 plugin/MCP adapter 完成。这样可以把并发控制和
 * timeout 策略放在可移植 core 中，把进程、HTTP、stdio 等平台能力留给
 * app/adapters。cc_tool_executor 会把 timeout_ms 写入 cc_tool_context_t，
 * 具体工具再决定如何把它落到 pipe read、HTTP request 或本地操作上。
 */

#ifndef CC_TOOL_EXECUTOR_POOL_H
#define CC_TOOL_EXECUTOR_POOL_H

#include "cc/app/cc_cancel_token.h"
#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_tool_executor_pool cc_tool_executor_pool_t;

typedef struct cc_tool_executor_pool_policy {
    const char *name;
    int concurrency;
    int timeout_ms;
} cc_tool_executor_pool_policy_t;

typedef struct cc_tool_executor_pool_config {
    int default_concurrency;
    int default_timeout_ms;
    const cc_tool_executor_pool_policy_t *policies;
    size_t policy_count;
} cc_tool_executor_pool_config_t;

typedef struct cc_tool_executor_pool_ticket {
    size_t lane_index;
} cc_tool_executor_pool_ticket_t;

cc_tool_executor_pool_config_t cc_tool_executor_pool_default_config(void);

cc_result_t cc_tool_executor_pool_create(
    const cc_tool_executor_pool_config_t *config,
    cc_tool_executor_pool_t **out_pool
);

void cc_tool_executor_pool_destroy(cc_tool_executor_pool_t *pool);

cc_result_t cc_tool_executor_pool_acquire(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_tool_executor_pool_ticket_t *out_ticket
);

/**
 * cc_tool_executor_pool_acquire_with_cancel — 获取 lane 并发令牌，同时观察取消。
 *
 * 和 acquire 一样，成功后调用方必须 release；不同点是等待 lane 空位时会短
 * 周期醒来检查 cancel_token。取消是协作式的：本函数不会强行终止已经运行
 * 的工具，只让尚未开始执行的调用尽快退出等待。
 */
cc_result_t cc_tool_executor_pool_acquire_with_cancel(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_cancel_token_t *cancel_token,
    cc_tool_executor_pool_ticket_t *out_ticket
);

void cc_tool_executor_pool_release(
    cc_tool_executor_pool_t *pool,
    cc_tool_executor_pool_ticket_t ticket
);

int cc_tool_executor_pool_timeout_ms(
    cc_tool_executor_pool_t *pool,
    const char *lane_name
);

#ifdef __cplusplus
}
#endif

#endif
