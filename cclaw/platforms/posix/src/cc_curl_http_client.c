



#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_platform.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <string.h>

#if CC_HAS_CURL
#include <curl/curl.h>
#endif

/*
 * curl 写回调上下文。
 *
 * response 用于累计 body/header；on_body 非空时支持流式消费；callback_error 保存回调中
 * 发生的 SDK 错误，curl 回调只能用 size_t 表示停止，真正错误需要通过该字段带出。
 */
typedef struct cc_curl_write_ctx {
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    size_t max_response_bytes;
    cc_cancel_token_t *cancel_token;


    cc_result_t callback_error;
} cc_curl_write_ctx_t;

/*
 * 将 body 数据追加到 response。
 *
 * max_response_bytes > 0 时执行最大响应限制；返回 0 会让 curl 中止传输，外层再把原因
 * 映射成 cc_result_t。
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

/* 复制并去掉 header name/value 两端空白和 CRLF。 */
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

/*
 * 解析并保存一行 HTTP 响应头。
 *
 * curl 会把状态行也交给 header callback；没有冒号的行直接忽略。返回 header 字符串由
 * cc_http_response_free 释放。
 */
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

/*
 * curl body 回调。
 *
 * 支持三种模式：取消 token 触发中止、on_body 流式消费、或把 body 缓存到 response。
 * provider stream 路径会使用 on_body 避免把完整响应一次性放进内存。
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

/*
 * curl header 回调。
 *
 * 保存响应头，供 HTTP provider 读取 Retry-After 等结构化错误信息。
 */
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

/*
 * curl 进度回调。
 *
 * 即使服务器暂时没有 body 数据，也能周期性检查 cancel token，使长请求可以被 runtime
 * 及时取消。
 */
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

/*
 * 把 SDK header 转成 curl_slist 行。
 *
 * curl_slist_append 会复制行内容，因此 builder 生成的临时字符串可立即释放。
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

/*
 * 执行 HTTP 请求。
 *
 * POSIX profile 使用 libcurl；没有 curl 时返回 CC_ERR_PLATFORM。函数支持普通缓冲响应和
 * 流式 body 回调，timeout/cancel/max_response_bytes 都在这一层落地。
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

/*
 * 释放 HTTP 响应。
 *
 * headers/body 都由 HTTP client 分配，provider/tool 处理完响应后必须调用本函数。
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
