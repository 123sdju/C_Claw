



#ifndef CC_HTTP_CLIENT_H
#define CC_HTTP_CLIENT_H

#include "cc/core/cc_result.h"
#include <stddef.h>

/* cancel token 前置声明，HTTP port 只借用指针以支持请求取消。 */
typedef struct cc_cancel_token cc_cancel_token_t;

/*
 * HTTP header 键值对。
 *
 * 在 request 中 header 字符串由调用方借用；在 response 中 header 数组和字符串由
 * response 拥有，并通过 cc_http_response_free() 释放。
 */
typedef struct cc_http_header {

    const char *name;

    const char *value;
} cc_http_header_t;

/*
 * 流式 body 回调。
 *
 * data 只在回调期间有效；返回非 OK 表示停止接收并把错误传回 HTTP 调用方。平台实现
 * 应遵守 max_response_bytes 和 cancel_token，避免大响应耗尽内存。
 */
typedef cc_result_t (*cc_http_body_callback_fn)(
    const char *data,
    size_t len,
    void *user_data
);

/*
 * HTTP 请求描述。
 *
 * 所有字符串和 header 数组都是借用指针，只需在 cc_http_client_perform() 调用期间有效。
 * 如果 on_body 非 NULL，平台实现可以流式回调 body；否则通常把响应体放入 out_response。
 * 自动 redirect 必须重新校验 network allowlist，不能绕过 SDK 安全策略。
 */
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

    cc_cancel_token_t *cancel_token;
} cc_http_request_t;

/*
 * HTTP 响应。
 *
 * status_code 是服务器状态码；headers/body 由平台实现分配，调用方用
 * cc_http_response_free() 清理。body_size 用于安全处理二进制或非 NUL 结尾数据。
 */
typedef struct cc_http_response {

    long status_code;


    cc_http_header_t *headers;

    size_t header_count;

    char *body;

    size_t body_size;
} cc_http_response_t;

/*
 * 执行 HTTP 请求。
 *
 * 这是平台 port 的统一入口，POSIX/Windows/ESP32/FreeRTOS 可分别实现。错误应映射为
 * CC_ERR_NETWORK、CC_ERR_TIMEOUT、CC_ERR_CANCELLED、CC_ERR_LIMIT_EXCEEDED 等稳定结果。
 */
cc_result_t cc_http_client_perform(
    const cc_http_request_t *request,
    cc_http_response_t *out_response
);

/* 释放 HTTP 响应拥有的 headers/body；不释放 response 指针本身。 */
void cc_http_response_free(cc_http_response_t *response);

#endif
