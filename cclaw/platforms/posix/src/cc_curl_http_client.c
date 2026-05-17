/**
 * 学习导读：cclaw/platforms/posix/src/cc_curl_http_client.c
 *
 * 所属层次：平台层。
 * 阅读重点：这里隐藏 POSIX、Windows、ESP32 的系统 API 差异，阅读时重点看同名端口函数如何按平台实现。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/ports/cc_http_client.h"
#include "cc/ports/cc_platform.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <string.h>

#if CC_HAS_CURL
#include <curl/curl.h>
#endif

typedef struct cc_curl_write_ctx {
    cc_http_response_t *response;
    cc_http_body_callback_fn on_body;
    void *user_data;
    size_t max_response_bytes;
} cc_curl_write_ctx_t;

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

#if CC_HAS_CURL
/* 学习注释：cc_curl_write_body 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
static size_t cc_curl_write_body(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t real_size = size * nmemb;
    cc_curl_write_ctx_t *ctx = (cc_curl_write_ctx_t *)userp;

    if (ctx->on_body) {
        cc_result_t rc = ctx->on_body((const char *)contents, real_size, ctx->user_data);
        if (rc.code != CC_OK) {
            cc_result_free(&rc);
            return 0;
        }
    }

    if (!ctx->on_body || ctx->max_response_bytes > 0) {
        if (!response_append(ctx->response, (const char *)contents, real_size,
                             ctx->max_response_bytes)) {
            return 0;
        }
    }

    return real_size;
}

/* 学习注释：append_header 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cc_curl_write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out_response->status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

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

/* 学习注释：cc_http_response_free 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
void cc_http_response_free(cc_http_response_t *response)
{
    if (!response) return;
    free(response->body);
    memset(response, 0, sizeof(*response));
}
