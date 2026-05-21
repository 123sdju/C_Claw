/**
 * 学习导读：cclaw/core/src/app/cc_context_builder.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_context_builder.c — LLM 消息上下文构建与压缩模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于 App 层（业务逻辑层），是 Agent 循环中"构建上下文"步骤的核心实现。
 * 负责将多个来源的信息组装成符合 OpenAI Chat Completions API 格式的
 * messages JSON 数组，同时实现两层上下文管理机制以防止超出 LLM 窗口限制。
 *
 * 上游调用方：
 *   - cc_agent_runtime.c —— 每次 LLM 调用前通过 cc_context_builder_build_messages()
 *     获取完整的 messages JSON 字符串
 *
 * 下游依赖模块：
 *   - cc_token_counter.c — token 数量估算，用于判断是否需要截断/压缩
 *   - cc_memory_context.c — 长期记忆注入，以 system message 形式追加
 *   - cc_json.c — JSON 序列化/反序列化
 *   - cc_string_builder.c — 文本拼接（用于构建压缩 prompt）
 *   - cc_llm_provider.c — LLM 调用（用于生成摘要压缩结果）
 *
 * 两层上下文管理策略：
 * ─────────────────────────────
 *
 *   第一层 — Token 预算动态截断：
 *     从存储加载最多 500 条历史消息，构建完整 messages JSON 后估算
 *     token 数。若超过 context_window_tokens 限制，从最旧的非 system
 *     消息开始丢弃，直到 token 数落入限制内。旧消息简单丢弃，无额外开销。
 *     这是兜底保护——确保无论如何都不会向 LLM 发送超出窗口的数据。
 *
 *   第二层 — LLM 摘要压缩（可选）：
 *     若启用（context_compress_threshold > 0），当消息 token 数超过
 *     context_window_tokens * threshold 时，对最旧的消息调用 LLM 生成
 *     摘要，以单条 system 消息替代摘要覆盖的旧消息。这样既压缩了占用，
 *     又保留了关键信息。
 *     这是主动优化——在触及硬截断之前就压缩旧对话，保留更多上下文。
 *
 * ─── 上下文构建流程 ───────────────────────────────────────────────────
 *
 *   1. 加载历史消息（最多 500 条，从存储层）
 *   2. 构建头部消息（system_prompt + 长期记忆注入）
 *   3. 估算头部 token 占用，计算有效预算 = 窗口大小 - 头部 - 预留
 *   4. 估算历史消息总 token 数
 *   5. 判断：总 token > 有效预算？
 *      ├─ 否 → 使用全部历史消息
 *      └─ 是 → 计算截断点，尝试压缩
 *   6. 尝试 LLM 摘要压缩（如果启用且超过阈值）
 *      ├─ 成功 → 插入摘要 system message，保留最近 N 条原始消息
 *      └─ 失败 → 回退到硬截断
 *   7. 将保留的消息序列化为 JSON messages 数组
 *   8. 返回结果
 *
 * ─── 设计决策 ─────────────────────────────────────────────────────────
 *
 *   为什么候选窗口默认取 500 条历史消息？
 *     500 是“可用于裁剪/压缩的候选上限”，不是最终发送给 LLM 的消息数。
 *     context builder 会先拿到足够的上下文素材，再由 token 预算、摘要压缩和
 *     最近消息保留策略共同决定真正进入请求的内容；这样可以避免过早丢掉
 *     可能对当前问题有价值的历史信息。
 *
 *   为什么压缩调用默认使用 temperature=0.3？
 *     摘要任务需要确定性和准确性，不需要创造力。低 temperature
 *     确保生成的摘要更加一致和可靠。
 *
 *   为什么压缩 prompt 要求 "Output ONLY the summary paragraph"？
 *     防止 LLM 在摘要中夹杂 "好的，让我总结一下..." 等废话，
 *     这些会白白浪费 token。
 *
 *   为什么保留最近 N 条原始消息不被压缩？
 *     最近对话是 Agent 推理的直接上下文，保留完整细节对连贯性
 *     至关重要。压缩只影响"旧对话"——那些用户大概率不会再追问
 *     细节的早期交互。
 *
 * ─── 消息内容编码规则（与 cc_agent_runtime 的约定）─────────────────
 *
 *   assistant 消息的 content 字段按以下方式编码：
 *   1. 包含 tool_calls 时 → JSON 包装:
 *      {"tool_calls":[...],"reasoning_content":"..."}
 *   2. 仅 reasoning_content 时 → JSON 包装:
 *      {"text":"回复内容","reasoning_content":"..."}
 *   3. 纯文本 → 直接存储字符串
 *
 *   本模块在构建 messages JSON 和提取纯文本摘要时，需要逆向
 *   理解这些编码规则，以确保正确序列化/反序列化。
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   标准 C 库：stdlib.h, string.h, stdio.h
 *   项目模块：cc_string_builder.h, cc_json.h, cc_token_counter.h,
 *             cc_memory_context.h, cc_agent_runtime.h, cc_result.h
 */

#include "cc/app/cc_context_builder.h"
#include "cc_agent_runtime_internal.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_token_counter.h"
#include "cc/app/cc_memory_context.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * MAX_LOAD_MESSAGES — 从存储加载的最大历史消息数
 *
 * 设计考量：
 *   加载更多消息意味着压缩模块有更多的"可选素材"——可以在更大的
 *   消息集合中筛选出最有价值的部分。
 *   500 条是一个保守的平衡点：足够覆盖数天的对话，又不会造成
 *   JSON 解析的明显延迟。
 *   实际发送给 LLM 的消息数由 token 预算控制，不受此值直接影响。
 */
#define MAX_LOAD_MESSAGES 500

/*
 * is_system_message — 判断消息是否为 system 角色
 *
 * 功能：
 *   检查消息的 role 字段是否为 CC_ROLE_SYSTEM。
 *   system 消息（如长期记忆注入块）在截断时应优先保留，
 *   不应被丢弃或压缩。
 *
 * 参数：
 *   msg — 消息指针
 *
 * 返回值：
 *   1 表示是 system 消息，0 表示不是
 */
static int is_system_message(const cc_message_t *msg)
{
    return msg->role == CC_ROLE_SYSTEM;
}

/*
 * token_budget — 计算压缩触发 token 阈值
 *
 * 功能：
 *   根据窗口大小和压缩阈值，计算触发 LLM 摘要压缩的 token 上限。
 *   公式：window_tokens * threshold
 *
 * 参数：
 *   window_tokens — 上下文窗口总 token 预算
 *   threshold     — 压缩触发阈值（0.0-1.0）
 *
 * 返回值：
 *   压缩触发的 token 阈值，未启用时返回 0
 *
 * 示例：
 *   window_tokens=8192, threshold=0.8 → budget=6553
 *   当消息 token > 6553 时触发压缩
 */
static int token_budget(int window_tokens, double threshold)
{
    if (window_tokens <= 0) return 0;
    if (threshold <= 0.0) return 0;
    return (int)(window_tokens * threshold);
}

/*
 * append_message_plaintext — 提取消息的纯文本内容并追加到 string builder
 *
 * 功能：
 *   从 cc_message_t 中提取人类可读的纯文本内容，追加到
 *   cc_string_builder_t 中。这是上下文压缩过程中的关键步骤——
 *   我们需要将历史消息转换为 LLM 可以理解的纯文本格式，以便
 *   生成摘要。
 *
 *   逆向理解 cc_agent_runtime 的消息编码规则：
 *   1. assistant + content 以 '{' 开头 → 可能是 reasoning_content 包装
 *   2. assistant + 有 tool_call_id → 可能是 tool_calls JSON 包装
 *   3. 其他情况 → content 本身就是纯文本
 *
 * 参数：
 *   sb  — 字符串构建器（输出）
 *   msg — 消息指针
 *
 * 设计决策：
 *   使用 JSON 解析而非字符串前缀匹配来提取内容，确保准确区分
 *   "恰好以 { 开头的纯文本" 和 "真正的 JSON 包装"。
 *   虽然解析 JSON 有一定开销，但压缩场景下这是必要的——
 *   我们宁愿多花一点 CPU 时间，也不愿把 JSON 包装当作文本发给 LLM。
 */
static void append_message_plaintext(cc_string_builder_t *sb, const cc_message_t *msg)
{
    if (msg->role == CC_ROLE_ASSISTANT && msg->tool_calls_json) {
        cc_string_builder_append(sb, msg->tool_calls_json);
        return;
    }
    if (!msg->content) return;

    /*
     * 分支 1: Assistant 的 reasoning_content 消息
     * content 格式: {"text":"实际回复","reasoning_content":"..."}
     * 我们只需要 text 字段（推理过程不需要出现在摘要中）
     */
    if (msg->role == CC_ROLE_ASSISTANT && msg->content[0] == '{') {
        cc_json_value_t *wrap = NULL;
        cc_result_t pr = cc_json_parse(msg->content, &wrap);
        if (pr.code == CC_OK && wrap) {
            cc_json_value_t *tv = cc_json_object_get(wrap, "text");
            if (tv) {
                const char *text = cc_json_string_value(tv);
                if (text) cc_string_builder_append(sb, text);
            }
            cc_json_destroy(wrap);
            return;
        }
        if (wrap) cc_json_destroy(wrap);
    }

    /*
     * 分支 2: Assistant 的 tool_call 消息
     * content 格式: {"tool_calls":[...],"reasoning_content":"..."}
     * 提取 tool_calls JSON 作为摘要内容（工具调用结果很重要）
     */
    if (msg->role == CC_ROLE_ASSISTANT && msg->tool_call_id && msg->tool_call_id[0] && msg->content) {
        cc_json_value_t *wrap = NULL;
        cc_result_t pr = cc_json_parse(msg->content, &wrap);
        if (pr.code == CC_OK && wrap) {
            cc_json_value_t *tcs = cc_json_object_get(wrap, "tool_calls");
            if (tcs) {
                char *tcs_str = cc_json_stringify_unformatted(tcs);
                if (tcs_str) {
                    cc_string_builder_append(sb, tcs_str);
                    free(tcs_str);
                }
            }
            cc_json_destroy(wrap);
            return;
        }
        if (wrap) cc_json_destroy(wrap);
    }

    /*
     * 分支 3: 普通消息（user / tool / plain assistant）
     * content 就是纯文本，直接使用
     */
    cc_string_builder_append(sb, msg->content);
}

/*
 * free_inline_messages — 释放内联消息数组及其所有字段
 *
 * 功能：
 *   释放 load_messages() 返回的消息数组。每个消息的 id、
 *   session_id、content、tool_call_id 都是独立分配的，
 *   需要逐个释放。
 *
 * 参数：
 *   messages — 消息数组指针
 *   count    — 消息数量
 *
 * 设计决策：
 *   消息数组和每条消息的字段都是独立分配的（由 cc_store_impl.c
 *   中的 load_messages 实现决定），所以这里需要双层释放。
 *   释放后 messages 指针本身不可再访问（它被 free 了）。
 */
static void free_inline_messages(cc_message_t *messages, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        cc_message_cleanup(&messages[i]);
    }
    free(messages);
}

/*
 * add_message_to_json_array — 将消息序列化为 JSON 并追加到数组
 *
 * 功能：
 *   将 cc_message_t 转换为符合 OpenAI Chat Completions API 格式的
 *   JSON 对象，并追加到 JSON 数组中。
 *
 *   关键规则：
 *   1. role 字段始终添加
 *   2. assistant + tool_call_id → content 设为 null，tool_calls 单独提取
 *   3. assistant + content 以 '{' 开头 → 提取 text 和 reasoning_content
 *   4. 其他消息 → content 直接使用
 *   5. tool 角色消息 → 添加 tool_call_id 字段
 *
 * 参数：
 *   arr — JSON 数组（输出）
 *   msg — 消息指针
 *
 * 设计决策：
 *   对 assistant 消息的 content 进行 JSON 解析而非直接传递，
 *   是为了确保输出的 messages JSON 符合 LLM API 的期望格式。
 *   例如 tool_calls 消息应该输出：
 *     {"role":"assistant","content":null,"tool_calls":[...]}
 *   而不是：
 *     {"role":"assistant","content":"{\"tool_calls\":[...]}"}
 *   前者是 OpenAI API 的标准格式，后者是双引号包裹的字符串，
 *   会导致 LLM 解析异常。
 */
static void add_message_to_json_array(
    cc_json_value_t *arr,
    const cc_message_t *msg,
    int include_reasoning_content)
{
    cc_json_value_t *jm = cc_json_create_object();
    const char *role_str = cc_message_role_string(msg->role);
    cc_json_object_set(jm, "role", cc_json_create_string(role_str));

    /*
     * 分支 1: Assistant 的 tool_call 消息
     * 需要输出: {"role":"assistant","content":null,"tool_calls":[...]}
     */
    if (msg->role == CC_ROLE_ASSISTANT && msg->tool_calls_json && msg->tool_calls_json[0]) {
        cc_json_object_set(jm, "content", cc_json_create_null());
        cc_json_value_t *dup = NULL;
        cc_json_parse(msg->tool_calls_json, &dup);
        if (dup) cc_json_object_set(jm, "tool_calls", dup);
        if (include_reasoning_content && msg->reasoning_content && msg->reasoning_content[0]) {
            cc_json_object_set(jm, "reasoning_content",
                cc_json_create_string(msg->reasoning_content));
        }
    }
    else if (msg->role == CC_ROLE_ASSISTANT && msg->tool_call_id && msg->tool_call_id[0]) {
        cc_json_object_set(jm, "content", cc_json_create_null());
        if (msg->content) {
            cc_json_value_t *wrapper = NULL;
            cc_json_parse(msg->content, &wrapper);
            if (wrapper) {
                cc_json_value_t *tcs = cc_json_object_get(wrapper, "tool_calls");
                if (tcs) {
                    char *tcs_str = cc_json_stringify(tcs);
                    cc_json_value_t *dup = NULL;
                    cc_json_parse(tcs_str, &dup);
                    free(tcs_str);
                    if (dup) cc_json_object_set(jm, "tool_calls", dup);
                }
                cc_json_value_t *rc = cc_json_object_get(wrapper, "reasoning_content");
                const char *rc_text = cc_json_string_value(rc);
                if (include_reasoning_content && rc_text && rc_text[0]) {
                    cc_json_object_set(jm, "reasoning_content",
                        cc_json_create_string(rc_text));
                }
                cc_json_destroy(wrapper);
            }
        }
    }
    /*
     * 分支 2: Assistant 的 reasoning_content 消息
     * 需要输出: {"role":"assistant","content":"实际文本","reasoning_content":"..."}
     */
    else if (msg->role == CC_ROLE_ASSISTANT && msg->reasoning_content) {
        cc_json_object_set(jm, "content",
            cc_json_create_string(msg->content ? msg->content : ""));
        if (include_reasoning_content && msg->reasoning_content[0]) {
            cc_json_object_set(jm, "reasoning_content",
                cc_json_create_string(msg->reasoning_content));
        }
    }
    else if (msg->role == CC_ROLE_ASSISTANT && msg->content && msg->content[0] == '{') {
        cc_json_value_t *wrap = NULL;
        cc_result_t pr = cc_json_parse(msg->content, &wrap);
        if (pr.code == CC_OK && wrap) {
            cc_json_value_t *tv = cc_json_object_get(wrap, "text");
            if (tv) {
                cc_json_object_set(jm, "content",
                    cc_json_create_string(cc_json_string_value(tv)));
            } else {
                cc_json_object_set(jm, "content",
                    cc_json_create_string(msg->content));
            }
            cc_json_value_t *rc = cc_json_object_get(wrap, "reasoning_content");
            const char *rc_text = cc_json_string_value(rc);
            if (include_reasoning_content && rc_text && rc_text[0]) {
                cc_json_object_set(jm, "reasoning_content",
                    cc_json_create_string(rc_text));
            }
            cc_json_destroy(wrap);
        } else {
            if (wrap) cc_json_destroy(wrap);
            cc_json_object_set(jm, "content",
                cc_json_create_string(msg->content));
        }
    }
    /*
     * 分支 3: 普通消息（user / tool / plain assistant）
     * 直接传递 content
     */
    else {
        if (msg->content) {
            cc_json_object_set(jm, "content",
                cc_json_create_string(msg->content));
        }
    }

    if (msg->tool_call_id && msg->role == CC_ROLE_TOOL) {
        cc_json_object_set(jm, "tool_call_id",
            cc_json_create_string(msg->tool_call_id));
    }

    cc_json_array_append(arr, jm);
}

/*
 * build_header_messages — 构建上下文头部消息（system + 记忆）
 *
 * 功能：
 *   组装 messages 数组的前半部分，包括：
 *   [0] system_prompt → {"role":"system","content":"..."}
 *   [1] 长期记忆注入  → {"role":"system","content":"..."} （可选）
 *
 *   这些消息始终保留在上下文顶部，不会被截断或压缩。
 *
 * 参数：
 *   system_prompt  — 系统提示词文本（可为 NULL）
 *   memory_store   — 长期记忆存储指针（可为 NULL）
 *
 * 返回值：
 *   JSON 数组，包含 system_prompt 和长期记忆消息
 *   调用方负责 cc_json_destroy
 *
 * 设计决策：
 *   使用 cc_memory_context_inject() 注入长期记忆，
 *   它会搜索与当前对话相关的记忆条目，格式化为文本块。
 *   注入失败是静默的——记忆是锦上添花，不是必须项。
 */
static cc_json_value_t* build_header_messages(
    const char *system_prompt,
    cc_memory_store_t *memory_store)
{
    cc_json_value_t *arr = cc_json_create_array();

    /*
     * 注入系统提示词（必须）
     * system_prompt 定义了 Agent 的核心身份、行为约束和工具使用策略
     */
    if (system_prompt && strlen(system_prompt) > 0) {
        cc_json_value_t *sys_msg = cc_json_create_object();
        cc_json_object_set(sys_msg, "role", cc_json_create_string("system"));
        cc_json_object_set(sys_msg, "content", cc_json_create_string(system_prompt));
        cc_json_array_append(arr, sys_msg);
    }

    /*
     * 注入长期记忆（可选）
     * 从 memory_store 中搜索相关记忆条目，格式化为 system message
     * 注入失败不影响整体流程——记忆是增强项，不是必需项
     */
    if (memory_store && memory_store->vtable) {
        char *mem_block = NULL;
        cc_result_t mem_rc = cc_memory_context_inject(
            memory_store, system_prompt, &mem_block);
        if (mem_rc.code == CC_OK && mem_block && strlen(mem_block) > 0) {
            cc_json_value_t *mem_msg = cc_json_create_object();
            cc_json_object_set(mem_msg, "role", cc_json_create_string("system"));
            cc_json_object_set(mem_msg, "content", cc_json_create_string(mem_block));
            cc_json_array_append(arr, mem_msg);
        }
        cc_result_free(&mem_rc);
        free(mem_block);
    }

    return arr;
}

/*
 * try_compress_history — 调用 LLM 对历史消息生成摘要
 *
 * 功能：
 *   将指定范围内的历史消息拼接成纯文本，发送给 LLM 请求生成摘要。
 *   摘要会保留关键事实、决策、工具调用结果和文件路径等重要信息，
 *   同时大幅减少 token 占用。
 *
 *   这是上下文压缩的核心步骤——通过"用 AI 总结 AI 的对话"来
 *   压缩上下文窗口。虽然这本身也需要一次 LLM 调用（消耗额外
 *   的 token），但相比不压缩导致截断大量消息，净效果是正向的。
 *
 * 参数：
 *   runtime      — Agent 运行时上下文（提供 LLM 调用能力）
 *   messages     — 完整的消息数组
 *   total_count  — 消息总数
 *   start_idx    — 要压缩的起始索引（包含）
 *   end_idx      — 要压缩的结束索引（不包含）
 *   out_summary  — 输出：摘要文本（调用方负责 free）
 *
 * 返回值：
 *   1 表示压缩成功（out_summary 有效）
 *   0 表示压缩失败（out_summary 为 NULL）
 *
 * 设计决策：
 *   1. summary_temperature 默认 0.3：摘要需要准确性和一致性，不需要创造力
 *   2. summary_max_tokens 默认 1024：限制摘要长度，防止 LLM 输出过长
 *   3. prompt 要求 "Output ONLY the summary paragraph"：
 *      防止 LLM 输出"好的让我总结一下..."等废话
 *   4. 消息数 < 3 时跳过压缩：太少消息不值得压缩开销
 *   5. LLM 调用失败时静默返回 0：压缩是优化项，不是必须项
 *
 * 压缩 prompt 设计：
 *   "Summarize the following conversation into a concise paragraph.
 *    Preserve all key facts, decisions, tool call results, file paths,
 *    and the user's explicit preferences.
 *    Output ONLY the summary paragraph, nothing else."
 *
 *   这个 prompt 经过精心设计：
 *   - concise paragraph → 要求段落格式（非列表），更节省 token
 *   - key facts, decisions → 保留重要决策和事实
 *   - tool call results → 保留工具执行结果（对 Agent 推理很重要）
 *   - file paths → 保留文件路径（后续操作可能需要）
 *   - user preferences → 保留用户偏好（影响后续交互风格）
 */
static int try_compress_history(
    cc_agent_runtime_t *runtime,
    cc_message_t *messages,
    int total_count,
    int start_idx,
    int end_idx,
    char **out_summary)
{
    *out_summary = NULL;

    if (!runtime->llm.vtable || !runtime->llm.vtable->chat || !runtime->llm.self) {
        return 0;
    }

    int msg_count = end_idx - start_idx;
    if (msg_count <= 2) return 0;

    /*
     * 步骤 1：将历史消息拼接为纯文本格式
     * 格式：[user]: 你好，帮我...
     *       [assistant]: 好的，...
     *       [tool]: 执行结果...
     */
    cc_string_builder_t sb;
    cc_string_builder_init(&sb);

    for (int i = start_idx; i < end_idx; i++) {
        cc_string_builder_append(&sb, "[");
        cc_string_builder_append(&sb, cc_message_role_string(messages[i].role));
        cc_string_builder_append(&sb, "]: ");
        append_message_plaintext(&sb, &messages[i]);
        cc_string_builder_append(&sb, "\n");
    }

    char *history_text = cc_string_builder_take(&sb);

    /*
     * 步骤 2：构建压缩 prompt
     * 将历史文本包装为单条 user 消息，附加摘要指令
     */
    cc_string_builder_t psb;
    cc_string_builder_init(&psb);
    cc_string_builder_append(&psb,
        "Summarize the following conversation into a concise paragraph. "
        "Preserve all key facts, decisions, tool call results, file paths, "
        "and the user's explicit preferences. "
        "Output ONLY the summary paragraph, nothing else.\n\n");
    cc_string_builder_append(&psb, history_text);
    char *prompt_text = cc_string_builder_take(&psb);

    cc_json_value_t *compress_arr = cc_json_create_array();
    cc_json_value_t *prompt_msg = cc_json_create_object();
    cc_json_object_set(prompt_msg, "role", cc_json_create_string("user"));
    cc_json_object_set(prompt_msg, "content", cc_json_create_string(prompt_text));
    cc_json_array_append(compress_arr, prompt_msg);
    char *compress_json = cc_json_stringify(compress_arr);
    cc_json_destroy(compress_arr);
    free(prompt_text);
    free(history_text);

    /*
     * 步骤 3：调用 LLM 生成摘要
     * 使用同步调用（非流式），因为摘要长度可控
     */
    cc_llm_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages_json = compress_json;
    req.model = runtime->config.model;
    req.max_tokens = runtime->config.summary_max_tokens;
    req.temperature = runtime->config.summary_temperature;
    req.stream = 0;

    cc_llm_response_t resp;
    memset(&resp, 0, sizeof(resp));
    cc_result_t rc = runtime->llm.vtable->chat(runtime->llm.self, &req, &resp);
    free(compress_json);

    if (rc.code != CC_OK || !resp.has_text || !resp.text) {
        cc_result_free(&rc);
        cc_llm_response_free(&resp);
        return 0;
    }

    *out_summary = strdup(resp.text);
    cc_result_free(&rc);
    cc_llm_response_free(&resp);
    return 1;
}

/**
 * cc_context_builder_build_messages — 构建完整的 LLM messages JSON
 *
 * 功能：
 *   这是本模块的唯一对外接口。将系统提示词、长期记忆、历史消息
 *   组装为符合 OpenAI Chat Completions API 格式的 messages JSON 数组。
 *
 *   实现两层上下文管理：
 *   第一层：Token 预算动态截断（兜底保护）
 *   第二层：LLM 摘要压缩（主动优化）
 *
 * 参数：
 *   runtime          — Agent 运行时上下文（提供配置和 LLM 调用能力）
 *   session_id       — 会话 ID（用于加载历史消息）
 *   system_prompt    — 系统提示词文本
 *   out_messages_json — 输出：messages JSON 数组字符串（调用方负责 free）
 *
 * 返回值：
 *   CC_OK 表示成功（out_messages_json 始终有效，即使没有历史消息）
 *
 * ─── 上下文构建流程详解 ──────────────────────────────────────────────
 *
 *   步骤 1：加载历史消息
 *     从存储加载最多 MAX_LOAD_MESSAGES (500) 条消息。
 *     选择 500 而非 100 是因为压缩模块需要更多的"可选素材"。
 *
 *   步骤 2：构建头部消息
 *     system_prompt + 长期记忆注入。
 *     估算头部 token 占用。
 *
 *   步骤 3：计算有效预算
 *     有效预算 = context_window_tokens - 头部 token - 预留 1024
 *     预留空间给 LLM 输出和其他开销。
 *
 *   步骤 4：估算历史消息总 token 数
 *     遍历所有消息，用 cc_token_estimate() 估算每条消息的 token 数。
 *
 *   步骤 5：判断是否需要截断/压缩
 *     如果 total_tokens > effective_budget + reserved：
 *       a. 计算截断点：保留最近 context_keep_recent 条消息
 *       b. 尝试 LLM 摘要压缩（如果启用）
 *       c. 压缩失败则回退到硬截断
 *
 *   步骤 6：序列化输出
 *     将头部消息 + 摘要（如有）+ 保留的历史消息序列化为 JSON。
 *
 * ─── 输出消息结构示例 ────────────────────────────────────────────────
 *
 *   未压缩时：
 *   [
 *     {"role": "system", "content": "你是..."},
 *     {"role": "system", "content": "[Memory]..."},
 *     {"role": "user", "content": "你好"},
 *     {"role": "assistant", "content": "你好！..."}
 *   ]
 *
 *   压缩后：
 *   [
 *     {"role": "system", "content": "你是..."},
 *     {"role": "system", "content": "[Memory]..."},
 *     {"role": "system", "content": "[Summary] 用户询问了...，助手..."},
 *     {"role": "user", "content": "最近的问题..."},
 *     {"role": "assistant", "content": "最近的回答..."}
 *   ]
 *
 * ─── 设计决策 ─────────────────────────────────────────────────────────
 *
 *   为什么预留 1024 token？
 *     这 1024 个 token 的预留空间用于：
 *     - JSON 结构字符（引号、逗号、花括号等）
 *     - LLM 内部处理开销（某些 tokenizer 对 JSON 结构的 token 分配
 *       可能比纯文本更多）
 *     - 安全缓冲（防止估算误差导致 API 报错）
 *
 *   为什么 system 消息不被压缩？
 *     system 消息（系统提示词、长期记忆）定义了 Agent 的核心身份
 *     和约束，丢失它们会导致 Agent 行为异常。压缩只针对"对话历史"。
 */
cc_result_t cc_context_builder_build_messages(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *system_prompt,
    char **out_messages_json
)
{
    /*
     * 读取配置参数
     * 这些值在 main.c 中从 config.json 加载并注入到 runtime config
     */
    int window_tokens = runtime->config.context_window_tokens;
    double compress_threshold = runtime->config.context_compress_threshold;
    int keep_recent = runtime->config.context_keep_recent;
    if (keep_recent <= 0) keep_recent = 20;

    /*
     * 步骤 1：从存储加载历史消息
     * 最多加载 MAX_LOAD_MESSAGES (500) 条
     * 加载失败不致命——返回空历史，由头部消息兜底
     */
    cc_message_t *messages = NULL;
    size_t count = 0;

    cc_result_t rc = runtime->store.vtable->load_messages(
        runtime->store.self, session_id, MAX_LOAD_MESSAGES, &messages, &count);

    /*
     * 步骤 2：构建头部消息（system_prompt + 长期记忆）
     * 估算头部 token 占用，以便计算可用于历史消息的预算
     */
    cc_json_value_t *header_arr = build_header_messages(system_prompt, runtime->memory_store);
    char *header_json = cc_json_stringify_unformatted(header_arr);
    int header_tokens = cc_token_estimate(header_json);
    free(header_json);

    int budget = token_budget(window_tokens, compress_threshold);
    int reserved = 1024;

    /*
     * 步骤 3：初始化输出 JSON 数组，先复制头部消息
     *
     * 为什么用 JSON 序列化/反序列化来"克隆"而非直接引用？
     *   因为 cc_json_array_append 会取得对象所有权，
     *   如果直接 append header_arr 中的对象，header_arr 就不再
     *   拥有它们，后续销毁 header_arr 会 double-free。
     *   序列化再反序列化是一种简单但安全的深拷贝方式。
     */
    cc_json_value_t *all_arr = cc_json_create_array();
    int arr_size = cc_json_array_size(header_arr);
    for (int i = 0; i < arr_size; i++) {
        cc_json_value_t *item = cc_json_array_get(header_arr, i);
        cc_json_value_t *clone = NULL;
        char *item_str = cc_json_stringify(item);
        cc_json_parse(item_str, &clone);
        free(item_str);
        if (clone) cc_json_array_append(all_arr, clone);
    }
    cc_json_destroy(header_arr);

    /*
     * 计算可用于历史消息的 token 预算
     * effective_budget = 总窗口 - 头部消息 - 预留空间
     * 如果头部消息本身就超过了窗口，effective_budget = 0
     */
    int effective_budget = window_tokens - header_tokens - reserved;
    if (effective_budget < 0) effective_budget = 0;

    /*
     * 步骤 4：处理历史消息
     *
     * 如果加载成功且有消息，检查是否需要截断/压缩。
     * 如果加载失败或没有消息，跳过此步骤，直接使用头部消息。
     */
    if (rc.code == CC_OK && messages && count > 0) {
        int use_count = (int)count;
        int use_start = 0;
        int compressed = 0;

        if (window_tokens > 0) {
            /*
             * 步骤 4a：估算历史消息总 token 数
             *
             * 每条消息的 token 估算 = cc_token_estimate(content) + 4
             * 其中 +4 是消息的 JSON 包装开销（role 字段、引号等）
             */
            int total_tokens = header_tokens;
            for (size_t i = 0; i < count; i++) {
                const char *ct = messages[i].content ? messages[i].content : "";
                total_tokens += cc_token_estimate(ct) + 4;
            }

            /*
             * 步骤 4b：判断是否需要截断/压缩
             *
             * 触发条件：total_tokens > effective_budget + reserved
             * 即：历史消息 + 头部 > 总窗口 - 预留空间 - 预留空间
             * 相当于：total_tokens > window_tokens
             */
            if (total_tokens > effective_budget + reserved) {
                /*
                 * 计算截断点：保留最近 keep_recent 条消息
                 *
                 * keep_from 是保留的消息起始索引
                 * 例如：100 条消息，keep_recent=20 → keep_from=80
                 * 保留索引 [80, 100) 的 20 条消息
                 */
                int keep_from = (int)count - keep_recent;
                if (keep_from < 0) keep_from = 0;

                int compress_end = keep_from;

                /*
                 * 精确计算截断点：从最新消息开始累积 token（倒序扫描），
                 * 找到刚好超出预算的位置。
                 *
                 * 注意：这里是从最新消息倒着往旧消息扫描，
                 * 所以 running 累积的是"从最新到当前"的 token 总数。
                 * 当 running > effective_budget 时，说明从最新到 i 的消息
                 * 已经超出预算，应该只保留 [i+1, count) 范围内的消息。
                 */
                int excess = total_tokens - effective_budget - reserved;
                if (excess > 0 && compress_end > 0) {
                    int running = 0;
                    int safe_from = (int)count;
                    for (int i = (int)count - 1; i >= 0; i--) {
                        const char *ct = messages[i].content ? messages[i].content : "";
                        running += cc_token_estimate(ct) + 4;
                        if (running > effective_budget) {
                            /*
                             * 从最新到 i 的消息已超出预算，
                             * 只保留 [i+1, count) 范围内的消息。
                             * 即使这少于 keep_recent 条，也必须截断——
                             * 兜底保护优先于连贯性。
                             */
                            safe_from = i + 1;
                            break;
                        }
                    }
                    /*
                     * 如果所有消息加起来都在预算内（极端情况：header 本身已占满），
                     * safe_from 保持为 count，即不发送任何历史消息。
                     */
                    keep_from = safe_from;
                    compress_end = safe_from;
                }

                /*
                 * 步骤 4c：尝试 LLM 摘要压缩（第二层管理）
                 *
                 * 触发条件：
                 *   1. 启用了压缩（compress_threshold > 0）
                 *   2. 有足够多的消息可压缩（compress_end >= 3）
                 *   3. 总 token 超过压缩预算
                 *
                 * 如果压缩成功，用摘要 system message 替代旧消息，
                 * 保留最近 keep_recent 条原始消息。
                 *
                 * 如果压缩失败（LLM 调用错误、无响应等），回退到硬截断。
                 */
                if (compress_threshold > 0.0 && compress_end >= 3) {
                    if (budget > 0 && total_tokens > budget) {
                        char *summary = NULL;
                        int ok = try_compress_history(runtime, messages, (int)count, 0, compress_end, &summary);
                        if (ok && summary && strlen(summary) > 0) {
                            /*
                             * 压缩成功：将摘要以 system message 形式插入
                             *
                             * 为什么用 system 角色而不是 user 或 assistant？
                             *   因为摘要是"对历史的总结"，属于上下文信息，
                             *   不是某个角色的发言。system 角色最合适，
                             *   而且 system 消息在截断时会被优先保留。
                             */
                            cc_json_value_t *sum_msg = cc_json_create_object();
                            cc_json_object_set(sum_msg, "role", cc_json_create_string("system"));
                            cc_json_object_set(sum_msg, "content", cc_json_create_string(summary));
                            cc_json_array_append(all_arr, sum_msg);
                            use_start = compress_end;
                            compressed = 1;
                        }
                        free(summary);
                    }
                }

                /*
                 * 步骤 4d：回退到硬截断
                 *
                 * 如果压缩未执行或执行失败，使用硬截断策略：
                 * 直接丢弃最旧的消息，保留最近的 keep_recent 条
                 */
                if (!compressed) {
                    use_start = keep_from;
                }

                use_count = (int)count - use_start;
                if (use_count < 0) use_count = 0;
            }
        }

        /*
         * 步骤 5：将保留的消息序列化到 JSON 数组
         *
         * 跳过 system 消息（它们已在头部消息中处理过，
         * 避免重复注入）。
         */
        for (int i = use_start; i < (int)count; i++) {
            if (is_system_message(&messages[i])) continue;
            add_message_to_json_array(all_arr, &messages[i],
                cc_agent_runtime_get_thinking_mode(runtime));
        }

        free_inline_messages(messages, count);
    } else {
        if (messages) free_inline_messages(messages, count);
    }

    /*
     * 步骤 6：序列化输出
     *
     * 最终的 messages JSON 数组包含：
     *   - 头部消息（system_prompt + 长期记忆）
     *   - 摘要消息（如果压缩成功）
     *   - 保留的历史消息
     */
    *out_messages_json = cc_json_stringify(all_arr);
    cc_json_destroy(all_arr);
    return cc_result_ok();
}
