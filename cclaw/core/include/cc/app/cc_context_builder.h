/**
 * 学习导读：cclaw/core/include/cc/app/cc_context_builder.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里声明上下文构建入口，重点看 session history、memory、skills
 *           和 tool schema 如何进入 LLM messages。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_context_builder.h — LLM 消息上下文构建模块
 *
 * @file    cc/app/cc_context_builder.h
 * @brief   从会话历史、系统提示词和长期记忆中组装完整的 LLM messages 数组。
 *
 * 本模块是 Agent 循环中"构建上下文"步骤的核心实现。它将多个来源
 * 的信息组装成符合 OpenAI Chat Completions API 格式的 messages JSON 数组：
 *   1. 系统提示词（system_prompt）
 *   2. 长期记忆注入块（可选，来自 cc_memory_context_inject）
 *   3. 对话摘要（压缩后的历史，可选）
 *   4. 会话历史消息（从 store 加载，经过 token 预算裁剪）
 *
 * ─── 上下文压缩策略 ───────────────────────────────────────────────────
 *
 *   本模块实现了两层上下文管理：
 *
 *   第一层 — Token 预算动态截断：
 *     从存储加载最多 500 条历史消息，构建完整 messages JSON 后估算
 *     token 数。若超过 context_window_tokens 限制，从最旧的非 system
 *     消息开始丢弃，直到 token 数落入限制内。旧消息简单丢弃，无额外开销。
 *
 *   第二层 — LLM 摘要压缩（可选）：
 *     若启用（context_compress_threshold > 0），当消息 token 数超过
 *     context_window_tokens * threshold 时，对最旧的消息调用 LLM 生成
 *     摘要，以单条 system 消息替代摘要覆盖的旧消息。这样既压缩了占用，
 *     又保留了关键信息。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - cc_context_builder_build_messages() 是唯一对外接口
 *   - 输出为标准 JSON 格式的 messages 数组字符串
 *   - 调用者负责 free 输出字符串
 *
 * ─── 组装顺序 ─────────────────────────────────────────────────────────
 *
 *   ┌─────────────────────────────────────────┐
 *   │ [0] { role: "system", content: ... }    │ ← system_prompt
 *   │ [1] { role: "system", content: ... }    │ ← 长期记忆块（可选）
 *   │ [2] { role: "system", content: ... }    │ ← 对话摘要（压缩后，可选）
 *   │ [3] { role: "user",    content: ... }   │ ← 保留的原始历史消息
 *   │ [4] { role: "assistant", content: ... } │
 *   │ ...                                     │
 *   └─────────────────────────────────────────┘
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h 和 cc/app/cc_agent_runtime.h。
 */

#ifndef CC_CONTEXT_BUILDER_H
#define CC_CONTEXT_BUILDER_H

#include "cc/core/cc_result.h"
#include "cc/app/cc_agent_runtime.h"

/**
 * cc_context_builder_build_messages — 从 runtime 的 store/memory 中构建发送给 LLM 的 messages JSON。
 *
 * @param runtime 借用 runtime，用于访问 store、memory store 和上下文配置。
 * @param session_id 借用会话 ID。
 * @param system_prompt 借用系统提示词；会作为首条 system 消息。
 * @param out_messages_json 输出新分配 JSON 字符串；调用方负责 free。
 * @return CC_OK 表示构建成功；失败返回存储、JSON 或内存错误。
 */
cc_result_t cc_context_builder_build_messages(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *system_prompt,
    char **out_messages_json
);

#endif
