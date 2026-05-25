/**
 * cc_run_queue.h — Agent run queue 的核心并发调度器。
 *
 * 所属层次：核心 SDK。
 *
 * 这个模块不依赖 POSIX 进程、文件 watcher 或 gateway，因此可以留在 core 中并
 * 被 ESP profile 复用。它提供一个可移植 job queue：
 *   - 同一个 session key 默认串行，避免两次 turn 同时写同一段历史。
 *   - 不同 lane 可以按配置并发，例如 main、subagent、plugin、mcp。
 *   - steer/followup/collect/interrupt 在 SDK 层形成确定的 pending/running
 *     处理语义，POSIX/Windows app 不需要各自实现队列。
 */

#ifndef CC_RUN_QUEUE_H
#define CC_RUN_QUEUE_H

#include "cc/core/cc_result.h"
#include "cc/app/cc_cancel_token.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_run_queue cc_run_queue_t;
typedef unsigned long cc_run_id_t;

typedef enum cc_run_queue_lane {
    CC_RUN_QUEUE_LANE_MAIN = 0,
    CC_RUN_QUEUE_LANE_SUBAGENT = 1,
    CC_RUN_QUEUE_LANE_PLUGIN = 2,
    CC_RUN_QUEUE_LANE_MCP = 3
} cc_run_queue_lane_t;

typedef enum cc_run_queue_action {
    CC_RUN_QUEUE_ACTION_STEER = 0,
    CC_RUN_QUEUE_ACTION_FOLLOWUP = 1,
    CC_RUN_QUEUE_ACTION_COLLECT = 2,
    CC_RUN_QUEUE_ACTION_INTERRUPT = 3
} cc_run_queue_action_t;

typedef struct cc_run_queue_config {
    int main_concurrency;
    int subagent_concurrency;
    int plugin_concurrency;
    int mcp_concurrency;
    int per_session_concurrency;
    int max_pending_per_session;
} cc_run_queue_config_t;

typedef struct cc_run_queue_request {
    const char *session_key;
    cc_run_queue_lane_t lane;
    cc_run_queue_action_t action;
} cc_run_queue_request_t;

typedef cc_result_t (*cc_run_queue_task_fn)(void *user_data);
typedef cc_result_t (*cc_run_queue_task_with_token_fn)(
    void *user_data,
    cc_cancel_token_t *cancel_token
);

cc_run_queue_config_t cc_run_queue_default_config(void);

cc_result_t cc_run_queue_create(
    const cc_run_queue_config_t *config,
    cc_run_queue_t **out_queue
);

void cc_run_queue_destroy(cc_run_queue_t *queue);

/**
 * cc_run_queue_submit — 提交一个异步 job，由 queue 自己的 worker 线程执行。
 *
 * user_data 只被队列借用。调用方必须保证它在 collect 之前有效；更高层的
 * agent manager 会为消息 run 分配自有 task 对象，普通调用方也应遵循同样规则。
 */
cc_result_t cc_run_queue_submit(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
);

/**
 * cc_run_queue_submit_with_token — 提交可感知取消令牌的异步 job。
 */
cc_result_t cc_run_queue_submit_with_token(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_with_token_fn task,
    void *user_data,
    cc_run_id_t *out_run_id
);

/**
 * cc_run_queue_collect — 等待指定 run 完成，并取回 task 返回的 cc_result_t。
 *
 * 返回值本身就是 task 的结果；如果 run_id 不存在，则返回 CC_ERR_NOT_FOUND。
 * collect 会释放队列内部 job 节点，因此同一个 run_id 只能 collect 一次。
 */
cc_result_t cc_run_queue_collect(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
);

/**
 * cc_run_queue_run — submit + collect 的同步包装，保留给简单调用方和测试使用。
 */
cc_result_t cc_run_queue_run(
    cc_run_queue_t *queue,
    const cc_run_queue_request_t *request,
    cc_run_queue_task_fn task,
    void *user_data
);

/**
 * cc_run_queue_interrupt_session — 标记 session 发生一次 interrupt。
 *
 * 核心队列使用协作式取消：它会标记 session/run 的 cancel source，让正在执行的
 * task 通过 cc_cancel_token_t 观察取消。队列不会强杀线程或平台进程；plugin
 * worker、MCP transport、shell 等实现需要在自己的安全点释放 pipe/HTTP/进程资源。
 */
cc_result_t cc_run_queue_interrupt_session(
    cc_run_queue_t *queue,
    const char *session_key
);

cc_result_t cc_run_queue_interrupt_run(
    cc_run_queue_t *queue,
    cc_run_id_t run_id
);

size_t cc_run_queue_in_flight(cc_run_queue_t *queue);
size_t cc_run_queue_pending(cc_run_queue_t *queue);

#ifdef __cplusplus
}
#endif

#endif
