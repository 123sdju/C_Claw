/**
 * 学习导读：cclaw/core/include/cc/core/cc_message.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义会话消息结构，重点看 role、tool_calls_json、reasoning_content
 *           和 tool_call_id 的深拷贝规则。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_message.h — 消息数据模型模块
 *
 * @file    cc/core/cc_message.h
 * @brief   定义 Agent 对话中的消息实体及其角色枚举。
 *
 * 一条消息是构成会话（cc_session_t）的最小单元。每次用户输入、
 * LLM 回复、工具调用结果都会被建模为一个 cc_message_t 实例。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 * 本模块是纯数据结构定义 + 构造/析构函数，不包含业务逻辑。
 * 消息本身不感知 LLM 或工具，仅存储角色和内容文本。
 *
 * ─── 角色模型 ─────────────────────────────────────────────────────────
 *
 * 遵循 OpenAI Chat Completions API 的角色模型：
 *   - CC_ROLE_SYSTEM    : 系统提示词，定义 Agent 行为
 *   - CC_ROLE_USER      : 用户输入
 *   - CC_ROLE_ASSISTANT : LLM 生成的回复
 *   - CC_ROLE_TOOL      : 工具调用的返回结果
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc_result.h 作为错误传递机制。
 *   不依赖任何外部库（纯标准 C 结构体）。
 */

#ifndef CC_MESSAGE_H
#define CC_MESSAGE_H

#include "cc_result.h"

/**
 * cc_message_role_t — 消息角色枚举
 *
 * 定义消息的发送者角色，遵循 OpenAI Chat API 规范。
 * 不同角色在构建 LLM 上下文时有不同的含义和序列化方式。
 */
typedef enum cc_message_role {
    CC_ROLE_SYSTEM,    /**< 系统提示词，设定 Agent 的行为、风格和约束 */
    CC_ROLE_USER,      /**< 用户输入，用户向 Agent 提出的自然语言请求 */
    CC_ROLE_ASSISTANT, /**< 助手回复，LLM 生成的文本或工具调用指令 */
    CC_ROLE_TOOL       /**< 工具返回，工具执行后将结果传回 LLM */
} cc_message_role_t;

/**
 * cc_message_t — 对话消息结构体
 *
 * 表示 Agent 对话流中的一条消息，可以是用户输入、LLM 回复、
 * 或工具调用的返回结果。消息按时间顺序组成会话历史。
 */
typedef struct cc_message {
    char *id;              /**< 消息的唯一标识符（调用方生成的唯一字符串），用于索引和去重 */
    char *session_id;      /**< 所属会话的 ID，关联到 cc_session_t */
    cc_message_role_t role; /**< 消息的发送者角色（system/user/assistant/tool） */
    char *content;         /**< 消息的文本内容，角色不同含义不同：
                            *   - user/assistant: 自然语言文本
                            *   - tool: 工具执行的输出结果
                            *   - system: 系统提示词 */
    char *content_parts_json; /**< 多模态消息内容数组 JSON。非 NULL 时优先用于构建
                               *   provider-neutral content parts，content 保留为文本摘要。 */
    char *tool_calls_json; /**< 结构化 assistant tool_calls 数组 JSON。
                            *   仅 CC_ROLE_ASSISTANT 需要调用工具时使用。
                            *   tool_calls 独立保存，避免和自然语言 content 混在一起。 */
    char *reasoning_content; /**< LLM 推理/思考内容，独立于用户可见 content 存储。 */
    char *tool_call_id;    /**< 关联的工具调用 ID。仅 CC_ROLE_TOOL 角色有效，
                            *   用于将工具返回结果与对应的 tool_call 请求关联 */
    char *created_at;      /**< 消息创建时间戳（ISO 8601 格式字符串） */
} cc_message_t;

/**
 * cc_message_create — 创建一条新消息
 *
 * 在堆上分配并初始化 cc_message_t。所有字符串参数会被内部拷贝，
 * 调用者仍保有原字符串的所有权。
 *
 * @param id           消息唯一标识符（可为 NULL，但调用方应传入唯一字符串）
 * @param session_id   所属会话 ID（不可为 NULL）
 * @param role         消息角色（system/user/assistant/tool）
 * @param content      消息文本内容（可为 NULL）
 * @param tool_call_id 关联的工具调用 ID（可为 NULL，仅 TOOL 角色使用）
 * @param out_message  输出：指向新创建消息的指针（调用者负责 cc_message_destroy）
 * @return             CC_OK 表示成功，否则包含错误码
 */
cc_result_t cc_message_create(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const char *content,
    const char *tool_call_id,
    cc_message_t **out_message
);

/**
 * cc_message_destroy — 销毁消息并释放所有关联内存
 *
 * 释放 message 结构体及其所有字符串字段的内存。
 * 传入 NULL 是安全的（无操作）。
 *
 * @param message  要销毁的消息指针
 */
void cc_message_destroy(cc_message_t *message);

/**
 * cc_message_cleanup — 释放栈上或数组内消息持有的字段
 *
 * 只释放字段，不释放 message 指针本身。用于 store->load_messages 返回的
 * 连续数组元素。
 */
void cc_message_cleanup(cc_message_t *message);

/**
 * cc_message_copy — 深拷贝消息字段
 */
cc_result_t cc_message_copy(const cc_message_t *src, cc_message_t *dst);

/**
 * cc_message_set_tool_calls_json — 替换 assistant 消息中的 tool_calls JSON 副本。
 *
 * @param message 要更新的消息；函数释放旧 tool_calls_json。
 * @param tool_calls_json 借用的新 JSON 文本；函数会深拷贝。
 * @return CC_OK 表示更新成功；失败返回参数或内存错误。
 */
cc_result_t cc_message_set_tool_calls_json(cc_message_t *message, const char *tool_calls_json);

/**
 * cc_message_set_content_parts_json — 替换消息中的多模态 content parts JSON 副本。
 *
 * @param message 要更新的消息；函数释放旧 content_parts_json。
 * @param content_parts_json 借用的新 JSON 文本；函数会深拷贝。
 * @return CC_OK 表示更新成功；失败返回参数或内存错误。
 */
cc_result_t cc_message_set_content_parts_json(cc_message_t *message, const char *content_parts_json);

/**
 * cc_message_set_reasoning_content — 替换消息中的 reasoning_content 副本。
 *
 * @param message 要更新的消息；函数释放旧 reasoning_content。
 * @param reasoning_content 借用的新推理内容；函数会深拷贝。
 * @return CC_OK 表示更新成功；失败返回参数或内存错误。
 */
cc_result_t cc_message_set_reasoning_content(cc_message_t *message, const char *reasoning_content);

/**
 * cc_message_role_string — 将角色枚举转换为字符串标识
 *
 * 用于 JSON 序列化、日志输出等场景。
 * 映射关系：CC_ROLE_SYSTEM→"system", CC_ROLE_USER→"user",
 *          CC_ROLE_ASSISTANT→"assistant", CC_ROLE_TOOL→"tool"
 *
 * @param role  消息角色枚举值
 * @return      对应的角色字符串（静态常量，不需要释放）
 */
const char *cc_message_role_string(cc_message_role_t role);

#endif
