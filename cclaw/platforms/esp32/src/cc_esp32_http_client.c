/**
 * 学习导读：cclaw/platforms/esp32/src/cc_esp32_http_client.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */


#ifdef ESP_PLATFORM
#include "cc/ports/cc_http_client.h"
#include "cc/util/cc_memory.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include <stdlib.h>
#include <string.h>

typedef struct cc_esp32_http_ctx {
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    size_t max_response_bytes;
    cc_result_t callback_error;
} cc_esp32_http_ctx_t;

/* 学习注释：response_append 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：http_event_handler 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    cc_esp32_http_ctx_t *ctx = (cc_esp32_http_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;

    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }

    const char *data = (const char *)evt->data;
    size_t len = (size_t)evt->data_len;

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

/* 学习注释：cc_http_client_perform 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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

/* 学习注释：cc_http_response_free 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_http_response_free(cc_http_response_t *response)
{
    if (!response) return;
    free(response->body);
    memset(response, 0, sizeof(*response));
}

#endif
