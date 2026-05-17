/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_http_client.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_http_client.h — HTTP client port
 *
 * This port keeps network transport behind one small interface so LLM
 * providers and tools do not depend directly on libcurl or a device SDK.
 */

#ifndef CC_HTTP_CLIENT_H
#define CC_HTTP_CLIENT_H

#include "cc/core/cc_result.h"
#include <stddef.h>

typedef struct cc_http_header {
    const char *name;
    const char *value;
} cc_http_header_t;

typedef cc_result_t (*cc_http_body_callback_fn)(
    const char *data,
    size_t len,
    void *user_data
);

typedef struct cc_http_request {
    const char *method;
    const char *url;
    const cc_http_header_t *headers;
    size_t header_count;
    const char *body;
    long timeout_ms;
    size_t max_response_bytes;
    cc_http_body_callback_fn on_body;
    void *user_data;
} cc_http_request_t;

typedef struct cc_http_response {
    long status_code;
    char *body;
    size_t body_size;
} cc_http_response_t;

cc_result_t cc_http_client_perform(
    const cc_http_request_t *request,
    cc_http_response_t *out_response
);

void cc_http_response_free(cc_http_response_t *response);

#endif
