/**
 * cc_agent_manager.h — 多 Agent 编排入口。
 *
 * 所属层次：核心 SDK。
 *
 * manager 不创建 LLM、store、plugin 进程，也不读取文件 watcher。它只保存
 * agent id 到 runtime 的映射，并把每次 handle_message 放进 run queue。
 * 这样 SDK 层可以支持多 agent/session 语义，而 app 层仍然可以自由决定
 * 如何加载 config、如何热重载 plugin、如何展示 CLI。
 */

#ifndef CC_AGENT_MANAGER_H
#define CC_AGENT_MANAGER_H

#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_run_queue.h"
#include "cc/core/cc_result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc_agent_manager cc_agent_manager_t;

typedef struct cc_agent_manager_options {
    cc_agent_runtime_t *default_runtime;
    cc_run_queue_t *queue;
    int owns_queue;
    const char *default_agent_id;
    cc_run_queue_action_t default_action;
} cc_agent_manager_options_t;

typedef struct cc_agent_manager_submit_options {
    cc_run_queue_action_t action;
    cc_run_queue_lane_t lane;
} cc_agent_manager_submit_options_t;

cc_result_t cc_agent_manager_create(
    const cc_agent_manager_options_t *options,
    cc_agent_manager_t **out_manager
);

void cc_agent_manager_destroy(cc_agent_manager_t *manager);

cc_result_t cc_agent_manager_add_agent(
    cc_agent_manager_t *manager,
    const char *agent_id,
    cc_agent_runtime_t *runtime
);

cc_result_t cc_agent_manager_handle_message(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    char **out_response
);

cc_result_t cc_agent_manager_handle_message_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    char **out_response
);

/**
 * 提交一个交互 turn。
 *
 * 当前 manager 把交互输入映射为 run queue 的 STEER action：同 session 的 pending
 * run 会被替换，running run 会收到 cancel token。需要 FOLLOWUP/COLLECT 的低层
 * 调用方可以直接使用 cc_run_queue_submit_with_token()。
 */
cc_result_t cc_agent_manager_submit(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    cc_run_id_t *out_run_id
);

cc_result_t cc_agent_manager_submit_with_options(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    const cc_agent_manager_submit_options_t *options,
    cc_run_id_t *out_run_id
);

cc_result_t cc_agent_manager_collect(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id,
    char **out_response
);

cc_result_t cc_agent_manager_set_current_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
);

cc_result_t cc_agent_manager_switch_agent(
    cc_agent_manager_t *manager,
    const char *agent_id
);

const char *cc_agent_manager_current_agent(cc_agent_manager_t *manager);

cc_result_t cc_agent_manager_interrupt(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
);

/**
 * 重置 session 的运行状态。
 *
 * reset 会先取消指定 agent/session 下 pending 或 running 的 run，再调用该
 * agent runtime 的 session store clear_session 清空历史消息和工具审计记录。
 * clear_session 属于存储端口能力；没有实现时返回 CC_ERR_NOT_SUPPORTED。
 */
cc_result_t cc_agent_manager_reset_session(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id
);

cc_result_t cc_agent_manager_make_session_key(
    const char *agent_id,
    const char *session_id,
    char **out_key
);

cc_result_t cc_agent_manager_list_agents(
    cc_agent_manager_t *manager,
    char ***out_ids,
    size_t *out_count
);

#ifdef __cplusplus
}
#endif

#endif
