/**
 * 学习导读：cclaw/adapters/src/llm/cc_ollama_provider.c
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

/* 学习注释：add_header 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：ollama_name 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static const char *ollama_name(void *self)
{
    (void)self;
    return "ollama";
}

/* 学习注释：ollama_build_request 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t ollama_build_request(
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
    (void)api_key;
    memset(out_request, 0, sizeof(*out_request));
    out_request->stream_kind = stream ? CC_LLM_STREAM_NDJSON : CC_LLM_STREAM_NONE;

    cc_string_builder_t url;
    cc_result_t rc = cc_string_builder_init(&url);
    if (rc.code != CC_OK) return rc;
    rc = cc_string_builder_appendf(&url, "%s/api/chat", base_url);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&url);
        return rc;
    }
    out_request->url = cc_string_builder_take(&url);

    rc = add_header(out_request, "Content-Type", "application/json");
    if (rc.code != CC_OK) {
        cc_llm_http_request_cleanup(out_request);
        return rc;
    }

    cc_json_value_t *body = cc_json_create_object();
    if (!body) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create Ollama request");
    }
    cc_json_object_set(body, "model", cc_json_create_string(
        request->model ? request->model : default_model));
    cc_json_object_set(body, "stream", cc_json_create_bool(stream));

    cc_json_value_t *messages = NULL;
    rc = cc_json_parse(request->messages_json, &messages);
    if (rc.code == CC_OK && messages) {
        cc_json_object_set(body, "messages", messages);
    } else {
        cc_result_free(&rc);
        cc_json_object_set(body, "messages", cc_json_create_array());
    }

    if (request->tools_json && strlen(request->tools_json) > 2) {
        cc_json_value_t *tools = NULL;
        rc = cc_json_parse(request->tools_json, &tools);
        if (rc.code == CC_OK && tools) {
            cc_json_object_set(body, "tools", tools);
        } else {
            cc_result_free(&rc);
        }
    }

    cc_json_value_t *options = cc_json_create_object();
    cc_json_object_set(options, "temperature", cc_json_create_number(request->temperature));
    cc_json_object_set(options, "num_predict", cc_json_create_number(request->max_tokens));
    cc_json_object_set(body, "options", options);

    out_request->body_json = cc_json_stringify_unformatted(body);
    cc_json_destroy(body);
    if (!out_request->url || !out_request->body_json) {
        cc_llm_http_request_cleanup(out_request);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to build Ollama request");
    }
    return cc_result_ok();
}

/* 学习注释：ollama_parse_response 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t ollama_parse_response(
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
        return cc_result_error(CC_ERR_MODEL, "Failed to parse Ollama response");
    }

    cc_json_value_t *api_error = cc_json_object_get(root, "error");
    if (api_error) {
        const char *msg = cc_json_string_value(api_error);
        cc_result_t err = cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Ollama API error");
        cc_json_destroy(root);
        return err;
    }

    cc_json_value_t *message = cc_json_object_get(root, "message");
    if (message) {
        const char *content = cc_json_string_value(cc_json_object_get(message, "content"));
        if (content && strlen(content) > 0) {
            out_response->has_text = 1;
            out_response->text = strdup(content);
        }

        const char *reasoning = cc_json_string_value(cc_json_object_get(message, "reasoning_content"));
        if (reasoning) out_response->reasoning_content = strdup(reasoning);

        cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
        if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
            cc_json_value_t *tc = cc_json_array_get(tool_calls, 0);
            cc_json_value_t *func = cc_json_object_get(tc, "function");
            out_response->has_tool_call = 1;
            out_response->tool_call.id = strdup(
                cc_json_string_value(cc_json_object_get(tc, "id")) ?
                cc_json_string_value(cc_json_object_get(tc, "id")) : "");
            out_response->tool_call.name = strdup(
                cc_json_string_value(cc_json_object_get(func, "name")) ?
                cc_json_string_value(cc_json_object_get(func, "name")) : "");
            cc_json_value_t *args = cc_json_object_get(func, "arguments");
            const char *args_text = cc_json_string_value(args);
            out_response->tool_call.arguments_json =
                args_text ? strdup(args_text) : (args ? cc_json_stringify_unformatted(args) : strdup("{}"));
        }
    }

    out_response->finished = cc_json_bool_value(cc_json_object_get(root, "done"));
    cc_json_destroy(root);
    return cc_result_ok();
}

/* 学习注释：ollama_parse_stream_event 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t ollama_parse_stream_event(
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
        const char *msg = cc_json_string_value(api_error);
        cc_json_destroy(root);
        return cc_result_error(CC_ERR_MODEL, msg ? msg : "Unknown Ollama API error");
    }

    cc_json_value_t *message = cc_json_object_get(root, "message");
    if (message) {
        const char *reasoning = cc_json_string_value(cc_json_object_get(message, "reasoning_content"));
        if (reasoning && *reasoning) {
            cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_THINKING, (char *)reasoning, NULL, NULL };
            on_chunk(&chunk, user_data);
        }

        const char *content = cc_json_string_value(cc_json_object_get(message, "content"));
        if (content && *content) {
            cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_TEXT, (char *)content, NULL, NULL };
            on_chunk(&chunk, user_data);
        }

        cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
        if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
            int count = cc_json_array_size(tool_calls);
            for (int i = 0; i < count; i++) {
                cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                cc_json_value_t *func = cc_json_object_get(tc, "function");
                if (!func) continue;

                const char *tool_id = cc_json_string_value(cc_json_object_get(tc, "id"));
                const char *name = cc_json_string_value(cc_json_object_get(func, "name"));
                if (name && *name) {
                    cc_stream_chunk_t chunk = {
                        CC_STREAM_CHUNK_TOOL_START, NULL, (char *)name, (char *)(tool_id ? tool_id : "")
                    };
                    on_chunk(&chunk, user_data);
                }

                cc_json_value_t *args = cc_json_object_get(func, "arguments");
                const char *args_text = cc_json_string_value(args);
                char *args_json = NULL;
                if (!args_text && args) args_json = cc_json_stringify_unformatted(args);
                const char *payload = args_text ? args_text : args_json;
                if (payload && *payload) {
                    cc_stream_chunk_t chunk = {
                        CC_STREAM_CHUNK_TOOL_DELTA, (char *)payload, NULL, (char *)(tool_id ? tool_id : "")
                    };
                    on_chunk(&chunk, user_data);
                }
                free(args_json);
            }
            cc_stream_chunk_t end = { CC_STREAM_CHUNK_TOOL_END, NULL, NULL, NULL };
            on_chunk(&end, user_data);
        }
    }

    if (cc_json_bool_value(cc_json_object_get(root, "done"))) {
        *out_finished = 1;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

static cc_llm_protocol_vtable_t ollama_protocol_vtable = {
    ollama_name,
    ollama_build_request,
    ollama_parse_response,
    ollama_parse_stream_event,
    NULL
};

/* 学习注释：cc_ollama_provider_create 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_ollama_provider_create(
    const char *base_url,
    const char *model,
    cc_llm_provider_t *out_provider
)
{
    cc_llm_protocol_t protocol = { NULL, &ollama_protocol_vtable };
    return cc_http_llm_provider_create(
        base_url ? base_url : "http://localhost:11434",
        NULL,
        model ? model : "qwen2.5-coder:7b",
        protocol,
        out_provider);
}
