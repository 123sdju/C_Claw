/**
 * 学习导读：cclaw/adapters/include/cc/adapters/cc_http_llm_provider.h
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#ifndef CC_HTTP_LLM_PROVIDER_H
#define CC_HTTP_LLM_PROVIDER_H

#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_http_client.h"

typedef enum cc_llm_stream_kind {
    CC_LLM_STREAM_NONE = 0,
    CC_LLM_STREAM_SSE = 1,
    CC_LLM_STREAM_NDJSON = 2
} cc_llm_stream_kind_t;

typedef struct cc_llm_http_request {
    char *url;
    char *api_key;
    char *body_json;
    cc_http_header_t *headers;
    size_t header_count;
    cc_llm_stream_kind_t stream_kind;
} cc_llm_http_request_t;

typedef struct cc_llm_protocol cc_llm_protocol_t;
typedef struct cc_llm_protocol_vtable cc_llm_protocol_vtable_t;

struct cc_llm_protocol {
    void *self;
    const cc_llm_protocol_vtable_t *vtable;
};

struct cc_llm_protocol_vtable {
    const char *(*name)(void *self);
    cc_result_t (*build_request)(
        void *self,
        const char *base_url,
        const char *api_key,
        const char *default_model,
        const cc_llm_chat_request_t *request,
        int stream,
        cc_llm_http_request_t *out_request
    );
    cc_result_t (*parse_response)(
        void *self,
        const char *response_json,
        cc_llm_response_t *out_response
    );
    cc_result_t (*parse_stream_event)(
        void *self,
        const char *event_json,
        cc_llm_stream_callback_fn on_chunk,
        void *user_data,
        int *out_finished
    );
    void (*destroy)(void *self);
};

void cc_llm_http_request_cleanup(cc_llm_http_request_t *request);

cc_result_t cc_http_llm_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_protocol_t protocol,
    cc_llm_provider_t *out_provider
);

#endif
