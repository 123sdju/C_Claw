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

/**
 * cc_http_header — 单个 HTTP header 名值对。
 *
 * 该结构体不拥有字符串；request 发送期间调用方必须保证 name/value 有效。
 */
typedef struct cc_http_header {
    /** header 名称，例如 "Content-Type"。 */
    const char *name;
    /** header 值，例如 "application/json"。 */
    const char *value;
} cc_http_header_t;

/**
 * cc_http_body_callback_fn — 流式响应体回调。
 *
 * HTTP client 每收到一段 body 就调用该回调。data 只在回调期间有效；
 * user_data 原样来自 cc_http_request_t。
 *
 * @param data 借用的响应体片段，不保证以 '\0' 结尾。
 * @param len data 的字节数。
 * @param user_data 调用方上下文；HTTP client 只透传。
 * @return CC_OK 表示继续接收；失败返回错误码并中止传输。
 */
typedef cc_result_t (*cc_http_body_callback_fn)(
    const char *data,
    size_t len,
    void *user_data
);

/**
 * cc_http_request — 请求数据结构，字段通常由调用方填充，字符串/数组所有权按对应 API 注释管理。
 *
 * 所有字段均为借用输入；cc_http_client_perform 不保存 request 指针。
 * 如果 on_body 非 NULL，响应体会以回调形式交付，out_response->body 通常为空。
 */
typedef struct cc_http_request {
    /** HTTP 方法；当前调用点主要使用 "POST"，也可扩展为 "GET" 等。 */
    const char *method;
    /** 完整请求 URL；调用期间保持有效。 */
    const char *url;
    /** 借用的 header 数组；可为 NULL。 */
    const cc_http_header_t *headers;
    /** headers 的元素数量。 */
    size_t header_count;
    /** 请求体字符串；无 body 时可为 NULL。 */
    const char *body;
    /** 请求超时毫秒数；0 表示平台默认超时。 */
    long timeout_ms;
    /** 最大可接受响应体字节数；0 表示不由端口层限制。 */
    size_t max_response_bytes;
    /** 可选响应体回调；用于流式 LLM 响应。 */
    cc_http_body_callback_fn on_body;
    /** 传给 on_body 的调用方上下文。 */
    void *user_data;
} cc_http_request_t;

/**
 * cc_http_response — 结果数据结构，承载调用输出和错误信息；其中堆字符串需要由对应 free 函数释放。
 *
 * 非流式请求成功后 body 由 HTTP client 分配，调用方必须用
 * cc_http_response_free 清理；结构体本身通常在调用方栈上。
 */
typedef struct cc_http_response {
    /** HTTP 状态码；传输层失败时可能保持为 0。 */
    long status_code;
    /** 堆上分配的响应体，可能为 NULL；由 cc_http_response_free 释放。 */
    char *body;
    /** body 的字节数，不含结尾 '\0'。 */
    size_t body_size;
} cc_http_response_t;

/**
 * cc_http_client_perform — 执行一次 HTTP 请求并返回状态码与响应体。
 *
 * 平台实现负责把 libcurl、ESP-IDF 或其他网络栈错误映射为 cc_result_t。
 * request 只在本次调用期间借用；成功后 out_response 中的 body 由调用方释放。
 *
 * @param request 借用请求对象；不可为 NULL。
 * @param out_response 输出响应；成功时写入 status_code/body/body_size。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_http_client_perform(
    const cc_http_request_t *request,
    cc_http_response_t *out_response
);

/**
 * cc_http_response_free — 释放结果结构体内部由平台层分配的缓冲区，并把大小/指针复位。
 *
 * 只释放 response->body，不释放 response 结构体本身；传入 NULL 安全。
 *
 * @param response 要清理的响应对象。
 */
void cc_http_response_free(cc_http_response_t *response);

#endif
