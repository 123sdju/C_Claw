/**
 * 学习导读：cclaw/platforms/esp32/src/cc_esp32_http_client.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_esp32_http_client.c — 基于 ESP-IDF esp_http_client 的 HTTP 客户端实现
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 ESP32 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_http_client.h 端口接口在 ESP32（ESP-IDF）平台的具体实现，
 *   基于 esp_http_client 库提供 HTTP/HTTPS 客户端能力。TLS 通过 esp_crt_bundle_attach
 *   绑定 Mozilla CA 证书包，无需手动管理证书文件。向上层（如 LLM provider、
 *   MCP transport 模块）提供统一的 cc_http_client_perform() / cc_http_response_free()
 *   接口，适合在内存受限的 IoT 设备上运行。
 *
 * 事件驱动回调架构：
 *   与 POSIX/libcurl 版本不同，ESP-IDF esp_http_client 通过单一事件处理函数分发数据：
 *     - HTTP_EVENT_ON_HEADER：逐对接收响应头键值对（evt->header_key/header_value）
 *     - HTTP_EVENT_ON_DATA：接收响应体分块，传递给 on_body 或累积到响应体
 *   cc_esp32_http_ctx_t 回调上下文集成了取消令牌检查、流式回调转发和错误累积。
 *
 * HTTP 方法支持：
 *   - GET / POST：原生支持（esp_http_client_set_method）
 *   - 其他方法：通过 X-HTTP-Method-Override header 兜底（POST + header）
 *
 * 与 POSIX/libcurl 版本的关键区别：
 *   - 使用 esp_http_client 事件回调而非 libcurl WRITEFUNCTION/HEADERFUNCTION
 *   - 事件处理函数返回 ESP_FAIL 中止传输（等价于 libcurl 返回 0）
 *   - 请求体通过 esp_http_client_set_post_field 设置（固定长度字符串）
 *   - 无进度回调（xferinfo），取消只在事件回调中检查
 *   - buffer_size 固定为 2048 字节（ESP32 内存受限环境）
 *   - response_header_append 直接从键值对拼接，无需解析 "Name: Value" 行
 */

#ifdef ESP_PLATFORM
#include "cc/ports/cc_http_client.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/util/cc_memory.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include <stdlib.h>
#include <string.h>

/**
 * cc_esp32_http_ctx — ESP-IDF HTTP event 回调上下文。
 *
 * ESP HTTP client 通过事件分发 header/body chunk。这里沿用 port 层语义：
 * 非流式调用累积 body，流式调用把 chunk 交给 on_body；callback_error 用来把
 * 取消、SSE parser 错误或大小限制从事件回调带回 perform 调用点。
 */
typedef struct cc_esp32_http_ctx {
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    size_t max_response_bytes;
    cc_cancel_token_t *cancel_token;
    cc_result_t callback_error;
} cc_esp32_http_ctx_t;

/**
 * response_append — 在设备端累积非流式响应体。
 *
 * ESP profile 也需要普通 JSON response（例如 MCP HTTP JSON 或 real chat provider）。
 * 为了和 POSIX 行为一致，body 始终保持 '\0' 结尾；超过 max_response_bytes 时
 * 返回失败，让 perform 报出可读错误。
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

/**
 * http_event_handler — 处理 ESP-IDF HTTP client 事件，把响应片段追加到请求上下文。
 *
 * @return 返回 esp_err_t 类型结果，供当前调用链继续判断。
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

/**
 * cc_http_client_perform — 执行一次平台 HTTP 请求，填充状态码和响应体或触发流式回调。
 *
 * @param request 借用的对象；函数不释放该对象本身。
 * @param out_response 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
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

/**
 * cc_http_response_free — 释放结果结构体内部由平台层分配的缓冲区，并把大小/指针复位。
 *
 * @param response 借用的对象；函数不释放该对象本身。
 */
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
