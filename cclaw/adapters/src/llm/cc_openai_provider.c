



#include "cc/adapters/cc_llm_providers.h"
#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_json.h"
#include "cc/util/cc_memory.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/*
 * 向协议请求追加 HTTP header。
 *
 * request 拥有 headers 数组和其中的 name/value 字符串；失败时调用方会通过
 * cc_llm_http_request_cleanup 释放已经添加的字段。
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

/*
 * 添加 OpenAI Bearer Authorization header。
 *
 * api_key 为空时允许构造无鉴权请求，便于本地代理或测试；真实云服务通常会返回 401。
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

/* 返回协议名称，通用 HTTP provider 用它推断多模态 capability。 */
static const char *openai_name(void *self)
{
    (void)self;
    return "openai";
}

/*
 * 通过序列化/反序列化克隆 JSON 节点。
 *
 * cc_json 没有通用 clone API 时使用这个保守方法，避免把原始 messages AST 的节点所有权
 * 转移给 OpenAI body。
 */
static cc_json_value_t *json_clone_value(const cc_json_value_t *value)
{
    char *json = cc_json_stringify_unformatted(value);
    if (!json) return NULL;
    cc_json_value_t *copy = NULL;
    cc_json_parse(json, &copy);
    free(json);
    return copy;
}

/* 读取 content part 的 mime 字段，缺失时返回 provider 需要的默认 MIME。 */
static const char *part_mime_or_default(const cc_json_value_t *part, const char *fallback)
{
    const char *mime = cc_json_string_value(cc_json_object_get(part, "mime"));
    return (mime && *mime) ? mime : fallback;
}

/* 将 MIME 映射成 OpenAI input_audio 支持的 format 字段。 */
static const char *audio_format_from_mime(const char *mime)
{
    if (!mime) return "wav";
    if (strstr(mime, "mp3") || strstr(mime, "mpeg")) return "mp3";
    if (strstr(mime, "wav")) return "wav";
    return "wav";
}

/*
 * 为 OpenAI 不支持直传的多模态 part 生成文本 fallback。
 *
 * 这样即使 artifact 只有 path/metadata 没有 inline base64，模型仍能看到资源描述，而不是
 * 静默丢失上下文。
 */
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

/* 追加 OpenAI content 数组中的 text part。 */
static void openai_append_text_part(cc_json_value_t *arr, const char *text)
{
    cc_json_value_t *part = cc_json_create_object();
    cc_json_object_set(part, "type", cc_json_create_string("text"));
    cc_json_object_set(part, "text", cc_json_create_string(text ? text : ""));
    cc_json_array_append(arr, part);
}

/*
 * 将 SDK content parts 转成 OpenAI content 数组。
 *
 * text 直接映射；inline image 变成 image_url data URL；inline audio 变成 input_audio；
 * 其它或非 inline 资源降级成文本描述。
 */
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

/*
 * 转换单条 SDK message。
 *
 * role、content、tool_call_id、reasoning_content、tool_calls 都按 OpenAI schema 复制；
 * content 数组会额外做多模态 part 转换。
 */
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

/* 转换完整 messages 数组；输入非法时返回空数组，避免请求构造失败扩大化。 */
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

/*
 * 构造 OpenAI Chat Completions HTTP 请求。
 *
 * 该函数只生成 url/header/body 和 stream_kind，不执行网络请求。工具 schema 直接透传到
 * tools 字段，stream 模式使用 SSE framing。
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

    char *messages_text = NULL;
    rc = cc_messages_to_json(
        request->messages,
        request->message_count,
        request->thinking_mode,
        &messages_text);
    cc_json_value_t *messages = NULL;
    if (rc.code == CC_OK && messages_text) {
        rc = cc_json_parse(messages_text, &messages);
    }
    free(messages_text);
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

/*
 * 解析 OpenAI 同步响应。
 *
 * 提取 choices[0].message.content、reasoning_content 和 tool_calls，填入 SDK 统一
 * cc_llm_response_t。API error object 会转换成 CC_ERR_MODEL。
 */
static cc_result_t openai_parse_response(
    void *self,
    const char *response_json,
    cc_llm_response_t *out_response
)
{
    (void)self;
    cc_llm_response_init(out_response);

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
                cc_llm_response_set_text(out_response, content);
            }

            const char *reasoning = cc_json_string_value(cc_json_object_get(message, "reasoning_content"));
            if (reasoning) out_response->reasoning_content = cc_strdup(reasoning);

            cc_json_value_t *tool_calls = cc_json_object_get(message, "tool_calls");
            if (tool_calls && cc_json_is_array(tool_calls) && cc_json_array_size(tool_calls) > 0) {
                int n = cc_json_array_size(tool_calls);
                for (int i = 0; i < n; i++) {
                    cc_json_value_t *tc = cc_json_array_get(tool_calls, i);
                    cc_json_value_t *func = cc_json_object_get(tc, "function");
                    cc_llm_response_add_tool_call(
                        out_response,
                        cc_json_string_value(cc_json_object_get(tc, "id")),
                        cc_json_string_value(cc_json_object_get(func, "name")),
                        cc_json_string_value(cc_json_object_get(func, "arguments")));
                }
            }
        }

        const char *finish = cc_json_string_value(cc_json_object_get(choice, "finish_reason"));
        out_response->finished = (finish && strcmp(finish, "stop") == 0) ? 1 : 0;
    }

    cc_json_destroy(root);
    return cc_result_ok();
}

/*
 * 解析 OpenAI SSE data 事件。
 *
 * delta.content 映射 TEXT，delta.reasoning_content 映射 THINKING，delta.tool_calls 会按
 * TOOL_START/TOOL_DELTA/TOOL_END 输出。函数不拥有 chunk 字符串，它们只在回调期间有效。
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

/* OpenAI 协议 vtable；destroy 为 NULL，因为该协议没有私有状态。 */
static cc_llm_protocol_vtable_t openai_protocol_vtable = {
    openai_name,
    openai_build_request,
    openai_parse_response,
    openai_parse_stream_event,
    NULL
};

/*
 * 创建 OpenAI provider。
 *
 * 这里把 OpenAI protocol 注入通用 HTTP provider；默认 base_url/model 只是 SDK 默认值，
 * 下游可通过配置覆盖。
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
