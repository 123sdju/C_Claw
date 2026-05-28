

#include "cc/app/cc_context_builder.h"
#include "cc_agent_runtime_internal.h"
#include "cc/app/cc_memory_context.h"
#include "cc/core/cc_message.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_token_counter.h"
#include <stdlib.h>
#include <string.h>

#define MAX_LOAD_MESSAGES 500

/* context builder 内部使用的动态 message 数组。 */
typedef struct message_vec {
    cc_message_t *items;
    size_t count;
    size_t capacity;
} message_vec_t;


/* 清理 message_vec 中每条深拷贝消息和数组缓冲。 */
static void message_vec_cleanup(message_vec_t *vec)
{
    if (!vec) return;
    for (size_t i = 0; i < vec->count; i++) cc_message_cleanup(&vec->items[i]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

/* 向 message_vec 追加 message 深拷贝。 */
static cc_result_t message_vec_append_copy(message_vec_t *vec, const cc_message_t *message)
{
    if (!vec || !message) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null message vector append");
    }
    if (vec->count == vec->capacity) {
        size_t next_cap = vec->capacity ? vec->capacity * 2 : 8;
        cc_message_t *next = realloc(vec->items, next_cap * sizeof(cc_message_t));
        if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow message vector");
        memset(next + vec->capacity, 0, (next_cap - vec->capacity) * sizeof(cc_message_t));
        vec->items = next;
        vec->capacity = next_cap;
    }
    cc_result_t rc = cc_message_copy(message, &vec->items[vec->count]);
    if (rc.code != CC_OK) return rc;
    vec->count++;
    return cc_result_ok();
}


/*
 * 创建一条临时文本 message 并追加到 vector。
 *
 * 用于 system prompt、memory block 和 summary 这类构造出来的上下文消息。
 */
static cc_result_t message_vec_append_text(
    message_vec_t *vec,
    cc_message_role_t role,
    const char *text
)
{
    cc_message_t *msg = NULL;
    cc_result_t rc = cc_message_create_text(NULL, NULL, role, text ? text : "", NULL, &msg);
    if (rc.code != CC_OK) return rc;
    rc = message_vec_append_copy(vec, msg);
    cc_message_destroy(msg);
    return rc;
}


/* 释放 session store 加载出的 message 数组。 */
static void free_loaded_messages(cc_message_t *messages, size_t count)
{
    for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
}

/*
 * 估算一组消息的 token 数。
 *
 * 先序列化成 JSON，再用轻量 token 估算器计算；失败时返回 0，让调用方倾向于不压缩。
 */
static int messages_token_estimate(const cc_message_t *messages, size_t count)
{
    char *json = NULL;


    cc_result_t rc = cc_messages_to_json(messages, count, 1, &json);
    if (rc.code != CC_OK || !json) {
        cc_result_free(&rc);
        return 0;
    }
    int tokens = cc_token_estimate_json_messages(json);
    free(json);
    cc_result_free(&rc);
    return tokens;
}


/*
 * 把 message 转为摘要 prompt 中的人类可读文本。
 *
 * 文本内容通过 message summary 获取；tool_calls 追加 JSON，确保压缩历史时不丢失工具调用。
 */
static void append_message_plaintext(cc_string_builder_t *sb, const cc_message_t *msg)
{
    char *summary = NULL;
    if (cc_message_get_text_summary(msg, &summary).code == CC_OK && summary) {
        cc_string_builder_append(sb, summary);
    }
    free(summary);
    if (msg->tool_calls.count > 0) {
        char *tool_calls = NULL;
        if (cc_tool_call_list_to_json(&msg->tool_calls, &tool_calls).code == CC_OK && tool_calls) {
            cc_string_builder_append(sb, "\n");
            cc_string_builder_append(sb, tool_calls);
        }
        free(tool_calls);
    }
}

/*
 * 尝试用 LLM 压缩历史消息。
 *
 * 只压缩较早历史，保留近期消息原文。压缩失败时返回 0，调用方会回退到截断策略，这样
 * provider 错误不会阻塞主请求。
 */
static int try_compress_history(
    cc_agent_runtime_t *runtime,
    cc_message_t *messages,
    int start_idx,
    int end_idx,
    char **out_summary
)
{
    *out_summary = NULL;
    if (!runtime || !runtime->llm.vtable || !runtime->llm.vtable->chat) return 0;
    if (end_idx - start_idx <= 2) return 0;



    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) return 0;
    cc_string_builder_append(&sb,
        "Summarize the following conversation into a concise paragraph. "
        "Preserve key facts, decisions, tool results, file paths, artifact ids, "
        "and explicit user preferences. Output ONLY the summary paragraph.\n\n");
    for (int i = start_idx; i < end_idx; i++) {
        cc_string_builder_append(&sb, "[");
        cc_string_builder_append(&sb, cc_message_role_string(messages[i].role));
        cc_string_builder_append(&sb, "]: ");
        append_message_plaintext(&sb, &messages[i]);
        cc_string_builder_append(&sb, "\n");
    }
    char *prompt = cc_string_builder_take(&sb);
    if (!prompt) return 0;

    cc_message_t *prompt_msg = NULL;
    cc_result_t rc = cc_message_create_text(
        "summary_prompt", "summary", CC_ROLE_USER, prompt, NULL, &prompt_msg);
    free(prompt);
    if (rc.code != CC_OK) return 0;

    cc_llm_chat_request_t req;
    memset(&req, 0, sizeof(req));
    req.messages = prompt_msg;
    req.message_count = 1;
    req.model = runtime->config.model;
    req.max_tokens = runtime->config.summary_max_tokens;
    req.temperature = runtime->config.summary_temperature;
    req.stream = 0;



    cc_llm_response_t resp;
    cc_llm_response_init(&resp);
    rc = runtime->llm.vtable->chat(runtime->llm.self, &req, &resp);
    cc_message_destroy(prompt_msg);
    if (rc.code != CC_OK || !resp.has_text || !resp.text) {
        cc_result_free(&rc);
        cc_llm_response_free(&resp);
        return 0;
    }
    *out_summary = strdup(resp.text);
    cc_result_free(&rc);
    cc_llm_response_free(&resp);
    return *out_summary != NULL;
}


/*
 * 追加上下文头部消息。
 *
 * system_prompt 作为 system 消息放在最前；active memory 检索到的 block 也作为 system
 * 消息注入。memory 注入失败不会吞掉非 OK rc，保证真正的构造错误可返回给调用方。
 */
static cc_result_t append_headers(
    cc_agent_runtime_t *runtime,
    const char *system_prompt,
    message_vec_t *out
)
{
    cc_result_t rc = cc_result_ok();
    if (system_prompt && system_prompt[0]) {
        rc = message_vec_append_text(out, CC_ROLE_SYSTEM, system_prompt);
        if (rc.code != CC_OK) return rc;
    }
    if (runtime && runtime->memory_store) {
        char *mem_block = NULL;
        rc = cc_memory_context_inject(runtime->memory_store, system_prompt, &mem_block);
        if (rc.code == CC_OK && mem_block && mem_block[0]) {
            rc = message_vec_append_text(out, CC_ROLE_SYSTEM, mem_block);
        }
        free(mem_block);
        cc_result_free(&rc);
        if (rc.code != CC_OK) return rc;
    }
    return cc_result_ok();
}


/*
 * 构建一次 LLM 请求的消息数组。
 *
 * 流程：加载 session 历史、追加 system/memory 头、估算 token、超过阈值时尝试 LLM 摘要，
 * 摘要失败则按预算保留最近消息。返回数组由调用方逐项 cleanup 后 free。
 */
cc_result_t cc_context_builder_build_messages(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *system_prompt,
    cc_message_t **out_messages,
    size_t *out_count
)
{
    if (!runtime || !session_id || !out_messages || !out_count) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null context builder argument");
    }
    *out_messages = NULL;
    *out_count = 0;

    cc_message_t *history = NULL;
    size_t history_count = 0;
    cc_result_t rc = runtime->store.vtable->load_messages(
        runtime->store.self, session_id, MAX_LOAD_MESSAGES, &history, &history_count);
    if (rc.code != CC_OK) return rc;

    message_vec_t out;
    memset(&out, 0, sizeof(out));
    rc = append_headers(runtime, system_prompt, &out);
    if (rc.code != CC_OK) {
        free_loaded_messages(history, history_count);
        message_vec_cleanup(&out);
        return rc;
    }

    int keep_recent = runtime->config.context_keep_recent > 0 ?
        runtime->config.context_keep_recent : 20;
    int budget = runtime->config.context_window_tokens;
    double threshold = runtime->config.context_compress_threshold;
    int history_tokens = messages_token_estimate(history, history_count);

    int compressed = 0;


    if (budget > 0 && threshold > 0.0 &&
        history_tokens > (int)(budget * threshold) &&
        history_count > (size_t)(keep_recent + 2)) {
        int end_idx = (int)history_count - keep_recent;
        char *summary = NULL;
        if (try_compress_history(runtime, history, 0, end_idx, &summary) && summary) {
            rc = message_vec_append_text(&out, CC_ROLE_SYSTEM, summary);
            free(summary);
            if (rc.code != CC_OK) {
                free_loaded_messages(history, history_count);
                message_vec_cleanup(&out);
                return rc;
            }
            for (size_t i = (size_t)end_idx; i < history_count; i++) {
                rc = message_vec_append_copy(&out, &history[i]);
                if (rc.code != CC_OK) break;
            }
            compressed = 1;
        }
    }

    if (!compressed) {
        size_t start = 0;
        if (budget > 0 && history_tokens > budget && history_count > 0) {


            start = history_count - 1;
            while (start > 0 &&
                   messages_token_estimate(&history[start], history_count - start) < budget) {
                start--;
            }
            if (messages_token_estimate(&history[start], history_count - start) > budget && start + 1 < history_count) {
                start++;
            }
        }
        for (size_t i = start; i < history_count; i++) {
            rc = message_vec_append_copy(&out, &history[i]);
            if (rc.code != CC_OK) break;
        }
    }

    free_loaded_messages(history, history_count);
    if (rc.code != CC_OK) {
        message_vec_cleanup(&out);
        return rc;
    }
    *out_messages = out.items;
    *out_count = out.count;
    return cc_result_ok();
}
