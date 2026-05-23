/**
 * 学习导读：cclaw/platforms/windows/src/cc_curl_http_client.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_curl_http_client.c — 基于 libcurl 的 HTTP 客户端实现（Windows 平台）
 *
 * 在整体架构中的角色和层次：
 *   本模块位于 Platform 层的 Windows 平台实现子层。
 *   Platform 层是整个系统的最底层，负责封装操作系统差异。
 *   本文件是 cc_http_client.h 端口接口在 Windows 平台的具体实现，
 *   基于 libcurl 提供完整的 HTTP/HTTPS 客户端能力。
 *   实现逻辑与 POSIX 版本的 cc_curl_http_client.c 几乎完全一致——
 *   两者共享相同的 libcurl 回调架构（WRITEFUNCTION/HEADERFUNCTION/XFERINFOFUNCTION），
 *   仅在头文件引入和平台宏方面有所区别。
 *
 * libcurl 回调驱动架构：
 *   通过 libcurl 的三个回调深度集成取消令牌和流式处理：
 *     - cc_curl_write_body：累积响应体（非流式）或传递给 on_body（流式/SSE）
 *     - cc_curl_write_header：逐行解析 HTTP 响应头
 *     - cc_curl_progress：传输进度回调，轮询 cancel_token 实现中途取消
 *   callback_error 字段保存回调错误码，使取消和解析失败可精确传递到调用方。
 *
 * 与 POSIX 版本的关系：
 *   Windows 和 POSIX 版本共享相同的 libcurl 逻辑，代码结构一致。
 *   这确保两个平台的 HTTP 行为（SSL/TLS 证书验证、超时、取消语义、响应解析）
 *   完全统一，上层代码无需关注运行平台。
 */

#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_platform.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <string.h>

#if CC_HAS_CURL
#include <curl/curl.h>
#endif

/**
 * cc_curl_write_ctx — libcurl body/header 回调共享上下文。
 *
 * 非流式调用把 body 累积到 response->body；流式调用把每个 chunk 交给 on_body，
 * 用于 LLM streaming、MCP SSE 和 streamable HTTP。callback_error 保存回调返回的
 * cc_result_t，让 CURLE_WRITE_ERROR 可以还原成取消、解析失败或大小限制。
 */
typedef struct cc_curl_write_ctx {
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    size_t max_response_bytes;
    cc_cancel_token_t *cancel_token;
    /*
     * libcurl collapses callback aborts into CURLE_WRITE_ERROR. Preserving
     * the original result keeps cancellation/parse failures visible to the
     * SDK layer instead of turning them into anonymous network errors.
     */
    cc_result_t callback_error;
} cc_curl_write_ctx_t;

/**
 * response_append — 累积非流式响应体。
 *
 * 只在 request.on_body 为 NULL 时使用。函数维护 response->body 的 '\0' 结尾，
 * 方便上层把 HTTP body 当 C 字符串解析 JSON；超过 max_response_bytes 或 OOM
 * 返回 0，由 libcurl 回调路径转换成 cc_result_t。
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

static char *copy_trimmed_header_piece(const char *data, size_t len)
{
    while (len > 0 && (*data == ' ' || *data == '\t' || *data == '\r' || *data == '\n')) {
        data++;
        len--;
    }
    while (len > 0) {
        char ch = data[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        len--;
    }

    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

static cc_result_t response_header_append(
    cc_http_response_t *response,
    const char *line,
    size_t len
)
{
    if (!response || !line || len == 0) return cc_result_ok();

    const char *colon = memchr(line, ':', len);
    if (!colon) return cc_result_ok();

    size_t name_len = (size_t)(colon - line);
    size_t value_len = len - name_len - 1;
    char *name = copy_trimmed_header_piece(line, name_len);
    char *value = copy_trimmed_header_piece(colon + 1, value_len);
    if (!name || !value) {
        free(name);
        free(value);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP response header");
    }

    cc_http_header_t *next = realloc(
        response->headers,
        (response->header_count + 1) * sizeof(*response->headers));
    if (!next) {
        free(name);
        free(value);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to grow HTTP response headers");
    }

    response->headers = next;
    response->headers[response->header_count].name = name;
    response->headers[response->header_count].value = value;
    response->header_count++;
    return cc_result_ok();
}

#if CC_HAS_CURL
/**
 * cc_curl_write_body — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * @param size 按值传入，用于控制本次操作。
 * @param nmemb 按值传入，用于控制本次操作。
 * @return 返回字节数、元素数或下标；不会转移任何指针所有权。
 */
static size_t cc_curl_write_body(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size = size * nmemb;
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)userp;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        if (ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        return 0;
    }

    if (ctx->on_body) {
        cc_result_t rc = ctx->on_body((const char *)contents, real_size, ctx->user_data);
        if (rc.code != CC_OK) {
            if (ctx->callback_error.code == CC_OK) ctx->callback_error = rc;
            else cc_result_free(&rc);
            return 0;
        }
    }

    if (!ctx->on_body || ctx->max_response_bytes > 0) {
        if (!response_append(ctx->response, (const char *)contents, real_size,
                             ctx->max_response_bytes)) {
            if (ctx->callback_error.code == CC_OK) {
                ctx->callback_error = cc_result_error(
                    ctx->max_response_bytes > 0 ? CC_ERR_INVALID_ARGUMENT : CC_ERR_OUT_OF_MEMORY,
                    "HTTP response body could not be buffered");
            }
            return 0;
        }
    }

    return real_size;
}

static size_t cc_curl_write_header(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size = size * nmemb;
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)userp;
    if (cc_cancel_token_is_cancelled(ctx->cancel_token)) {
        if (ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        return 0;
    }
    cc_result_t rc = response_header_append(ctx->response, (const char *)contents, real_size);
    if (rc.code != CC_OK) {
        if (ctx->callback_error.code == CC_OK) ctx->callback_error = rc;
        else cc_result_free(&rc);
        return 0;
    }
    return real_size;
}

static int cc_curl_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)clientp;
    if (cc_cancel_token_is_cancelled(ctx ? ctx->cancel_token : NULL)) {
        if (ctx && ctx->callback_error.code == CC_OK) {
            ctx->callback_error = cc_result_error(CC_ERR_CANCELLED, "HTTP request cancelled");
        }
        return 1;
    }
    return 0;
}

/**
 * append_header — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * @param headers 输出参数；调用方传入有效指针，成功后接收结果。
 * @param name 借用的只读字符串；函数不会释放该指针。
 * @param value 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t append_header(struct curl_slist **headers, const char *name, const char *value)
{
    if (!name || !value) return cc_result_ok();

    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) return rc;

    rc = cc_string_builder_appendf(&sb, "%s: %s", name, value);
    if (rc.code != CC_OK) {
        cc_string_builder_deinit(&sb);
        return rc;
    }

    char *line = cc_string_builder_take(&sb);
    struct curl_slist *next = curl_slist_append(*headers, line);
    free(line);

    if (!next) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append HTTP header");
    *headers = next;
    return cc_result_ok();
}
#endif

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

#if !CC_HAS_CURL
    (void)request;
    return cc_result_error(CC_ERR_PLATFORM, "No HTTP client adapter available in this build");
#else
    CURL *curl = curl_easy_init();
    if (!curl) return cc_result_error(CC_ERR_NETWORK, "Failed to initialize curl");

    struct curl_slist *headers = NULL;
    for (size_t i = 0; i < request->header_count; i++) {
        cc_result_t rc = append_header(&headers, request->headers[i].name, request->headers[i].value);
        if (rc.code != CC_OK) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return rc;
        }
    }

    const char *method = request->method ? request->method : "GET";
    curl_easy_setopt(curl, CURLOPT_URL, request->url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
        request->timeout_ms > 0 ? request->timeout_ms : 120000L);

    if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body ? request->body : "");
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        if (request->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
        }
    }

    cc_curl_write_ctx_t write_ctx;
    memset(&write_ctx, 0, sizeof(write_ctx));
    write_ctx.response = out_response;
    write_ctx.on_body = request->on_body;
    write_ctx.user_data = request->user_data;
    write_ctx.max_response_bytes = request->max_response_bytes;
    write_ctx.cancel_token = request->cancel_token;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cc_curl_write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, cc_curl_write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cc_curl_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &write_ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out_response->status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (write_ctx.callback_error.code != CC_OK) {
        cc_result_t rc = write_ctx.callback_error;
        memset(&write_ctx.callback_error, 0, sizeof(write_ctx.callback_error));
        cc_http_response_free(out_response);
        return rc;
    }

    if (res != CURLE_OK) {
        cc_http_response_free(out_response);
        return cc_result_error(CC_ERR_NETWORK, curl_easy_strerror(res));
    }

    if (!out_response->body) {
        out_response->body = strdup("");
        if (!out_response->body) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate empty HTTP body");
        }
    }

    return cc_result_ok();
#endif
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
