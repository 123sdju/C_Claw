



#ifdef ESP_PLATFORM
#include "cc/ports/cc_http_client.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/util/cc_memory.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include <stdlib.h>
#include <string.h>

/*
 * ESP32 HTTP 事件上下文。
 *
 * esp_http_client 通过事件回调交付 header/body；ctx 保存 response、stream 回调、最大 body
 * 限制和取消 token，并用 callback_error 把 SDK 错误从 ESP 回调带回主流程。
 */
typedef struct cc_esp32_http_ctx {
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    size_t max_response_bytes;
    cc_cancel_token_t *cancel_token;
    cc_result_t callback_error;
} cc_esp32_http_ctx_t;

/*
 * 追加响应 body。
 *
 * max_response_bytes 用于 MCU RAM 保护；超过限制或 realloc 失败返回 0，事件回调会中止请求。
 */
static int response_append(
    cc_http_response_t *response,
    const char *data,
    size_t len,
    size_t max_response_bytes
)
{
    if (!response || len == 0) return 1;
    if (max_response_bytes > 0 && response->body_size + len > max_response_bytes) {
        return 0;
    }

    char *next = realloc(response->body, response->body_size + len + 1);
    if (!next) return 0;

    response->body = next;
    memcpy(response->body + response->body_size, data, len);
    response->body_size += len;
    response->body[response->body_size] = '\0';
    return 1;
}

/*
 * 保存一个响应头。
 *
 * header name/value 都深拷贝到 response，供 provider 错误分类读取 Retry-After 等字段。
 */
static cc_result_t response_header_append(
    cc_http_response_t *response,
    const char *name,
    const char *value
)
{
    if (!response || !name || !value) return cc_result_ok();
    cc_http_header_t *next = realloc(
        response->headers,
        (response->header_count + 1) * sizeof(*response->headers));
    if (!next) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow HTTP response headers");
    }
    response->headers = next;
    response->headers[response->header_count].name = cc_strdup(name);
    response->headers[response->header_count].value = cc_strdup(value);
    if (!response->headers[response->header_count].name ||
        !response->headers[response->header_count].value) {
        free((char *)response->headers[response->header_count].name);
        free((char *)response->headers[response->header_count].value);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP response header");
    }
    response->header_count++;
    return cc_result_ok();
}

/*
 * ESP-IDF HTTP event handler。
 *
 * ON_HEADER 保存 header，ON_DATA 可走 stream 回调或 body 缓冲；每次事件都检查 cancel token，
 * 让长请求可以被 runtime 取消。
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    cc_esp32_http_ctx_t *ctx = (cc_esp32_http_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
            return ESP_FAIL;
        }
        ctx->callback_error = response_header_append(ctx->response, evt->header_key, evt->header_value);
        return ctx->callback_error.code == CC_OK ? ESP_OK : ESP_FAIL;
    }

    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    const char *data = (const char *)evt->data;
    size_t len = (size_t)evt->data_len;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        return ESP_FAIL;
    }

    if (ctx->on_body) {
        ctx->callback_error = ctx->on_body(data, len, ctx->user_data);
        if (ctx->callback_error.code != CC_OK) return ESP_FAIL;
    }

    if (!ctx->on_body || ctx->max_response_bytes > 0) {
        if (!response_append(ctx->response, data, len, ctx->max_response_bytes)) {
            ctx->callback_error = cc_result_error(CC_ERR_OUT_OF_MEMORY, "HTTP response buffer full");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

/*
 * 执行 ESP32 HTTP 请求。
 *
 * 使用 esp_http_client 和证书 bundle；POST/GET 原生支持，其它 method 用
 * X-HTTP-Method-Override 退化。成功后 out_response 由调用方 cc_http_response_free。
 */
cc_result_t cc_http_client_perform(
    const cc_http_request_t *request,
    cc_http_response_t *out_response
)
{
    if (!request || !request->url || !out_response) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP request");
    }

    memset(out_response, 0, sizeof(*out_response));

    cc_esp32_http_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.response = out_response;
    ctx.on_body = request->on_body;
    ctx.user_data = request->user_data;
    ctx.max_response_bytes = request->max_response_bytes;
    ctx.cancel_token = request->cancel_token;

    if (cc_cancel_token_is_cancelled(request->cancel_token)) {
        return cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled before start");
    }

    esp_http_client_config_t config = {
        .url = request->url,
        .timeout_ms = request->timeout_ms > 0 ? (int)request->timeout_ms : 120000,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return cc_result_error(CC_ERR_NETWORK, "Failed to initialize esp_http_client");

    const char *method = request->method ? request->method : "GET";
    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcmp(method, "GET") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "X-HTTP-Method-Override", method);
    }

    for (size_t i = 0; i < request->header_count; i++) {
        if (request->headers[i].name && request->headers[i].value) {
            esp_http_client_set_header(client, request->headers[i].name, request->headers[i].value);
        }
    }

    if (request->body) {
        esp_http_client_set_post_field(client, request->body, (int)strlen(request->body));
    }

    esp_err_t err = esp_http_client_perform(client);
    out_response->status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ctx.callback_error.code != CC_OK) {
        cc_http_response_free(out_response);
        return ctx.callback_error;
    }

    if (err != ESP_OK) {
        cc_http_response_free(out_response);
        return cc_result_error(CC_ERR_NETWORK, esp_err_to_name(err));
    }

    if (!out_response->body && !request->on_body) {
        out_response->body = cc_strdup("");
        if (!out_response->body) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate empty HTTP body");
        }
    }

    return cc_result_ok();
}

/* 释放 ESP32 HTTP response 的 headers/body。 */
void cc_http_response_free(cc_http_response_t *response)
{
    if (!response) return;
    for (size_t i = 0; i < response->header_count; i++) {
        free((char *)response->headers[i].name);
        free((char *)response->headers[i].value);
    }
    free(response->headers);
    free(response->body);
    memset(response, 0, sizeof(*response));
}

#endif
