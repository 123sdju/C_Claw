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

/**
 * cc_llm_stream_kind — HTTP LLM 后端的流式响应协议类型。
 *
 * OpenAI/Anthropic 通常使用 SSE，Ollama 常见为按行 NDJSON。统一枚举后，
 * cc_http_llm_provider 可以用同一套传输逻辑分派不同解析器。
 */
typedef enum cc_llm_stream_kind {
    /** 不启用流式解析，按完整 HTTP 响应体解析。 */
    CC_LLM_STREAM_NONE = 0,
    /** Server-Sent Events，每个事件中包含一段 JSON payload。 */
    CC_LLM_STREAM_SSE = 1,
    /** 换行分隔 JSON，每行是一段流式事件。 */
    CC_LLM_STREAM_NDJSON = 2
} cc_llm_stream_kind_t;

/**
 * cc_llm_http_request — 请求数据结构，字段通常由调用方填充，字符串/数组所有权按对应 API 注释管理。
 *
 * 协议适配器 build_request 会分配并填充这些字段，HTTP provider 发送请求后
 * 统一调用 cc_llm_http_request_cleanup 释放。调用方不应把字段指针保存到
 * cleanup 之后继续使用。
 */
typedef struct cc_llm_http_request {
    /** 目标 URL，拥有字符串；cleanup 释放。 */
    char *url;
    /** API key，拥有字符串；某些本地 provider 可为空。 */
    char *api_key;
    /** JSON 请求体，拥有字符串；发送后 cleanup 释放。 */
    char *body_json;
    /** HTTP header 数组，拥有数组及元素内容；长度由 header_count 给出。 */
    cc_http_header_t *headers;
    /** headers 的元素数量。 */
    size_t header_count;
    /** 响应流格式；决定 stream 分片解析方式。 */
    cc_llm_stream_kind_t stream_kind;
} cc_llm_http_request_t;

/**
 * cc_llm_protocol — LLM HTTP provider 使用的协议策略对象，保存协议私有数据和 vtable。
 *
 * 该对象把“怎么构造请求、怎么解析响应”的模型厂商差异从 HTTP 传输层拆开。
 * cc_http_llm_provider_create 会接管 protocol.self，并在 provider destroy 时
 * 调用 protocol.vtable->destroy。
 */
typedef struct cc_llm_protocol cc_llm_protocol_t;
/**
 * cc_llm_protocol_vtable — 虚函数表，列出该端口/协议必须实现的回调；调用方只通过这些函数指针访问具体实现。
 *
 * vtable 通常是静态常量；每个回调的 self 参数来自 cc_llm_protocol.self。
 */
typedef struct cc_llm_protocol_vtable cc_llm_protocol_vtable_t;

/**
 * cc_llm_protocol — LLM HTTP provider 使用的协议策略对象，保存协议私有数据和 vtable。
 *
 * self 的实际类型由 OpenAI/Anthropic/Ollama 等具体协议实现决定。
 */
struct cc_llm_protocol {
    /** 协议私有状态；由 provider 接管并通过 vtable->destroy 释放。 */
    void *self;
    /** 静态 vtable；必须覆盖 name/build_request/parse_response 等回调。 */
    const cc_llm_protocol_vtable_t *vtable;
};

/**
 * cc_llm_protocol_vtable — 虚函数表，列出该端口/协议必须实现的回调；调用方只通过这些函数指针访问具体实现。
 *
 * build_request 负责把统一 chat request 转成厂商 HTTP 请求；parse_response 和
 * parse_stream_event 再把厂商响应转回 cc_llm_response_t/stream chunk。
 */
struct cc_llm_protocol_vtable {
    /** 返回协议名称的借用字符串，用于日志和诊断。 */
    const char *(*name)(void *self);
    /** 构造一次 HTTP 请求；out_request 成功后由 cleanup 释放内部字段。 */
    cc_result_t (*build_request)(
        void *self,
        const char *base_url,
        const char *api_key,
        const char *default_model,
        const cc_llm_chat_request_t *request,
        int stream,
        cc_llm_http_request_t *out_request
    );
    /** 解析完整非流式响应 JSON，填充 out_response。 */
    cc_result_t (*parse_response)(
        void *self,
        const char *response_json,
        cc_llm_response_t *out_response
    );
    /** 解析一段流式 JSON 事件，并通过 on_chunk 发布增量。 */
    cc_result_t (*parse_stream_event)(
        void *self,
        const char *event_json,
        cc_llm_stream_callback_fn on_chunk,
        void *user_data,
        int *out_finished
    );
    /** 释放协议私有 self；可为 NULL 表示无私有状态。 */
    void (*destroy)(void *self);
};

/**
 * cc_llm_http_request_cleanup — 释放 HTTP LLM request 内部拥有的路径、请求体和 header 数组。
 *
 * 只释放 request 内部字段，不释放 request 结构体本身；清理后字段会被置空，
 * 因而可安全重复用于下一次 build_request 输出。
 *
 * @param request 要清理的请求对象；NULL 时函数直接返回。
 */
void cc_llm_http_request_cleanup(cc_llm_http_request_t *request);

/**
 * cc_http_llm_provider_create — 把协议策略和 HTTP client 组合成统一的 LLM provider。
 *
 * 函数会复制 base_url/api_key/model，并接管 protocol.self。成功后
 * out_provider 可交给 runtime 使用；销毁 provider 时会同时释放 HTTP client、
 * 复制的字符串和协议私有状态。
 *
 * @param base_url 借用字符串；为空时由具体 provider 工厂传入默认值。
 * @param api_key 借用字符串；本地 provider 可传 NULL 或空串。
 * @param model 借用字符串；会复制到 provider 私有状态。
 * @param protocol 协议策略值对象；成功或失败后调用方都不应再销毁其 self。
 * @param out_provider 输出参数；成功时写入 LLM provider 端口值。
 * @return CC_OK 表示 provider 创建成功；失败返回错误码并清理已接管资源。
 */
cc_result_t cc_http_llm_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_protocol_t protocol,
    cc_llm_provider_t *out_provider
);

#endif
