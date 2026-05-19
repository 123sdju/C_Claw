/**
 * 学习导读：cclaw/adapters/src/llm/cc_anthropic_provider.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/**
 * add_header — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
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
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
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
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
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
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param body 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param messages_json 借用的只读字符串；函数不会释放该指针。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
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
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param body 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param tools_json 借用的只读字符串；函数不会释放该指针。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
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
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param default_model 借用的只读字符串；函数不会释放该指针。
 * @param request 借用的对象；函数不释放该对象本身。
 * @param stream 按值传入，用于控制本次操作。
 * @param out_request 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
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
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param response_json 借用的只读字符串；函数不会释放该指针。
 * @param out_response 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
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
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param event_json 借用的只读字符串；函数不会释放该指针。
 * @param on_chunk 按值传入，用于控制本次操作。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 * @param out_finished 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
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
 * cc_anthropic_provider_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：LLM 协议适配层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param model 借用的只读字符串；函数不会释放该指针。
 * @param out_provider 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
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
