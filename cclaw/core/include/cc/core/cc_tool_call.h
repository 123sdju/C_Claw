/**
 * 学习导读：cclaw/core/include/cc/core/cc_tool_call.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义工具调用与结果结构，重点看 JSON 参数、工具错误、
 *           tool_call_id 关联和释放规则。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_tool_call.h — 工具调用与 LLM 响应数据模型
 *
 * @file    cc/core/cc_tool_call.h
 * @brief   定义工具调用请求、工具返回结果、LLM 响应等核心数据结构。
 *
 * 本模块是 Agent 工具调用流程的数据骨架。当 LLM 决定调用工具时，
 * 它返回一个工具调用请求（cc_tool_call_t）；工具执行后产生工具结果
 * （cc_tool_result_t）；最终这些信息被封装在 LLM 响应（cc_llm_response_t）
 * 中传递给 Agent Runtime 进行决策。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   所有结构体通过专用的 create/destroy 函数管理生命周期，
 *   字符串字段在 create 时被内部拷贝。
 *
 * ─── 数据流 ───────────────────────────────────────────────────────────
 *
 *   LLM 响应 → cc_llm_response_t { text 或 tool_call }
 *            → 如果有 tool_call → cc_tool_call_t → 工具执行
 *                                                     ↓
 *   LLM 收到 → cc_tool_result_t ←───────────────── 工具返回
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   仅依赖 cc_result.h。不依赖其他 OC 模块。
 */

#ifndef CC_TOOL_CALL_H
#define CC_TOOL_CALL_H

#include "cc_result.h"

/**
 * cc_tool_call_t — 工具调用请求
 *
 * 当 LLM 在响应中决定调用工具时，会产出一个工具调用对象。
 * 该对象描述了要调用哪个工具以及传入什么参数。
 */
typedef struct cc_tool_call {
    char *id;             /**< 工具调用的唯一标识符（由 LLM 生成） */
    char *name;           /**< 要调用的工具名称，与 cc_tool_vtable.name() 对应 */
    char *arguments_json; /**< 调用参数，JSON 格式字符串。
                           *   其 schema 由工具注册表的 build_schema_json() 定义 */
} cc_tool_call_t;

/**
 * cc_tool_result_t — 工具调用的返回结果
 *
 * 工具执行完成后产生的结果对象。ok 字段区分成功/失败，
 * 成功时 content 包含工具输出，失败时 error 包含错误信息。
 */
typedef struct cc_tool_result {
    int ok;              /**< 工具执行是否成功：1 = 成功，0 = 失败 */
    char *content;       /**< 成功时的输出内容（纯文本），失败时为 NULL */
    char *error;         /**< 失败时的错误描述，成功时为 NULL */
    char *metadata_json; /**< 附加的元数据（JSON 格式），可包含执行时间、
                          *   资源使用量等统计信息。可为 NULL */
} cc_tool_result_t;

/**
 * cc_llm_response_t — LLM 的一次响应
 *
 * 封装 LLM 单次推理的完整响应。LLM 的每次回复要么是纯文本
 * （对话继续），要么是工具调用请求（需要执行工具后继续推理）。
 * finished 标志指示 LLM 是否认为当前任务已完成。
 */
typedef struct cc_llm_response {
    int has_text;              /**< 是否包含文本回复：1 = 有文本，0 = 无 */
    char *text;                /**< 文本回复内容（自然语言），has_text=0 时为 NULL */
    int has_tool_call;         /**< 是否包含工具调用：1 = 有工具调用，0 = 无 */
    cc_tool_call_t tool_call;  /**< 工具调用信息，has_tool_call=0 时字段为空 */
    int finished;              /**< LLM 是否认为任务已完成：1 = 终点，0 = 继续 */
    char *reasoning_content;   /**< LLM 的思维链内容（CoT），仅 thinking_mode 时有效。
                                *   通常不展示给用户，用于调试和审计。可为 NULL */
} cc_llm_response_t;

/**
 * cc_tool_call_create — 创建工具调用请求对象
 *
 * 在堆上分配并初始化 cc_tool_call_t。所有字符串参数被内部拷贝。
 *
 * @param id             工具调用唯一 ID（不可为 NULL）
 * @param name           工具名称（不可为 NULL）
 * @param arguments_json 调用参数 JSON（可为 NULL，表示无参数调用）
 * @param out_call       输出：指向新创建的工具调用对象的指针
 * @return               CC_OK 表示成功
 */
cc_result_t cc_tool_call_create(
    const char *id,
    const char *name,
    const char *arguments_json,
    cc_tool_call_t **out_call
);

/**
 * cc_tool_call_destroy — 销毁工具调用对象
 *
 * 释放 cc_tool_call_t 的所有动态内存。传入 NULL 是安全的。
 *
 * @param call  要销毁的工具调用对象指针
 */
void cc_tool_call_destroy(cc_tool_call_t *call);

/**
 * cc_tool_result_create — 创建工具结果对象
 *
 * 在堆上分配并初始化 cc_tool_result_t。所有字符串参数被内部拷贝。
 *
 * @param ok            执行是否成功（1=成功, 0=失败）
 * @param content       成功时的输出内容（可为 NULL）
 * @param error         失败时的错误描述（可为 NULL）
 * @param metadata_json 附加元数据 JSON（可为 NULL）
 * @param out_result    输出：指向新创建的工具结果对象的指针
 * @return              CC_OK 表示成功
 */
cc_result_t cc_tool_result_create(
    int ok,
    const char *content,
    const char *error,
    const char *metadata_json,
    cc_tool_result_t **out_result
);

/**
 * cc_tool_result_destroy — 销毁工具结果对象
 *
 * 释放 cc_tool_result_t 的所有动态内存。传入 NULL 是安全的。
 *
 * @param result  要销毁的工具结果对象指针
 */
void cc_tool_result_destroy(cc_tool_result_t *result);

/**
 * cc_llm_response_init — 初始化栈上的 LLM 响应结构体
 *
 * 将 cc_llm_response_t 的所有字段清零，使其处于安全的初始状态。
 * 使用栈上的 cc_llm_response_t 前必须调用此函数。
 * 用完后调用 cc_llm_response_free() 清理动态分配的内容。
 *
 * @param response  要初始化的响应结构体指针（不可为 NULL）
 * @return          CC_OK 表示成功
 */
cc_result_t cc_llm_response_init(cc_llm_response_t *response);

/**
 * cc_llm_response_free — 释放 LLM 响应中的动态内容
 *
 * 释放 text、tool_call、reasoning_content 等动态分配的字符串。
 * 注意：不释放 response 指针本身（它可能在栈上）。
 * 传入 NULL 是安全的。
 *
 * @param response  要清理的响应结构体指针
 */
void cc_llm_response_free(cc_llm_response_t *response);

#endif
