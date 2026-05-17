/**
 * 学习导读：cclaw/adapters/src/llm/cc_http_llm_provider.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_memory.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

typedef struct cc_http_llm_provider {
    char *base_url;
    char *api_key;
    char *model;
    cc_llm_protocol_t protocol;
} cc_http_llm_provider_t;

/* 学习注释：cc_llm_http_request_cleanup 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_llm_http_request_cleanup(cc_llm_http_request_t *request)
{
    if (!request) return;
    free(request->url);
    free(request->api_key);
    free(request->body_json);
    for (size_t i = 0; i < request->header_count; i++) {
        free((char *)request->headers[i].name);
        free((char *)request->headers[i].value);
    }
    free(request->headers);
    memset(request, 0, sizeof(*request));
}

/* 学习注释：http_post_json_with_headers 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t http_post_json_with_headers(
    const char *url,
    const cc_http_header_t *headers,
    size_t header_count,
    const char *body_json,
    char **out_response
)
{
    cc_http_request_t http_request;
    memset(&http_request, 0, sizeof(http_request));
    http_request.method = "POST";
    http_request.url = url;
    http_request.headers = headers;
    http_request.header_count = header_count;
    http_request.body = body_json;
    http_request.timeout_ms = 120000;

    cc_http_response_t response;
    memset(&response, 0, sizeof(response));
    cc_result_t rc = cc_http_client_perform(&http_request, &response);
    if (rc.code != CC_OK) return rc;

    if (response.status_code < 200 || response.status_code >= 300) {
        cc_result_t err = cc_result_errf(CC_ERR_MODEL,
            "LLM HTTP %ld: %s",
            response.status_code,
            response.body ? response.body : "empty response body");
        cc_http_response_free(&response);
        return err;
    }

    *out_response = response.body ? response.body : cc_strdup("");
    response.body = NULL;
    cc_http_response_free(&response);
    return cc_result_ok();
}

typedef struct cc_http_llm_stream_ctx {
    cc_llm_protocol_t protocol;
    cc_llm_stream_kind_t stream_kind;
    cc_llm_stream_callback_fn on_chunk;
    void *user_data;
    char *buffer;
    size_t buf_len;
    size_t buf_cap;
    size_t total_bytes;
    int finished;
} cc_http_llm_stream_ctx_t;

/* 学习注释：stream_ctx_append 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t stream_ctx_append(
    cc_http_llm_stream_ctx_t *ctx,
    const char *data,
    size_t len
)
{
    if (len == 0) return cc_result_ok();
    if (ctx->buf_len + len >= ctx->buf_cap) {
        size_t new_cap = ctx->buf_cap ? ctx->buf_cap * 2 : 4096;
        while (new_cap < ctx->buf_len + len + 1) new_cap *= 2;
        char *new_buf = realloc(ctx->buffer, new_cap);
        if (!new_buf) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Stream buffer realloc failed");
        ctx->buffer = new_buf;
        ctx->buf_cap = new_cap;
    }
    memcpy(ctx->buffer + ctx->buf_len, data, len);
    ctx->buf_len += len;
    ctx->buffer[ctx->buf_len] = '\0';
    return cc_result_ok();
}

/* 学习注释：emit_finished_once 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t emit_finished_once(cc_http_llm_stream_ctx_t *ctx)
{
    if (ctx->finished) return cc_result_ok();
    ctx->finished = 1;
    cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_FINISHED, NULL, NULL, NULL };
    ctx->on_chunk(&chunk, ctx->user_data);
    return cc_result_ok();
}

/* 学习注释：dispatch_stream_event 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t dispatch_stream_event(
    cc_http_llm_stream_ctx_t *ctx,
    const char *event_json
)
{
    if (!event_json || !*event_json) return cc_result_ok();
    if (strcmp(event_json, "[DONE]") == 0) {
        return emit_finished_once(ctx);
    }
    if (!ctx->protocol.vtable->parse_stream_event) {
        return cc_result_error(CC_ERR_PLATFORM, "LLM protocol does not parse stream events");
    }

    int finished = 0;
    cc_result_t rc = ctx->protocol.vtable->parse_stream_event(
        ctx->protocol.self,
        event_json,
        ctx->on_chunk,
        ctx->user_data,
        &finished);
    if (rc.code != CC_OK) return rc;
    if (finished) return emit_finished_once(ctx);
    return cc_result_ok();
}

/* 学习注释：process_sse_event 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t process_sse_event(
    cc_http_llm_stream_ctx_t *ctx,
    char *event_text
)
{
    cc_string_builder_t data_json;
    cc_result_t rc = cc_string_builder_init(&data_json);
    if (rc.code != CC_OK) return rc;

    char *line = event_text;
    while (*line) {
        char *line_end = strchr(line, '\n');
        if (line_end) *line_end = '\0';
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        if (strncmp(line, "data:", 5) == 0) {
            const char *data = line + 5;
            if (*data == ' ') data++;
            if (data_json.length > 0) cc_string_builder_append(&data_json, "\n");
            rc = cc_string_builder_append(&data_json, data);
            if (rc.code != CC_OK) {
                cc_string_builder_deinit(&data_json);
                return rc;
            }
        }
        if (!line_end) break;
        line = line_end + 1;
    }

    rc = dispatch_stream_event(ctx, cc_string_builder_cstr(&data_json));
    cc_string_builder_deinit(&data_json);
    return rc;
}

/* 学习注释：process_sse_buffer 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t process_sse_buffer(cc_http_llm_stream_ctx_t *ctx)
{
    char *p = ctx->buffer;
    char *buf_end = ctx->buffer + ctx->buf_len;

    while (!ctx->finished && p < buf_end) {
        char *end = strstr(p, "\n\n");
        size_t sep_len = 2;
        if (!end) {
            end = strstr(p, "\r\n\r\n");
            sep_len = 4;
        }
        if (!end) break;

        char saved = *end;
        *end = '\0';
        cc_result_t rc = process_sse_event(ctx, p);
        *end = saved;
        if (rc.code != CC_OK) return rc;
        p = end + sep_len;
    }

    if (p > ctx->buffer) {
        size_t remaining = ctx->buf_len - (size_t)(p - ctx->buffer);
        if (remaining > 0) memmove(ctx->buffer, p, remaining);
        ctx->buf_len = remaining;
        ctx->buffer[ctx->buf_len] = '\0';
    }
    return cc_result_ok();
}

/* 学习注释：process_ndjson_buffer 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t process_ndjson_buffer(cc_http_llm_stream_ctx_t *ctx)
{
    char *p = ctx->buffer;
    char *buf_end = ctx->buffer + ctx->buf_len;

    while (!ctx->finished && p < buf_end) {
        char *end = strchr(p, '\n');
        if (!end) break;
        *end = '\0';
        size_t len = strlen(p);
        if (len > 0 && p[len - 1] == '\r') p[len - 1] = '\0';
        cc_result_t rc = dispatch_stream_event(ctx, p);
        *end = '\n';
        if (rc.code != CC_OK) return rc;
        p = end + 1;
    }

    if (p > ctx->buffer) {
        size_t remaining = ctx->buf_len - (size_t)(p - ctx->buffer);
        if (remaining > 0) memmove(ctx->buffer, p, remaining);
        ctx->buf_len = remaining;
        ctx->buffer[ctx->buf_len] = '\0';
    }
    return cc_result_ok();
}

/* 学习注释：stream_body_callback 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t stream_body_callback(const char *data, size_t len, void *user_data)
{
    cc_http_llm_stream_ctx_t *ctx = (cc_http_llm_stream_ctx_t *)user_data;
    if (ctx->finished) return cc_result_ok();
    ctx->total_bytes += len;

    cc_result_t rc = stream_ctx_append(ctx, data, len);
    if (rc.code != CC_OK) return rc;

    if (ctx->stream_kind == CC_LLM_STREAM_SSE) {
        return process_sse_buffer(ctx);
    }
    if (ctx->stream_kind == CC_LLM_STREAM_NDJSON) {
        return process_ndjson_buffer(ctx);
    }
    return cc_result_error(CC_ERR_PLATFORM, "Unsupported LLM stream framing");
}

/* 学习注释：flush_stream_tail 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t flush_stream_tail(cc_http_llm_stream_ctx_t *ctx)
{
    if (ctx->finished || ctx->buf_len == 0) return cc_result_ok();
    if (ctx->stream_kind == CC_LLM_STREAM_NDJSON) {
        cc_result_t rc = dispatch_stream_event(ctx, ctx->buffer);
        ctx->buf_len = 0;
        if (ctx->buffer) ctx->buffer[0] = '\0';
        return rc;
    }
    return cc_result_ok();
}

/* 学习注释：http_post_json_stream_with_headers 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t http_post_json_stream_with_headers(
    const cc_llm_http_request_t *http_req,
    cc_llm_protocol_t protocol,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    cc_http_llm_stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.protocol = protocol;
    ctx.stream_kind = http_req->stream_kind;
    ctx.on_chunk = on_chunk;
    ctx.user_data = user_data;

    cc_http_request_t http_request;
    memset(&http_request, 0, sizeof(http_request));
    http_request.method = "POST";
    http_request.url = http_req->url;
    http_request.headers = http_req->headers;
    http_request.header_count = http_req->header_count;
    http_request.body = http_req->body_json;
    http_request.timeout_ms = 300000;
    http_request.max_response_bytes = 0;
    http_request.on_body = stream_body_callback;
    http_request.user_data = &ctx;

    cc_http_response_t response;
    memset(&response, 0, sizeof(response));
    cc_result_t rc = cc_http_client_perform(&http_request, &response);
    if (rc.code == CC_OK) {
        rc = flush_stream_tail(&ctx);
    }

    if (rc.code == CC_OK && (response.status_code < 200 || response.status_code >= 300)) {
        rc = cc_result_errf(CC_ERR_MODEL,
            "LLM stream HTTP %ld: %s",
            response.status_code,
            response.body ? response.body : "empty response body");
    }

    if (rc.code == CC_OK) {
        rc = emit_finished_once(&ctx);
    }

    cc_http_response_free(&response);
    free(ctx.buffer);
    return rc;
}

/* 学习注释：http_llm_chat 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t http_llm_chat(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_response_t *out_response
)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    memset(out_response, 0, sizeof(*out_response));

    cc_llm_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    cc_result_t rc = provider->protocol.vtable->build_request(
        provider->protocol.self,
        provider->base_url,
        provider->api_key,
        provider->model,
        request,
        0,
        &http_req
    );
    if (rc.code != CC_OK) return rc;

    char *response_json = NULL;
    rc = http_post_json_with_headers(
        http_req.url,
        http_req.headers,
        http_req.header_count,
        http_req.body_json,
        &response_json
    );
    cc_llm_http_request_cleanup(&http_req);
    if (rc.code != CC_OK) return rc;

    rc = provider->protocol.vtable->parse_response(
        provider->protocol.self,
        response_json,
        out_response
    );
    free(response_json);
    return rc;
}

/* 学习注释：http_llm_chat_stream 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t http_llm_chat_stream(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    cc_llm_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    cc_result_t rc = provider->protocol.vtable->build_request(
        provider->protocol.self,
        provider->base_url,
        provider->api_key,
        provider->model,
        request,
        1,
        &http_req
    );
    if (rc.code != CC_OK) return rc;

    if (http_req.stream_kind == CC_LLM_STREAM_NONE ||
        !provider->protocol.vtable->parse_stream_event) {
        cc_llm_http_request_cleanup(&http_req);
        return cc_result_error(CC_ERR_PLATFORM, "LLM protocol does not support streaming");
    }

    rc = http_post_json_stream_with_headers(
        &http_req,
        provider->protocol,
        on_chunk,
        user_data);
    cc_llm_http_request_cleanup(&http_req);
    return rc;
}

/* 学习注释：http_llm_destroy 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void http_llm_destroy(void *self)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    if (!provider) return;
    free(provider->base_url);
    free(provider->api_key);
    free(provider->model);
    if (provider->protocol.vtable && provider->protocol.vtable->destroy) {
        provider->protocol.vtable->destroy(provider->protocol.self);
    }
    free(provider);
}

static cc_llm_provider_vtable_t http_llm_vtable = {
    http_llm_chat,
    http_llm_chat_stream,
    http_llm_destroy
};

/* 学习注释：cc_http_llm_provider_create 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_result_t cc_http_llm_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_protocol_t protocol,
    cc_llm_provider_t *out_provider
)
{
    if (!protocol.vtable || !protocol.vtable->build_request ||
        !protocol.vtable->parse_response || !out_provider) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP LLM provider argument");
    }

    cc_http_llm_provider_t *provider = calloc(1, sizeof(*provider));
    if (!provider) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create HTTP LLM provider");
    }

    provider->base_url = base_url ? cc_strdup(base_url) : cc_strdup("");
    provider->api_key = api_key ? cc_strdup(api_key) : NULL;
    provider->model = model ? cc_strdup(model) : cc_strdup("");
    provider->protocol = protocol;

    if (!provider->base_url || !provider->model) {
        http_llm_destroy(provider);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP LLM provider config");
    }

    out_provider->self = provider;
    out_provider->vtable = &http_llm_vtable;
    return cc_result_ok();
}
