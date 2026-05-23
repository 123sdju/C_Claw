/**
 * 学习导读：cclaw/adapters/src/llm/cc_http_llm_provider.c
 *
 * 所属层次：适配器层。
 * 阅读重点：这里把端口接口落到具体后端，阅读时重点看协议转换、资源释放和失败降级。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_http_llm_provider.c — HTTP LLM provider 传输层模块
 *
 * 本模块在整体架构中的角色：
 * ─────────────────────────────
 * 位于适配器层，是 LLM provider 两层策略中的**传输层**。负责 HTTP 生命周期管理
 * （base_url/api_key/model 字符串所有权、HTTP 请求/响应收发），而 API 协议差异
 * （build_request/parse_response/parse_stream_chunk）则委托给 cc_llm_protocol_t
 * vtable 的具体实现（OpenAI/Ollama/Anthropic）。
 *
 * 两层策略（cc_llm_provider_t = cc_http_llm_provider + cc_llm_protocol_t）：
 * ─────────────────────────────────────────────────────────────────────────
 *
 *   第一层 — cc_http_llm_provider（本模块）：
 *     掌管 HTTP 通用逻辑：拥有并释放 base_url、api_key、model 字符串，
 *     组装 HTTP POST 请求、设置超时、处理 HTTP 状态码错误转换。
 *     流式场景下提供 SSE/NDJSON 两种流格式的通用分帧解析（按 \n\n 或 \n 拆分事件），
 *     然后将每条 JSON 事件分派给协议层的 parse_stream_event。
 *     不感知任何具体 API 的 JSON 格式差异。
 *
 *   第二层 — cc_llm_protocol_t vtable（OpenAI/Ollama/Anthropic 实现）：
 *     只实现三种协议策略回调：build_request（构造 API 特有的 URL/header/body）、
 *     parse_response（解析完整 JSON 响应）、parse_stream_event（解析流式 JSON 增量）。
 *     不感知 HTTP 传输细节（连接、超时、取消令牌）。
 *
 * 上游调用方：
 *   - cc_llm_provider_t 的客户端（runtime/agent 层）通过 chat/chat_stream 接口调用
 *
 * 下游依赖模块：
 *   - cc_http_client.c — 底层 HTTP POST 执行（cc_http_client_perform）
 *   - cc_llm_protocol_t vtable 实现 — build_request / parse_response / parse_stream_event
 *   - cc_string_builder.c — URL 拼接、SSE data 字段拼接
 *   - cc_json.c — 无直接依赖，由协议实现自行处理 JSON
 *
 * ─── 非流式调用流程 ─────────────────────────────────────────────────────
 *
 *   1. http_llm_chat() 被调用
 *   2. 委托 protocol.vtable->build_request() 构造厂商 HTTP 请求
 *   3. http_post_json_with_headers() 发送 POST，检查 2xx 状态码
 *   4. 委托 protocol.vtable->parse_response() 解析响应 JSON
 *   5. cc_llm_http_request_cleanup() 释放 HTTP 请求资源
 *
 * ─── 流式调用流程 ───────────────────────────────────────────────────────
 *
 *   1. http_llm_chat_stream() 被调用
 *   2. 委托 protocol.vtable->build_request(stream=1) 构造流式 HTTP 请求
 *   3. http_post_json_stream_with_headers() 以 stream_body_callback 回调
 *      方式接收网络数据块
 *   4. stream_body_callback() 将数据追加到缓冲区，根据 stream_kind 选择
 *      SSE（\n\n 分帧）或 NDJSON（\n 分行）解析
 *   5. dispatch_stream_event() 将每条 JSON 事件交给 protocol.vtable->parse_stream_event
 *   6. 协议层的 on_chunk 回调最终通知到上层
 *   7. 流结束时 emit_finished_once() 确保只发一次 FINISHED 事件
 *
 * ─── 流格式支持 ─────────────────────────────────────────────────────────
 *
 *   SSE（Server-Sent Events）：
 *     - 双换行（\n\n 或 \r\n\r\n）分隔事件
 *     - 每个事件内 "data:" 前缀的行拼接为 JSON payload
 *     - 遇到 "data: [DONE]" 表示流结束
 *     - 用于 OpenAI 和 Anthropic
 *
 *   NDJSON（Newline Delimited JSON）：
 *     - 每行一个完整的 JSON 事件
 *     - 用于 Ollama
 *
 *   process_sse_buffer / process_ndjson_buffer 会将已完整接收的事件消费掉，
 *   未收齐的尾部片段保留在缓冲区中等待后续数据到达。
 *
 * ─── 设计决策 ─────────────────────────────────────────────────────────
 *
 *   为什么非流式超时 120s，流式超时 300s？
 *     流式响应可能持续输出数分钟（如长推理场景），需要更长的超时容忍。
 *     非流式响应体大小可控，120s 足够覆盖正常请求。
 *
 *   为什么 max_response_bytes 对流式设为 0？
 *     流式响应体大小不可预知（取决于生成长度），因此不设上限。
 *
 *   为什么 http_llm_destroy 同时释放 protocol.self？
 *     cc_http_llm_provider_create 接管 protocol.self 的所有权，
 *     destroy 时统一释放，避免调用方重复管理两个对象生命周期。
 */

#include "cc/adapters/cc_http_llm_provider.h"
#include "cc/util/cc_memory.h"
#include "cc/util/cc_string_builder.h"
#include <stdlib.h>
#include <string.h>

/**
 * cc_http_llm_provider — HTTP LLM provider 私有状态。
 *
 * 拥有 base_url/api_key/model 字符串以及 protocol.self；destroy 时统一释放。
 */
typedef struct cc_http_llm_provider {
    char *base_url;
    char *api_key;
    char *model;
    cc_llm_protocol_t protocol;
} cc_http_llm_provider_t;

/**
 * cc_llm_http_request_cleanup — 释放协议 build_request 填入的 URL、API key、body 和 header 数组。
 *
 * @param request 借用的对象；函数不释放该对象本身。
 */
void cc_llm_http_request_cleanup(cc_llm_http_request_t *request)
{
    if (!request) return;
    free(request->url);
    free(request->api_key);
    free(request->body_json);
    for (size_t i = 0; i < request->header_count; i++) {
        free((char *)request->headers[i].name);
        free((char *)request->headers[i].value);
    }
    free(request->headers);
    memset(request, 0, sizeof(*request));
}

/**
 * http_post_json_with_headers — 处理 HTTP 请求/响应细节，并把传输错误转换为统一结果。
 *
 * @param url 借用的只读字符串；函数不会释放该指针。
 * @param header_count 按值传入，用于控制本次操作。
 * @param body_json 借用的只读字符串；函数不会释放该指针。
 * @param out_response 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t http_post_json_with_headers(
    const char *url,
    const cc_http_header_t *headers,
    size_t header_count,
    const char *body_json,
    cc_cancel_token_t *cancel_token,
    char **out_response
)
{
    cc_http_request_t http_request;
    memset(&http_request, 0, sizeof(http_request));
    http_request.method = "POST";
    http_request.url = url;
    http_request.headers = headers;
    http_request.header_count = header_count;
    http_request.body = body_json;
    http_request.timeout_ms = 120000;
    http_request.cancel_token = cancel_token;

    cc_http_response_t response;
    memset(&response, 0, sizeof(response));
    cc_result_t rc = cc_http_client_perform(&http_request, &response);
    if (rc.code != CC_OK) return rc;

    if (response.status_code < 200 || response.status_code >= 300) {
        cc_result_t err = cc_result_errf(CC_ERR_MODEL,
            "LLM HTTP %ld: %s",
            response.status_code,
            response.body ? response.body : "empty response body");
        cc_http_response_free(&response);
        return err;
    }

    *out_response = response.body ? response.body : cc_strdup("");
    response.body = NULL;
    cc_http_response_free(&response);
    return cc_result_ok();
}

/**
 * cc_http_llm_stream_ctx — HTTP LLM 流式解析上下文，保存协议、回调和未解析尾部缓冲。
 *
 * 该结构只拥有 buffer；protocol、on_chunk 和 user_data 都来自调用方，生命周期必须覆盖
 * 当前 HTTP 请求。buffer 用来跨 on_body chunk 保存未完成的 SSE/NDJSON 片段，finished
 * 防止上游已经发出完成信号后继续把尾部网络数据当作模型增量。
 */
typedef struct cc_http_llm_stream_ctx {
    cc_llm_protocol_t protocol;
    cc_llm_stream_kind_t stream_kind;
    cc_llm_stream_callback_fn on_chunk;
    void *user_data;
    char *buffer;
    size_t buf_len;
    size_t buf_cap;
    size_t total_bytes;
    int finished;
} cc_http_llm_stream_ctx_t;

/**
 * stream_ctx_append — 向动态数组、字符串缓冲或结果集合追加内容，必要时扩容。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param data 借用的只读字符串；函数不会释放该指针。
 * @param len 按值传入，用于控制本次操作。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t stream_ctx_append(
    cc_http_llm_stream_ctx_t *ctx,
    const char *data,
    size_t len
)
{
    if (len == 0) return cc_result_ok();
    if (ctx->buf_len + len >= ctx->buf_cap) {
        size_t new_cap = ctx->buf_cap ? ctx->buf_cap * 2 : 4096;
        while (new_cap < ctx->buf_len + len + 1) new_cap *= 2;
        char *new_buf = realloc(ctx->buffer, new_cap);
        if (!new_buf) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Stream buffer realloc failed");
        ctx->buffer = new_buf;
        ctx->buf_cap = new_cap;
    }
    memcpy(ctx->buffer + ctx->buf_len, data, len);
    ctx->buf_len += len;
    ctx->buffer[ctx->buf_len] = '\0';
    return cc_result_ok();
}

/**
 * emit_finished_once — 确保流式回调只收到一次 finished 事件，避免重复结束通知。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t emit_finished_once(cc_http_llm_stream_ctx_t *ctx)
{
    if (ctx->finished) return cc_result_ok();
    ctx->finished = 1;
    cc_stream_chunk_t chunk = { CC_STREAM_CHUNK_FINISHED, NULL, NULL, NULL };
    ctx->on_chunk(&chunk, ctx->user_data);
    return cc_result_ok();
}

/**
 * dispatch_stream_event — 解析 provider 的一段流式事件，并通过统一 chunk 回调交给 runtime。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param event_json 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t dispatch_stream_event(
    cc_http_llm_stream_ctx_t *ctx,
    const char *event_json
)
{
    if (!event_json || !*event_json) return cc_result_ok();
    if (strcmp(event_json, "[DONE]") == 0) {
        return emit_finished_once(ctx);
    }
    if (!ctx->protocol.vtable->parse_stream_event) {
        return cc_result_error(CC_ERR_PLATFORM, "LLM protocol does not parse stream events");
    }

    int finished = 0;
    cc_result_t rc = ctx->protocol.vtable->parse_stream_event(
        ctx->protocol.self,
        event_json,
        ctx->on_chunk,
        ctx->user_data,
        &finished);
    if (rc.code != CC_OK) return rc;
    if (finished) return emit_finished_once(ctx);
    return cc_result_ok();
}

/**
 * process_sse_event — 处理一条完整 SSE 事件，把 data 字段交给协议解析器并识别 [DONE] 结束标记。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param event_text 可写缓冲区或字符串指针；函数可能就地修改内容但不释放缓冲区本身。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t process_sse_event(
    cc_http_llm_stream_ctx_t *ctx,
    char *event_text
)
{
    cc_string_builder_t data_json;
    cc_result_t rc = cc_string_builder_init(&data_json);
    if (rc.code != CC_OK) return rc;

    char *line = event_text;
    while (*line) {
        char *line_end = strchr(line, '\n');
        if (line_end) *line_end = '\0';
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r') line[len - 1] = '\0';
        if (strncmp(line, "data:", 5) == 0) {
            const char *data = line + 5;
            if (*data == ' ') data++;
            if (data_json.length > 0) cc_string_builder_append(&data_json, "\n");
            rc = cc_string_builder_append(&data_json, data);
            if (rc.code != CC_OK) {
                cc_string_builder_deinit(&data_json);
                return rc;
            }
        }
        if (!line_end) break;
        line = line_end + 1;
    }

    rc = dispatch_stream_event(ctx, cc_string_builder_cstr(&data_json));
    cc_string_builder_deinit(&data_json);
    return rc;
}

/**
 * process_sse_buffer — 从累积的 SSE 缓冲区中拆出完整事件，保留未收齐的尾部片段。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t process_sse_buffer(cc_http_llm_stream_ctx_t *ctx)
{
    char *p = ctx->buffer;
    char *buf_end = ctx->buffer + ctx->buf_len;

    while (!ctx->finished && p < buf_end) {
        char *end = strstr(p, "\n\n");
        size_t sep_len = 2;
        if (!end) {
            end = strstr(p, "\r\n\r\n");
            sep_len = 4;
        }
        if (!end) break;

        char saved = *end;
        *end = '\0';
        cc_result_t rc = process_sse_event(ctx, p);
        *end = saved;
        if (rc.code != CC_OK) return rc;
        p = end + sep_len;
    }

    if (p > ctx->buffer) {
        size_t remaining = ctx->buf_len - (size_t)(p - ctx->buffer);
        if (remaining > 0) memmove(ctx->buffer, p, remaining);
        ctx->buf_len = remaining;
        ctx->buffer[ctx->buf_len] = '\0';
    }
    return cc_result_ok();
}

/**
 * process_ndjson_buffer — 从 NDJSON 流式缓冲区中逐行取出 JSON 事件并分发给协议解析器。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t process_ndjson_buffer(cc_http_llm_stream_ctx_t *ctx)
{
    char *p = ctx->buffer;
    char *buf_end = ctx->buffer + ctx->buf_len;

    while (!ctx->finished && p < buf_end) {
        char *end = strchr(p, '\n');
        if (!end) break;
        *end = '\0';
        size_t len = strlen(p);
        if (len > 0 && p[len - 1] == '\r') p[len - 1] = '\0';
        cc_result_t rc = dispatch_stream_event(ctx, p);
        *end = '\n';
        if (rc.code != CC_OK) return rc;
        p = end + 1;
    }

    if (p > ctx->buffer) {
        size_t remaining = ctx->buf_len - (size_t)(p - ctx->buffer);
        if (remaining > 0) memmove(ctx->buffer, p, remaining);
        ctx->buf_len = remaining;
        ctx->buffer[ctx->buf_len] = '\0';
    }
    return cc_result_ok();
}

/**
 * stream_body_callback — 维护流式输出缓冲和事件分发状态。
 *
 * @param data 借用的只读字符串；函数不会释放该指针。
 * @param len 按值传入，用于控制本次操作。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t stream_body_callback(const char *data, size_t len, void *user_data)
{
    cc_http_llm_stream_ctx_t *ctx = (cc_http_llm_stream_ctx_t *)user_data;
    if (ctx->finished) return cc_result_ok();
    ctx->total_bytes += len;

    cc_result_t rc = stream_ctx_append(ctx, data, len);
    if (rc.code != CC_OK) return rc;

    if (ctx->stream_kind == CC_LLM_STREAM_SSE) {
        return process_sse_buffer(ctx);
    }
    if (ctx->stream_kind == CC_LLM_STREAM_NDJSON) {
        return process_ndjson_buffer(ctx);
    }
    return cc_result_error(CC_ERR_PLATFORM, "Unsupported LLM stream framing");
}

/**
 * flush_stream_tail — 维护流式输出缓冲和事件分发状态。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t flush_stream_tail(cc_http_llm_stream_ctx_t *ctx)
{
    if (ctx->finished || ctx->buf_len == 0) return cc_result_ok();
    if (ctx->stream_kind == CC_LLM_STREAM_NDJSON) {
        cc_result_t rc = dispatch_stream_event(ctx, ctx->buffer);
        ctx->buf_len = 0;
        if (ctx->buffer) ctx->buffer[0] = '\0';
        return rc;
    }
    return cc_result_ok();
}

/**
 * http_post_json_stream_with_headers — 处理 HTTP 请求/响应细节，并把传输错误转换为统一结果。
 *
 * @param protocol 按值传入，用于控制本次操作。
 * @param on_chunk 按值传入，用于控制本次操作。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t http_post_json_stream_with_headers(
    const cc_llm_http_request_t *http_req,
    cc_llm_protocol_t protocol,
    cc_cancel_token_t *cancel_token,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    cc_http_llm_stream_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.protocol = protocol;
    ctx.stream_kind = http_req->stream_kind;
    ctx.on_chunk = on_chunk;
    ctx.user_data = user_data;

    cc_http_request_t http_request;
    memset(&http_request, 0, sizeof(http_request));
    http_request.method = "POST";
    http_request.url = http_req->url;
    http_request.headers = http_req->headers;
    http_request.header_count = http_req->header_count;
    http_request.body = http_req->body_json;
    http_request.timeout_ms = 300000;
    http_request.max_response_bytes = 0;
    http_request.on_body = stream_body_callback;
    http_request.user_data = &ctx;
    http_request.cancel_token = cancel_token;

    cc_http_response_t response;
    memset(&response, 0, sizeof(response));
    cc_result_t rc = cc_http_client_perform(&http_request, &response);
    if (rc.code == CC_OK) {
        rc = flush_stream_tail(&ctx);
    }

    if (rc.code == CC_OK && (response.status_code < 200 || response.status_code >= 300)) {
        rc = cc_result_errf(CC_ERR_MODEL,
            "LLM stream HTTP %ld: %s",
            response.status_code,
            response.body ? response.body : "empty response body");
    }

    if (rc.code == CC_OK) {
        rc = emit_finished_once(&ctx);
    }

    cc_http_response_free(&response);
    free(ctx.buffer);
    return rc;
}

/**
 * http_llm_chat — 执行一次非流式 LLM HTTP 调用，发送请求并解析完整 JSON 响应。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param request 借用的对象；函数不释放该对象本身。
 * @param out_response 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t http_llm_chat(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_response_t *out_response
)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    memset(out_response, 0, sizeof(*out_response));

    cc_llm_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    cc_result_t rc = provider->protocol.vtable->build_request(
        provider->protocol.self,
        provider->base_url,
        provider->api_key,
        provider->model,
        request,
        0,
        &http_req
    );
    if (rc.code != CC_OK) return rc;

    char *response_json = NULL;
    rc = http_post_json_with_headers(
        http_req.url,
        http_req.headers,
        http_req.header_count,
        http_req.body_json,
        request->cancel_token,
        &response_json
    );
    cc_llm_http_request_cleanup(&http_req);
    if (rc.code != CC_OK) return rc;

    rc = provider->protocol.vtable->parse_response(
        provider->protocol.self,
        response_json,
        out_response
    );
    free(response_json);
    return rc;
}

/**
 * http_llm_chat_stream — 维护流式输出缓冲和事件分发状态。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 * @param request 借用的对象；函数不释放该对象本身。
 * @param on_chunk 按值传入，用于控制本次操作。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t http_llm_chat_stream(
    void *self,
    const cc_llm_chat_request_t *request,
    cc_llm_stream_callback_fn on_chunk,
    void *user_data
)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    cc_llm_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    cc_result_t rc = provider->protocol.vtable->build_request(
        provider->protocol.self,
        provider->base_url,
        provider->api_key,
        provider->model,
        request,
        1,
        &http_req
    );
    if (rc.code != CC_OK) return rc;

    if (http_req.stream_kind == CC_LLM_STREAM_NONE ||
        !provider->protocol.vtable->parse_stream_event) {
        cc_llm_http_request_cleanup(&http_req);
        return cc_result_error(CC_ERR_PLATFORM, "LLM protocol does not support streaming");
    }

    rc = http_post_json_stream_with_headers(
        &http_req,
        provider->protocol,
        request->cancel_token,
        on_chunk,
        user_data);
    cc_llm_http_request_cleanup(&http_req);
    return rc;
}

/**
 * http_llm_destroy — 释放、停止或复位该组件拥有的资源，防止失败路径泄漏。
 *
 * @param self vtable 私有上下文；生命周期由创建该端口的实现管理。
 */
static void http_llm_destroy(void *self)
{
    cc_http_llm_provider_t *provider = (cc_http_llm_provider_t *)self;
    if (!provider) return;
    free(provider->base_url);
    free(provider->api_key);
    free(provider->model);
    if (provider->protocol.vtable && provider->protocol.vtable->destroy) {
        provider->protocol.vtable->destroy(provider->protocol.self);
    }
    free(provider);
}

static cc_llm_provider_vtable_t http_llm_vtable = {
    http_llm_chat,
    http_llm_chat_stream,
    http_llm_destroy
};

/**
 * cc_http_llm_provider_create — 完成对应初始化步骤，失败时返回 cc_result_t 错误。
 *
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param model 借用的只读字符串；函数不会释放该指针。
 * @param protocol 按值传入，用于控制本次操作。
 * @param out_provider 输出参数；调用方传入有效指针，成功后接收结果。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_http_llm_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_protocol_t protocol,
    cc_llm_provider_t *out_provider
)
{
    if (!protocol.vtable || !protocol.vtable->build_request ||
        !protocol.vtable->parse_response || !out_provider) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid HTTP LLM provider argument");
    }

    cc_http_llm_provider_t *provider = calloc(1, sizeof(*provider));
    if (!provider) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create HTTP LLM provider");
    }

    provider->base_url = base_url ? cc_strdup(base_url) : cc_strdup("");
    provider->api_key = api_key ? cc_strdup(api_key) : NULL;
    provider->model = model ? cc_strdup(model) : cc_strdup("");
    provider->protocol = protocol;

    if (!provider->base_url || !provider->model) {
        http_llm_destroy(provider);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy HTTP LLM provider config");
    }

    out_provider->self = provider;
    out_provider->vtable = &http_llm_vtable;
    return cc_result_ok();
}
