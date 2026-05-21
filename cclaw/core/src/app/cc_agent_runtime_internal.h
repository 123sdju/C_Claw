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

/**
 * cc_agent_runtime — runtime 内部完整状态。
 *
 * 端口值为浅拷贝，指针依赖均为借用；config 中字符串由 runtime 深拷贝拥有。
 */
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
    cc_tool_executor_pool_t *tool_pool;
    cc_runtime_services_t services;
    int thinking_mode;
    cc_mutex_t mutex;
};

/**
 * cc_agent_runtime_execute_tool_step — 参与工具注册、工具调用或工具结果写回流程。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 * @param session_id 借用的只读字符串；函数不会释放该指针。
 * @param call 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param reasoning_content 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_agent_runtime_execute_tool_step(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *reasoning_content,
    cc_cancel_token_t *cancel_token
);

/**
 * cc_agent_runtime_store_assistant_text — 把最终 assistant 文本和可选 reasoning_content 包装成消息并追加到 session store。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 * @param session_id 借用的只读字符串；函数不会释放该指针。
 * @param text 借用的只读字符串；函数不会释放该指针。
 * @param reasoning_content 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_agent_runtime_store_assistant_text(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *text,
    const char *reasoning_content
);

#endif
