

#ifndef CC_TOOL_EXECUTOR_POOL_H
#define CC_TOOL_EXECUTOR_POOL_H

#include "cc/app/cc_cancel_token.h"
#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 工具执行池不透明句柄；内部按 lane 管理并发计数和等待队列。 */
typedef struct cc_tool_executor_pool cc_tool_executor_pool_t;

/*
 * 单个工具 lane 的限流策略。
 *
 * name 是 lane 名称，concurrency 控制同时执行数量，timeout_ms 控制该 lane 默认工具超时。
 */
typedef struct cc_tool_executor_pool_policy {
    const char *name;
    int concurrency;
    int timeout_ms;
} cc_tool_executor_pool_policy_t;

/*
 * 工具执行池配置。
 *
 * default_* 用于未命名 lane，policies 数组只在 create 调用期间借用。该结构让高风险或
 * 慢工具可以被隔离，避免阻塞所有工具执行。
 */
typedef struct cc_tool_executor_pool_config {
    int default_concurrency;
    int default_timeout_ms;
    const cc_tool_executor_pool_policy_t *policies;
    size_t policy_count;
} cc_tool_executor_pool_config_t;

/* acquire 成功后返回的票据；release 必须带同一 ticket 归还 lane 容量。 */
typedef struct cc_tool_executor_pool_ticket {
    size_t lane_index;
} cc_tool_executor_pool_ticket_t;

/* 返回默认执行池配置。 */
cc_tool_executor_pool_config_t cc_tool_executor_pool_default_config(void);

/* 创建工具执行池；config 为 NULL 时使用默认配置。 */
cc_result_t cc_tool_executor_pool_create(
    const cc_tool_executor_pool_config_t *config,
    cc_tool_executor_pool_t **out_pool
);

/* 销毁执行池；调用前应保证没有未 release 的 ticket。 */
void cc_tool_executor_pool_destroy(cc_tool_executor_pool_t *pool);

/* 获取某个 lane 的执行许可；可能阻塞直到有空位。 */
cc_result_t cc_tool_executor_pool_acquire(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_tool_executor_pool_ticket_t *out_ticket
);


/* 带取消 token 获取执行许可；等待期间取消应返回 CC_ERR_CANCELLED。 */
cc_result_t cc_tool_executor_pool_acquire_with_cancel(
    cc_tool_executor_pool_t *pool,
    const char *lane_name,
    cc_cancel_token_t *cancel_token,
    cc_tool_executor_pool_ticket_t *out_ticket
);

/* 释放执行许可并唤醒等待者。 */
void cc_tool_executor_pool_release(
    cc_tool_executor_pool_t *pool,
    cc_tool_executor_pool_ticket_t ticket
);

/* 查询 lane 默认 timeout；未知 lane 返回 default_timeout_ms。 */
int cc_tool_executor_pool_timeout_ms(
    cc_tool_executor_pool_t *pool,
    const char *lane_name
);

#ifdef __cplusplus
}
#endif

#endif
