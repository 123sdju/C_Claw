/**
 * 学习导读：cclaw/adapters/src/llm/cc_anthropic_provider.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_anthropic_provider.c — Anthropic Messages API 协议策略模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于适配器层，是 LLM provider 两层策略中的**协议层**。只实现
 * cc_llm_protocol_t vtable 的四个回调，负责 Anthropic Messages API 特有的
 * JSON 格式转换。这是四个协议实现中**格式转换最复杂**的模块，因为 Anthropic
 * API 的消息结构、工具定义格式与 OpenAI 原生格式差异最大。
 *
 * 本模块不持有任何私有状态（self 始终为 NULL），因此不需要 destroy 回调。
 *
 * 上游调用方：
 *   - cc_http_llm_provider.c — 通过 cc_llm_protocol_t vtable 委托调用
 *     build_request / parse_response / parse_stream_event
 *
 * 下游依赖模块：
 *   - cc_json.c — JSON 对象构造与解析
 *   - cc_string_builder.c — URL 拼接、system prompt 拼接、tool result 格式化
 *   - cc_http_llm_provider.h — cc_llm_http_request_t 结构体
 *
 * ─── 消息格式转换（OpenAI 格式 → Anthropic 格式）──────────────────────
 *
 *   Anthropic Messages API 与 OpenAI Chat Completions API 的消息结构不同：
 *
 *   1. System 消息提升为顶层字段：
 *      OpenAI 的 system role 消息在 messages 数组中；Anthropic 要求 system
 *      是请求体的顶层字符串字段。本模块遍历所有 system role 消息，将 content
 *      用换行符拼接，设为 body.system；这些消息不再出现在 messages 数组中。
 *
 *   2. Tool role 消息转为 User role：
 *      OpenAI 有独立的 tool role 用于工具调用结果；Anthropic 没有 tool role，
 *      需要用 user role 发送格式化的工具返回文本：
 *        "Tool result {tool_call_id}: {content}"
 *
 *   3. Assistant 消息保留：role 不变，只取 content 字段。
 *
 *   4. 其他 role（user 等）：保留 role，取 content 字段。
 *
 * ─── 工具格式转换（OpenAI function 格式 → Anthropic tool 格式）─────────
 *
 *   OpenAI 工具定义：
 *     { type: "function", function: { name, description, parameters } }
 *
 *   Anthropic 工具定义：
 *     { name, description, input_schema }
 *
 *   parameters 对象被序列化后重新解析为 input_schema，实现格式平铺。
 *   空工具数组不会被设置到请求体中。
 *
 * ─── build_request — 构造 Anthropic HTTP 请求 ──────────────────────────
 *
 *   终端点：  POST {base_url}/v1/messages
 *   认证方式：x-api-key: {api_key}（必需）
 *   版本头：  anthropic-version: 2023-06-01
 *   Content-Type：application/json
 *   流格式：  SSE（CC_LLM_STREAM_SSE）
 *
 *   请求体 JSON 字段：
 *     - model       —— 优先用 request->model，回退到 default_model
 *     - max_tokens  —— request->max_tokens（Anthropic 必需字段）
 *     - temperature —— request->temperature
 *     - stream      —— 布尔值
 *     - system      —— 拼接后的 system prompt（顶层字符串）
 *     - messages    —— 格式转换后的消息数组
 *     - tools       —— 格式转换后的工具数组
 *
 * ─── parse_response — 解析非流式响应 ───────────────────────────────────
 *
 *   响应 JSON 结构（Anthropic Messages API）：
 *     error.message                       —— API 错误消息
 *     content[]                           —— 内容块数组：
 *       { type: "text", text: "..." }      —— 文本块，拼接所有文本块内容
 *       { type: "tool_use", id, name, input } —— 工具调用块，只取第一个
 *     stop_reason                         —— "end_turn" 表示正常结束
 *
 *   与 OpenAI 的关键差异：
 *     - content 是数组而非字符串，需要遍历拼接所有 text 类型的块
 *     - 工具调用结构不同：id/name/input 是 content 块的直接子字段
 *     - arguments 在 Anthropic 中名为 input
 *
 * ─── parse_stream_event — 解析流式 SSE 事件 ────────────────────────────
 *
 *   Anthropic 流式事件使用 type 字段区分事件类型，与 OpenAI 的 choices/delta
 *   结构完全不同：
 *
 *   content_block_start：
 *     content_block.type = "tool_use" → CC_STREAM_CHUNK_TOOL_START
 *     （携带 name 和 id）
 *
 *   content_block_delta：
 *     delta.type = "text_delta"       → CC_STREAM_CHUNK_TEXT
 *     delta.type = "thinking_delta"   → CC_STREAM_CHUNK_THINKING
 *     delta.type = "input_json_delta" → CC_STREAM_CHUNK_TOOL_DELTA
 *     （携带 partial_json 增量）
 *
 *   message_delta：
 *     delta.stop_reason = "tool_use"  → CC_STREAM_CHUNK_TOOL_END
 *     delta.stop_reason = "end_turn"  → out_finished = 1
 *
 *   message_stop：
 *     → out_finished = 1
 *
 *   与 OpenAI 流式的关键差异：
 *     - 事件按 type 路由而非遍历 choices 数组
 *     - 工具调用 delta 是 partial_json 字符串（增量 JSON 片段）
 *     - 有专门的 thinking_delta 事件类型用于推理链
 *     - 结束信号可能是 message_delta.stop_reason 或 message_stop
 *
 * ─── 默认值与创建 ────────────────────────────────────────────────────────
 *
 *   cc_anthropic_provider_create() 将 protocol.self 设为 NULL，委托
 *   cc_http_llm_provider_create() 组合传输层与协议层：
 *     - 默认 base_url：https://api.anthropic.com
 *     - 默认 model：   claude-3-5-haiku-latest
 */

#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/**
 * add_header — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * @param request 借用的对象；函数不释放该对象本身。
 * @param name 借用的只读字符串；函数不会释放该指针。
 * @param value 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t add_header(cc_llm_http_request_t *request, const char *name, const char *value)
{
    cc_http_header_t *headers = realloc(
        request->headers,
        sizeof(cc_http_header_t) * (request->header_count + 1));
    if (!headers) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to add HTTP header");
    request->headers = headers;
    size_t index = request->header_count++;
    request->headers[index].name = strdup(name);
    request->headers[index].value = strdup(value ? value : "");
    if (!request->headers[index].name || !request->headers[index].value) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP header");
    }
    return cc_result_ok();
}

/**
 * anthropic_name — 返回端口、工具或协议的静态名称字符串，用于注册和日志。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @return 返回借用或静态只读字符串；调用方不得释放。
 */
static const char *anthropic_name(void *self)
{
    (void)self;
    return "anthropic";
}

/**
 * anthropic_text_message — 处理消息对象的创建、复制、字段更新或序列化。
 *
 * @param role 借用的只读字符串；函数不会释放该指针。
 * @param content 借用的只读字符串；函数不会释放该指针。
 * @return 返回借用对象指针；NULL 表示未注入、未找到或当前对象无效。
 */
static cc_json_value_t *anthropic_text_message(const char *role, const char *content)
{
    cc_json_value_t *message = cc_json_create_object();
    cc_json_object_set(message, "role", cc_json_create_string(role));
    cc_json_object_set(message, "content", cc_json_create_string(content ? content : ""));
    return message;
}

/**
 * append_anthropic_messages — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * @param messages_json 借用的只读字符串；函数不会释放该指针。
 */
static void append_anthropic_messages(
    cc_json_value_t *body,
    const char *messages_json
)
{
    cc_json_value_t *src = NULL;
    cc_result_t rc = cc_json_parse(messages_json, &src);
    if (rc.code != CC_OK || !src || !cc_json_is_array(src)) {
        cc_result_free(&rc);
        if (src) cc_json_destroy(src);
        cc_json_object_set(body, "messages", cc_json_create_array());
        return;
    }

    cc_json_value_t *messages = cc_json_create_array();
    cc_string_builder_t system;
    cc_string_builder_init(&system);

    int count = cc_json_array_size(src);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *msg = cc_json_array_get(src, i);
        const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
        const char *content = cc_json_string_value(cc_json_object_get(msg, "content"));
        if (!role) continue;

        if (strcmp(role, "system") == 0) {
            if (content && *content) {
                if (system.length > 0) cc_string_builder_append(&system, "\n");
                cc_string_builder_append(&system, content);
            }
            continue;
        }

        if (strcmp(role, "assistant") == 0) {
            cc_json_array_append(messages, anthropic_text_message("assistant", content ? content : ""));
        } else if (strcmp(role, "tool") == 0) {
            cc_string_builder_t tool_result;
            cc_string_builder_init(&tool_result);
            const char *tool_id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
            cc_string_builder_appendf(&tool_result, "Tool result%s%s: %s",
                tool_id ? " " : "",
                tool_id ? tool_id : "",
                content ? content : "");
            char *text = cc_string_builder_take(&tool_result);
            cc_json_array_append(messages, anthropic_text_message("user", text ? text : ""));
            free(text);
        } else {
            cc_json_array_append(messages, anthropic_text_message("user", content ? content : ""));
        }
    }

    if (system.length > 0) {
        cc_json_object_set(body, "system", cc_json_create_string(cc_string_builder_cstr(&system)));
    }
    cc_string_builder_deinit(&system);
    cc_json_destroy(src);
    cc_json_object_set(body, "messages", messages);
}

/**
 * append_anthropic_tools — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * @param tools_json 借用的只读字符串；函数不会释放该指针。
 */
static void append_anthropic_tools(cc_json_value_t *body, const char *tools_json)
{
    if (!tools_json || strlen(tools_json) <= 2) return;
    cc_json_value_t *src = NULL;
    cc_result_t rc = cc_json_parse(tools_json, &src);
    if (rc.code != CC_OK || !src || !cc_json_is_array(src)) {
        cc_result_free(&rc);
        if (src) cc_json_destroy(src);
        return;
    }

    cc_json_value_t *tools = cc_json_create_array();
    int count = cc_json_array_size(src);
    for (int i = 0; i < count; i++) {
        cc_json_value_t *tool = cc_json_array_get(src, i);
        cc_json_value_t *func = cc_json_object_get(tool, "function");
        if (!func) continue;

        cc_json_value_t *anthropic_tool = cc_json_create_object();
        const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
        const char *description = cc_json_string_value(cc_json_object_get(func, "description"));
        cc_json_object_set(anthropic_tool, "name", cc_json_create_string(name ? name : ""));
        cc_json_object_set(anthropic_tool, "description", cc_json_create_string(description ? description : ""));

        cc_json_value_t *params = cc_json_object_get(func, "parameters");
        if (params) {
            char *schema = cc_json_stringify_unformatted(params);
            cc_json_value_t *schema_copy = NULL;
            if (schema && cc_json_parse(schema, &schema_copy).code == CC_OK && schema_copy) {
                cc_json_object_set(anthropic_tool, "input_schema", schema_copy);
            } else {
                cc_json_object_set(anthropic_tool, "input_schema", cc_json_create_object());
            }
            free(schema);
        } else {
            cc_json_object_set(anthropic_tool, "input_schema", cc_json_create_object());
        }
        cc_json_array_append(tools, anthropic_tool);
    }

    if (cc_json_array_size(tools) > 0) {
        cc_json_object_set(body, "tools", tools);
    } else {
        cc_json_destroy(tools);
    }
    cc_json_destroy(src);
}

/**
 * anthropic_build_request — 把统一 chat request 转换为该 provider 的 HTTP URL、header 和 JSON body。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param default_model 借用的只读字符串；函数不会释放该指针。
 * @param request 借用的对象；函数不释放该对象本身。
 * @param stream 按值传入，用于控制本次操作。
 * @param out_request 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t anthropic_build_request(
    void *self,
    const char *base_url,
    const char *api_key,
    const char *default_model,
    const cc_llm_chat_request_t *request,
    int stream,
    cc_llm_http_request_t *out_request
)
{
    (void)self;
    memset(out_request, 0, sizeof(*out_request));
    out_request->stream_kind = stream ? CC_LLM_STREAM_SSE : CC_LLM_STREAM_NONE;

    cc_string_builder_t url;
    cc_result_t rc = cc_string_builder_init(&url);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_appendf(&url, "%s/v1/messages", base_url);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&url);
        return rc;
    }
    out_request->url = cc_string_builder_take(&url);
    out_request->api_key = api_key ? strdup(api_key) : NULL;

    rc = add_header(out_request, "Content-Type", "application/json");
    if (rc.code == CC_OK) rc = add_header(out_request, "x-api-key", api_key ? api_key : "");
    if (rc.code == CC_OK) rc = add_header(out_request, "anthropic-version", "2023-06-01");
    if (rc.code != CC_OK) {
        cc_llm_http_request_cleanup(out_request);
        return rc;
    }

    cc_json_value_t *body = cc_json_create_object();
    if (!body) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create Anthropic request");
    }
    cc_json_object_set(body, "model", cc_json_create_string(
        request->model ? request->model : default_model));
    cc_json_object_set(body, "max_tokens", cc_json_create_number(request->max_tokens));
    cc_json_object_set(body, "temperature", cc_json_create_number(request->temperature));
    cc_json_object_set(body, "stream", cc_json_create_bool(stream));
    append_anthropic_messages(body, request->messages_json);
    append_anthropic_tools(body, request->tools_json);

    out_request->body_json = cc_json_stringify_unformatted(body);
    cc_json_destroy(body);
    if (!out_request->url || !out_request->body_json ||
        (api_key && !out_request->api_key)) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build Anthropic request");
    }
    return cc_result_ok();
}

/**
 * anthropic_parse_response — 解析 provider 的完整响应 JSON，填充统一 LLM response。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param response_json 借用的只读字符串；函数不会释放该指针。
 * @param out_response 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t anthropic_parse_response(
    void *self,
    const char *response_json,
    cc_llm_response_t *out_response
)
{
    (void)self;
    memset(out_response, 0, sizeof(*out_response));

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(response_json, &root);
    if (rc.code != CC_OK || !root) {
        if (root) cc_json_destroy(root);
        return cc_result_error(CC_ERR_MODEL, "Failed to parse Anthropic response");
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Anthropic API error");
        cc_json_destroy(root);
        return err;
    }

    cc_json_value_t *content = cc_json_object_get(root, "content");
    if (content && cc_json_is_array(content)) {
        cc_string_builder_t text;
        cc_string_builder_init(&text);
        int count = cc_json_array_size(content);
        for (int i = 0; i < count; i++) {
            cc_json_value_t *block = cc_json_array_get(content, i);
            const char *type = cc_json_string_value(cc_json_object_get(block, "type"));
            if (type && strcmp(type, "text") == 0) {
                const char *block_text = cc_json_string_value(cc_json_object_get(block, "text"));
                if (block_text) cc_string_builder_append(&text, block_text);
            } else if (type && strcmp(type, "tool_use") == 0 && !out_response->has_tool_call) {
                out_response->has_tool_call = 1;
                const char *id = cc_json_string_value(cc_json_object_get(block, "id"));
                const char *name = cc_json_string_value(cc_json_object_get(block, "name"));
                out_response->tool_call.id = strdup(id ? id : "");
                out_response->tool_call.name = strdup(name ? name : "");
                cc_json_value_t *input = cc_json_object_get(block, "input");
                out_response->tool_call.arguments_json =
                    input ? cc_json_stringify_unformatted(input) : strdup("{}");
            }
        }
        if (text.length > 0) {
            out_response->has_text = 1;
            out_response->text = cc_string_builder_take(&text);
        } else {
            cc_string_builder_deinit(&text);
        }
    }

    const char *stop_reason = cc_json_string_value(cc_json_object_get(root, "stop_reason"));
    out_response->finished = (stop_reason && strcmp(stop_reason, "end_turn") == 0) ? 1 : 0;
    cc_json_destroy(root);
    return cc_result_ok();
}

/**
 * anthropic_parse_stream_event — 解析 provider 的一段流式事件，并通过统一 chunk 回调交给 runtime。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param event_json 借用的只读字符串；函数不会释放该指针。
 * @param on_chunk 按值传入，用于控制本次操作。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 * @param out_finished 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t anthropic_parse_stream_event(
    void *self,
    const char *event_json,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data,
    int *out_finished
)
{
    (void)self;
    *out_finished = 0;

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(event_json, &root);
    if (rc.code != CC_OK || !root) {
        if (root) cc_json_destroy(root);
        cc_result_free(&rc);
        return cc_result_ok();
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Anthropic API error");
        cc_json_destroy(root);
        return err;
    }

    const char *type = cc_json_string_value(cc_json_object_get(root, "type"));
    if (type && strcmp(type, "content_block_start") == 0) {
        cc_json_value_t *block = cc_json_object_get(root, "content_block");
        const char *block_type = cc_json_string_value(cc_json_object_get(block, "type"));
        if (block_type && strcmp(block_type, "tool_use") == 0) {
            const char *id = cc_json_string_value(cc_json_object_get(block, "id"));
            const char *name = cc_json_string_value(cc_json_object_get(block, "name"));
            if (name && *name) {
                cc_stream_chunk_t chunk = {
                    CC_STREAM_CHUNK_TOOL_START, NULL, (char *)name, (char *)(id ? id : "")
                };
                on_chunk(&chunk, user_data);
            }
        }
    } else if (type && strcmp(type, "content_block_delta") == 0) {
        cc_json_value_t *delta = cc_json_object_get(root, "delta");
        const char *delta_type = cc_json_string_value(cc_json_object_get(delta, "type"));
        if (delta_type && strcmp(delta_type, "text_delta") == 0) {
            const char *text = cc_json_string_value(cc_json_object_get(delta, "text"));
            if (text && *text) {
                cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_TEXT, (char *)text, NULL, NULL };
                on_chunk(&chunk, user_data);
            }
        } else if (delta_type && strcmp(delta_type, "thinking_delta") == 0) {
            const char *thinking = cc_json_string_value(cc_json_object_get(delta, "thinking"));
            if (thinking && *thinking) {
                cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_THINKING, (char *)thinking, NULL, NULL };
                on_chunk(&chunk, user_data);
            }
        } else if (delta_type && strcmp(delta_type, "input_json_delta") == 0) {
            const char *partial = cc_json_string_value(cc_json_object_get(delta, "partial_json"));
            if (partial && *partial) {
                cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_TOOL_DELTA, (char *)partial, NULL, NULL };
                on_chunk(&chunk, user_data);
            }
        }
    } else if (type && strcmp(type, "message_delta") == 0) {
        cc_json_value_t *delta = cc_json_object_get(root, "delta");
        const char *stop_reason = cc_json_string_value(cc_json_object_get(delta, "stop_reason"));
        if (stop_reason && strcmp(stop_reason, "tool_use") == 0) {
            cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_TOOL_END, NULL, NULL, NULL };
            on_chunk(&chunk, user_data);
        } else if (stop_reason && strcmp(stop_reason, "end_turn") == 0) {
            *out_finished = 1;
        }
    } else if (type && strcmp(type, "message_stop") == 0) {
        *out_finished = 1;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

static cc_llm_protocol_vtable_t anthropic_protocol_vtable = {
    anthropic_name,
    anthropic_build_request,
    anthropic_parse_response,
    anthropic_parse_stream_event,
    NULL
};

/**
 * cc_anthropic_provider_create — 完成对应初始化步骤，失败时返回 cc_result_t 错误。
 *
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param model 借用的只读字符串；函数不会释放该指针。
 * @param out_provider 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_anthropic_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
)
{
    cc_llm_protocol_t protocol = { NULL, &anthropic_protocol_vtable };
    return cc_http_llm_provider_create(
        base_url ? base_url : "https://api.anthropic.com",
        api_key,
        model ? model : "claude-3-5-haiku-latest",
        protocol,
        out_provider);
}
