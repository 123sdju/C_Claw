/**
 * 学习导读：cclaw/adapters/src/llm/cc_openai_provider.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_openai_provider.c — OpenAI Chat Completions API 协议策略模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于适配器层，是 LLM provider 两层策略中的**协议层**。只实现
 * cc_llm_protocol_t vtable 的四个回调，负责 OpenAI Chat Completions API
 * 特有的 JSON 格式转换。HTTP 传输细节完全由 cc_http_llm_provider 处理。
 *
 * 本模块不持有任何私有状态（self 始终为 NULL），因此不需要 destroy 回调。
 *
 * 上游调用方：
 *   - cc_http_llm_provider.c — 通过 cc_llm_protocol_t vtable 委托调用
 *     build_request / parse_response / parse_stream_event
 *
 * 下游依赖模块：
 *   - cc_json.c — JSON 对象构造与解析
 *   - cc_string_builder.c — URL 拼接、Authorization header 拼接
 *   - cc_http_llm_provider.h — cc_llm_http_request_t 结构体、cc_llm_stream_kind
 *
 * ─── build_request — 构造 OpenAI HTTP 请求 ──────────────────────────────
 *
 *   终端点：  POST {base_url}/v1/chat/completions
 *   认证方式：Authorization: Bearer {api_key}（api_key 为空则跳过）
 *   Content-Type：application/json
 *   流格式：  SSE（CC_LLM_STREAM_SSE）
 *
 *   请求体 JSON 字段：
 *     - model       —— 优先用 request->model，回退到 default_model
 *     - messages    —— request->messages_json 直接解析为 JSON 数组透传
 *     - stream      —— 布尔值，控制是否启用流式
 *     - max_tokens  —— request->max_tokens
 *     - temperature —— request->temperature
 *     - tools       —— request->tools_json 解析后透传（非空数组时）
 *     - tool_choice —— "auto"（仅在有 tools 时设置）
 *
 *   messages_json 已经是 OpenAI 原生格式（role/content 数组），无需格式转换，
 *   直接解析为 cc_json_value_t 后嵌入请求体。若解析失败则回退为空数组。
 *
 * ─── parse_response — 解析非流式响应 ────────────────────────────────────
 *
 *   响应 JSON 结构（OpenAI Chat Completions API）：
 *     error.message                          —— API 错误消息
 *     choices[0].message.content             —— 文本回复（→ out_response.text）
 *     choices[0].message.reasoning_content   —— 推理链内容（→ reasoning_content）
 *     choices[0].message.tool_calls[0]       —— 工具调用（→ tool_call）
 *       .id / .function.name / .function.arguments
 *     choices[0].finish_reason               —— "stop" 表示正常结束
 *
 *   只取 choices[0]（第一个候选项），适用于非流式单候选场景。
 *
 * ─── parse_stream_event — 解析流式 SSE 事件 ─────────────────────────────
 *
 *   每条 SSE data 是一个 JSON 对象，包含一个 delta 增量：
 *     choices[0].delta.content               —— 文本增量 → CC_STREAM_CHUNK_TEXT
 *     choices[0].delta.reasoning_content     —— 推理增量 → CC_STREAM_CHUNK_THINKING
 *     choices[0].delta.tool_calls[i]         —— 工具调用增量：
 *       .function.name    → CC_STREAM_CHUNK_TOOL_START（首次出现时）
 *       .function.arguments → CC_STREAM_CHUNK_TOOL_DELTA（增量拼接）
 *     choices[0].finish_reason = "tool_calls" → CC_STREAM_CHUNK_TOOL_END
 *     choices[0].finish_reason = "stop"       → out_finished = 1
 *
 *   流式 tool_call 采用增量模式：每段 arguments JSON 以 CC_STREAM_CHUNK_TOOL_DELTA
 *   发出，上层负责将所有片段拼接为完整 arguments。
 *
 * ─── 默认值与创建 ────────────────────────────────────────────────────────
 *
 *   cc_openai_provider_create() 将 protocol.self 设为 NULL，委托
 *   cc_http_llm_provider_create() 组合传输层与协议层：
 *     - 默认 base_url：https://api.openai.com
 *     - 默认 model：   gpt-4o-mini
 */

#include "cc/adapters/cc_llm_providers.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_memory.h"
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
static cc_result_t add_header(
    cc_llm_http_request_t *request,
    const char *name,
    const char *value
)
{
    cc_http_header_t *headers = realloc(
        request->headers,
        sizeof(cc_http_header_t) * (request->header_count + 1));
    if (!headers) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to add HTTP header");
    request->headers = headers;
    size_t index = request->header_count++;
    request->headers[index].name = cc_strdup(name);
    request->headers[index].value = cc_strdup(value ? value : "");
    if (!request->headers[index].name || !request->headers[index].value) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP header");
    }
    return cc_result_ok();
}

/**
 * add_bearer_header — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * @param request 借用的对象；函数不释放该对象本身。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t add_bearer_header(cc_llm_http_request_t *request, const char *api_key)
{
    if (!api_key || !*api_key) return cc_result_ok();

    cc_string_builder_t auth;
    cc_result_t rc = cc_string_builder_init(&auth);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_appendf(&auth, "Bearer %s", api_key);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&auth);
        return rc;
    }
    char *value = cc_string_builder_take(&auth);
    rc = add_header(request, "Authorization", value);
    free(value);
    return rc;
}

/**
 * openai_name — 返回端口、工具或协议的静态名称字符串，用于注册和日志。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @return 返回借用或静态只读字符串；调用方不得释放。
 */
static const char *openai_name(void *self)
{
    (void)self;
    return "openai";
}

static cc_json_value_t *json_clone_value(const cc_json_value_t *value)
{
    char *json = cc_json_stringify_unformatted(value);
    if (!json) return NULL;
    cc_json_value_t *copy = NULL;
    cc_json_parse(json, &copy);
    free(json);
    return copy;
}

static const char *part_mime_or_default(const cc_json_value_t *part, const char *fallback)
{
    const char *mime = cc_json_string_value(cc_json_object_get(part, "mime"));
    return (mime && *mime) ? mime : fallback;
}

static const char *audio_format_from_mime(const char *mime)
{
    if (!mime) return "wav";
    if (strstr(mime, "mp3") || strstr(mime, "mpeg")) return "mp3";
    if (strstr(mime, "wav")) return "wav";
    return "wav";
}

static char *describe_unsupported_part(const char *provider, const cc_json_value_t *part)
{
    const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
    const char *id = cc_json_string_value(cc_json_object_get(part, "id"));
    const char *mime = cc_json_string_value(cc_json_object_get(part, "mime"));
    const char *path = cc_json_string_value(cc_json_object_get(part, "path"));
    double bytes = cc_json_number_value(cc_json_object_get(part, "bytes"));
    double width = cc_json_number_value(cc_json_object_get(part, "width"));
    double height = cc_json_number_value(cc_json_object_get(part, "height"));
    double duration_ms = cc_json_number_value(cc_json_object_get(part, "duration_ms"));

    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) return strdup("[multimodal artifact]");
    cc_string_builder_appendf(&sb, "[%s multimodal fallback: type=%s",
        provider ? provider : "provider", type ? type : "file");
    if (id && *id) cc_string_builder_appendf(&sb, " id=%s", id);
    if (mime && *mime) cc_string_builder_appendf(&sb, " mime=%s", mime);
    if (path && *path) cc_string_builder_appendf(&sb, " path=%s", path);
    if (bytes > 0) cc_string_builder_appendf(&sb, " bytes=%.0f", bytes);
    if (width > 0 && height > 0) cc_string_builder_appendf(&sb, " size=%.0fx%.0f", width, height);
    if (duration_ms > 0) cc_string_builder_appendf(&sb, " duration_ms=%.0f", duration_ms);
    cc_string_builder_append(&sb, " multimodal_fallback=true]");
    return cc_string_builder_take(&sb);
}

static void openai_append_text_part(cc_json_value_t *arr, const char *text)
{
    cc_json_value_t *part = cc_json_create_object();
    cc_json_object_set(part, "type", cc_json_create_string("text"));
    cc_json_object_set(part, "text", cc_json_create_string(text ? text : ""));
    cc_json_array_append(arr, part);
}

static cc_json_value_t *openai_transform_content_parts(const cc_json_value_t *parts)
{
    cc_json_value_t *out = cc_json_create_array();
    int count = cc_json_array_size(parts);
    for (int i = 0; i < count; ++i) {
        cc_json_value_t *part = cc_json_array_get(parts, i);
        const char *type = cc_json_string_value(cc_json_object_get(part, "type"));
        if (!type) continue;

        if (strcmp(type, "text") == 0) {
            openai_append_text_part(out,
                cc_json_string_value(cc_json_object_get(part, "text")));
        } else if (strcmp(type, "image") == 0) {
            const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
            if (data && *data) {
                const char *mime = part_mime_or_default(part, "image/png");
                cc_string_builder_t url;
                cc_string_builder_init(&url);
                cc_string_builder_appendf(&url, "data:%s;base64,%s", mime, data);
                char *data_url = cc_string_builder_take(&url);
                cc_json_value_t *image_part = cc_json_create_object();
                cc_json_object_set(image_part, "type", cc_json_create_string("image_url"));
                cc_json_value_t *image_url = cc_json_create_object();
                cc_json_object_set(image_url, "url", cc_json_create_string(data_url ? data_url : ""));
                cc_json_object_set(image_part, "image_url", image_url);
                cc_json_array_append(out, image_part);
                free(data_url);
            } else {
                char *fallback = describe_unsupported_part("openai", part);
                openai_append_text_part(out, fallback);
                free(fallback);
            }
        } else if (strcmp(type, "audio") == 0) {
            const char *data = cc_json_string_value(cc_json_object_get(part, "data_base64"));
            if (data && *data) {
                const char *mime = part_mime_or_default(part, "audio/wav");
                cc_json_value_t *audio_part = cc_json_create_object();
                cc_json_object_set(audio_part, "type", cc_json_create_string("input_audio"));
                cc_json_value_t *input_audio = cc_json_create_object();
                cc_json_object_set(input_audio, "data", cc_json_create_string(data));
                cc_json_object_set(input_audio, "format",
                    cc_json_create_string(audio_format_from_mime(mime)));
                cc_json_object_set(audio_part, "input_audio", input_audio);
                cc_json_array_append(out, audio_part);
            } else {
                char *fallback = describe_unsupported_part("openai", part);
                openai_append_text_part(out, fallback);
                free(fallback);
            }
        } else {
            char *fallback = describe_unsupported_part("openai", part);
            openai_append_text_part(out, fallback);
            free(fallback);
        }
    }
    return out;
}

static cc_json_value_t *openai_transform_message(const cc_json_value_t *msg)
{
    cc_json_value_t *out = cc_json_create_object();
    const char *role = cc_json_string_value(cc_json_object_get(msg, "role"));
    cc_json_object_set(out, "role", cc_json_create_string(role ? role : "user"));

    cc_json_value_t *content = cc_json_object_get(msg, "content");
    if (content && cc_json_is_array(content)) {
        cc_json_object_set(out, "content", openai_transform_content_parts(content));
    } else if (content) {
        cc_json_value_t *copy = json_clone_value(content);
        if (copy) cc_json_object_set(out, "content", copy);
    }

    const char *tool_call_id = cc_json_string_value(cc_json_object_get(msg, "tool_call_id"));
    if (tool_call_id) cc_json_object_set(out, "tool_call_id", cc_json_create_string(tool_call_id));
    const char *reasoning = cc_json_string_value(cc_json_object_get(msg, "reasoning_content"));
    if (reasoning) cc_json_object_set(out, "reasoning_content", cc_json_create_string(reasoning));
    cc_json_value_t *tool_calls = cc_json_object_get(msg, "tool_calls");
    if (tool_calls) {
        cc_json_value_t *copy = json_clone_value(tool_calls);
        if (copy) cc_json_object_set(out, "tool_calls", copy);
    }
    return out;
}

static cc_json_value_t *openai_transform_messages(cc_json_value_t *messages)
{
    if (!messages || !cc_json_is_array(messages)) return cc_json_create_array();
    cc_json_value_t *out = cc_json_create_array();
    int count = cc_json_array_size(messages);
    for (int i = 0; i < count; ++i) {
        cc_json_array_append(out, openai_transform_message(cc_json_array_get(messages, i)));
    }
    return out;
}

/**
 * openai_build_request — 把统一 chat request 转换为该 provider 的 HTTP URL、header 和 JSON body。
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
static cc_result_t openai_build_request(
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

    cc_string_builder_t url;
    cc_result_t rc = cc_string_builder_init(&url);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_appendf(&url, "%s/v1/chat/completions", base_url);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&url);
        return rc;
    }
    out_request->url = cc_string_builder_take(&url);
    out_request->api_key = api_key ? cc_strdup(api_key) : NULL;
    out_request->stream_kind = stream ? CC_LLM_STREAM_SSE : CC_LLM_STREAM_NONE;

    rc = add_header(out_request, "Content-Type", "application/json");
    if (rc.code == CC_OK) rc = add_bearer_header(out_request, api_key);
    if (rc.code != CC_OK) {
        cc_llm_http_request_cleanup(out_request);
        return rc;
    }

    cc_json_value_t *body = cc_json_create_object();
    if (!body) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create OpenAI request");
    }
    cc_json_object_set(body, "model", cc_json_create_string(
        request->model ? request->model : default_model));

    cc_json_value_t *messages = NULL;
    rc = cc_json_parse(request->messages_json, &messages);
    if (rc.code == CC_OK && messages) {
        cc_json_value_t *converted = openai_transform_messages(messages);
        cc_json_destroy(messages);
        cc_json_object_set(body, "messages", converted);
    } else {
        cc_result_free(&rc);
        cc_json_object_set(body, "messages", cc_json_create_array());
    }

    cc_json_object_set(body, "stream", cc_json_create_bool(stream));
    cc_json_object_set(body, "max_tokens", cc_json_create_number(request->max_tokens));
    cc_json_object_set(body, "temperature", cc_json_create_number(request->temperature));

    if (request->tools_json && strlen(request->tools_json) > 2) {
        cc_json_value_t *tools = NULL;
        rc = cc_json_parse(request->tools_json, &tools);
        if (rc.code == CC_OK && tools) {
            cc_json_object_set(body, "tools", tools);
            cc_json_object_set(body, "tool_choice", cc_json_create_string("auto"));
        } else {
            cc_result_free(&rc);
        }
    }

    out_request->body_json = cc_json_stringify_unformatted(body);
    cc_json_destroy(body);
    if (!out_request->url || !out_request->body_json ||
        (api_key && !out_request->api_key)) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build OpenAI request");
    }
    return cc_result_ok();
}

/**
 * openai_parse_response — 解析 provider 的完整响应 JSON，填充统一 LLM response。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param response_json 借用的只读字符串；函数不会释放该指针。
 * @param out_response 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t openai_parse_response(
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
        return cc_result_error(CC_ERR_MODEL, "Failed to parse OpenAI response");
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(cc_json_object_get(api_error, "message"));
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown OpenAI API error");
        cc_json_destroy(root);
        return err;
    }

    cc_json_value_t *choices = cc_json_object_get(root, "choices");
    if (choices && cc_json_is_array(choices) && cc_json_array_size(choices) > 0) {
        cc_json_value_t *choice = cc_json_array_get(choices, 0);
        cc_json_value_t *message = cc_json_object_get(choice, "message");
        if (message) {
            const char *content = cc_json_string_value(cc_json_object_get(message, "content"));
            if (content) {
                out_response->has_text = 1;
                out_response->text = cc_strdup(content);
            }

            const char *reasoning = cc_json_string_value(cc_json_object_get(message, "reasoning_content"));
            if (reasoning) out_response->reasoning_content = cc_strdup(reasoning);

            cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
            if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
                cc_json_value_t *tc = cc_json_array_get(tool_calls, 0);
                cc_json_value_t *func = cc_json_object_get(tc, "function");
                out_response->has_tool_call = 1;
                out_response->tool_call.id = cc_strdup(
                    cc_json_string_value(cc_json_object_get(tc, "id")) ?
                    cc_json_string_value(cc_json_object_get(tc, "id")) : "");
                out_response->tool_call.name = cc_strdup(
                    cc_json_string_value(cc_json_object_get(func, "name")) ?
                    cc_json_string_value(cc_json_object_get(func, "name")) : "");
                out_response->tool_call.arguments_json = cc_strdup(
                    cc_json_string_value(cc_json_object_get(func, "arguments")) ?
                    cc_json_string_value(cc_json_object_get(func, "arguments")) : "{}");
            }
        }

        const char *finish = cc_json_string_value(cc_json_object_get(choice, "finish_reason"));
        out_response->finished = (finish && strcmp(finish, "stop") == 0) ? 1 : 0;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

/**
 * openai_parse_stream_event — 解析 provider 的一段流式事件，并通过统一 chunk 回调交给 runtime。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param event_json 借用的只读字符串；函数不会释放该指针。
 * @param on_chunk 按值传入，用于控制本次操作。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 * @param out_finished 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t openai_parse_stream_event(
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

    cc_json_value_t *choices = cc_json_object_get(root, "choices");
    if (!choices || !cc_json_is_array(choices) || cc_json_array_size(choices) == 0) {
        cc_json_destroy(root);
        return cc_result_ok();
    }

    cc_json_value_t *choice = cc_json_array_get(choices, 0);
    const char *finish = cc_json_string_value(cc_json_object_get(choice, "finish_reason"));
    cc_json_value_t *delta = cc_json_object_get(choice, "delta");

    if (delta) {
        cc_json_value_t *tool_calls = cc_json_object_get(delta, "tool_calls");
        if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
            int count = cc_json_array_size(tool_calls);
            for (int i = 0; i < count; i++) {
                cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                cc_json_value_t *func = cc_json_object_get(tc, "function");
                const char *tool_id = cc_json_string_value(cc_json_object_get(tc, "id"));
                if (!func) continue;

                const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
                if (name && *name) {
                    cc_stream_chunk_t chunk = {
                        CC_STREAM_CHUNK_TOOL_START, NULL, (char *)name, (char *)(tool_id ? tool_id : "")
                    };
                    on_chunk(&chunk, user_data);
                }

                const char *args = cc_json_string_value(cc_json_object_get(func, "arguments"));
                if (args && *args) {
                    cc_stream_chunk_t chunk = {
                        CC_STREAM_CHUNK_TOOL_DELTA, (char *)args, NULL, (char *)(tool_id ? tool_id : "")
                    };
                    on_chunk(&chunk, user_data);
                }
            }
        } else {
            const char *reasoning = cc_json_string_value(cc_json_object_get(delta, "reasoning_content"));
            if (reasoning && *reasoning) {
                cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_THINKING, (char *)reasoning, NULL, NULL };
                on_chunk(&chunk, user_data);
            }

            const char *content = cc_json_string_value(cc_json_object_get(delta, "content"));
            if (content && *content) {
                cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_TEXT, (char *)content, NULL, NULL };
                on_chunk(&chunk, user_data);
            }
        }
    }

    if (finish && strcmp(finish, "tool_calls") == 0) {
        cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_TOOL_END, NULL, NULL, NULL };
        on_chunk(&chunk, user_data);
    } else if (finish && strcmp(finish, "stop") == 0) {
        *out_finished = 1;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

static cc_llm_protocol_vtable_t openai_protocol_vtable = {
    openai_name,
    openai_build_request,
    openai_parse_response,
    openai_parse_stream_event,
    NULL
};

/**
 * cc_openai_provider_create — 完成对应初始化步骤，失败时返回 cc_result_t 错误。
 *
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param model 借用的只读字符串；函数不会释放该指针。
 * @param out_provider 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_openai_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
)
{
    cc_llm_protocol_t protocol = { NULL, &openai_protocol_vtable };
    return cc_http_llm_provider_create(
        base_url ? base_url : "https://api.openai.com",
        api_key,
        model ? model : "gpt-4o-mini",
        protocol,
        out_provider);
}
