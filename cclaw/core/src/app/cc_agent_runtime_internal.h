/**
 * 学习导读：cclaw/core/src/app/cc_agent_runtime_internal.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#ifndef CC_AGENT_RUNTIME_INTERNAL_H
#define CC_AGENT_RUNTIME_INTERNAL_H

#include "cc/app/cc_agent_runtime.h"

struct cc_agent_runtime {
    cc_agent_runtime_config_t config;
    cc_llm_provider_t llm;
    cc_tool_registry_t *tool_registry;
    cc_session_store_t store;
    cc_policy_engine_t policy;
    cc_sandbox_t sandbox;
    cc_event_bus_t *event_bus;
    cc_logger_t *logger;
    cc_memory_store_t *memory_store;
    cc_runtime_services_t services;
    int thinking_mode;
    cc_mutex_t mutex;
};

cc_result_t cc_agent_runtime_execute_tool_step(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *reasoning_content
);

cc_result_t cc_agent_runtime_store_assistant_text(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *text,
    const char *reasoning_content
);

#endif
