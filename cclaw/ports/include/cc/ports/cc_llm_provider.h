/**
 * 学习导读：cclaw/ports/include/cc/ports/cc_llm_provider.h
 *
 * 所属层次：端口层。
 * 阅读重点：这里定义可替换接口，阅读时重点看 struct + vtable + void *self 如何表达多态和依赖注入。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_llm_provider.h — LLM 提供商抽象端口（Port）
 *
 * @file    cc/ports/cc_llm_provider.h
 * @brief   定义大语言模型后端的抽象接口，实现多种 LLM 服务的统一调用。
 *
 * 本模块是端口-适配器架构中的核心端口。不同的 LLM 后端（Ollama、OpenAI、
 * DeepSeek 等）通过实现相同的 vtable 提供统一的 chat 接口，
 * 上层 Agent Runtime 不感知底层差异。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 每个 LLM Provider 实现必须填充 vtable 的 chat 和 destroy 方法
 *   - chat 方法接收 cc_llm_chat_request_t（含消息历史和工具列表），
 *     返回 cc_llm_response_t（含文本回复或工具调用请求）
 *   - 所有字段通过内部拷贝管理，调用方保有参数所有权
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/core/cc_result.h 和 cc/core/cc_tool_call.h。
 */

#ifndef CC_LLM_PROVIDER_H
#define CC_LLM_PROVIDER_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_tool_call.h"
#include "cc/core/cc_stream_chunk.h"

/**
 * cc_llm_chat_request_t — LLM 聊天请求
 *
 * 封装一次 LLM 调用的所有输入参数，包括消息历史、可用工具、
 * 模型选择、生成参数等。结构体中的所有字符串为请求期间有效，
 * 由调用方管理生命周期。
 */
typedef struct cc_llm_chat_request {
    char *messages_json;    /**< 消息历史，JSON 数组格式。
                             *   每条消息含 role 和 content。符合 OpenAI Chat API 规范。
                             *   如: [{"role":"user","content":"Hello"}] */
    char *tools_json;       /**< 可用工具的 JSON Schema 数组。
                             *   由 cc_tool_registry_build_schema_json() 生成。
                             *   可为 NULL（无工具可用时）。 */
    char *model;            /**< 模型名称（如 "qwen2.5-coder:7b"、"gpt-4o"） */
    int stream;             /**< 是否启用流式输出：1 = SSE 流式, 0 = 一次性返回。
                             *   当前版本暂不支持 stream（预留字段）。 */
    int max_tokens;         /**< 最大生成 token 数。0 表示使用模型默认值 */
    double temperature;     /**< 生成温度（0.0 ~ 2.0）。越高越随机，越低越确定。
                             *   默认 0.7。设为 0 表示贪婪解码（最确定）。 */
    int thinking_mode;      /**< 是否启用思维链模式：1 = LLM 输出 CoT 推理内容,
                             *   0 = 仅输出最终回答。需要模型支持。 */
} cc_llm_chat_request_t;

/* ── 前向声明 ───────────────────────────────────────────────────────── */

typedef struct cc_llm_provider_vtable cc_llm_provider_vtable_t;
/**
 * cc_llm_provider_t — 前向声明的端口/服务句柄类型，具体字段在本文件后文或对应端口中定义。
 */
typedef struct cc_llm_provider cc_llm_provider_t;

/**
 * cc_llm_provider_t — LLM 提供商实例（多态句柄）
 *
 * 值语义结构体，通过 self + vtable 实现多态。
 * 可直接按值传递，浅拷贝后两个实例指向同一个底层后端。
 */
struct cc_llm_provider {
    void *self;                           /**< 指向具体后端实现的私有数据 */
    const cc_llm_provider_vtable_t *vtable; /**< 虚函数表 */
};

/**
 * cc_llm_provider_vtable_t — LLM 提供商虚函数表
 *
 * 定义 LLM 后端的抽象接口。不同后端的差异（HTTP API 格式、
 * 认证方式等）被封装在 chat 方法的实现内部。
 */
struct cc_llm_provider_vtable {
    /**
     * chat — 发送聊天请求并获取 LLM 回复（同步、非流式）
     *
     * 将请求发送到 LLM 后端并等待完整回复。根据回复内容的不同：
     *   - 如果 LLM 返回文本：response.has_text=1, response.text 有内容
     *   - 如果 LLM 返回工具调用：response.has_tool_call=1, response.tool_call 有内容
     *   - 如果 LLM 认为完成：response.finished=1
     *
     * @param self          后端私有数据
     * @param request       请求参数（消息历史、工具、模型等）
     * @param out_response  输出：LLM 回复（调用者负责 cc_llm_response_free）
     * @return              CC_OK 表示成功
     */
    cc_result_t (*chat)(
        void *self,
        const cc_llm_chat_request_t *request,
        cc_llm_response_t *out_response
    );

    /**
     * chat_stream — 发送聊天请求并以 SSE 流式方式获取 LLM 回复
     *
     * 通过 on_chunk 回调逐步返回 LLM 的响应增量。每个 chunk 可能是：
     *   - CC_STREAM_CHUNK_TEXT: 回复文本片段
     *   - CC_STREAM_CHUNK_THINKING: 思维链/推理过程片段
     *   - CC_STREAM_CHUNK_TOOL_START: 工具调用开始
     *   - CC_STREAM_CHUNK_TOOL_DELTA: 工具调用参数增量
     *   - CC_STREAM_CHUNK_TOOL_END: 工具调用参数收集完毕
     *   - CC_STREAM_CHUNK_FINISHED: 流结束
     *
     * 语义约定：
     *   - on_chunk 被调用的线程上下文由具体实现决定（可能是网络线程）
     *   - 每次 on_chunk 调用之间不保证时序（但实现上 SSE 保证顺序）
     *   - FINISHED 之后不再调用 on_chunk
     *
     * @param self      后端私有数据
     * @param request   请求参数（stream 字段会被实现强制设为 1）
     * @param on_chunk  每收到一个 chunk 时调用的回调
     * @param user_data 透传给 on_chunk 的上下文数据
     * @return          CC_OK 表示流已正常结束，非零表示连接失败
     */
    cc_result_t (*chat_stream)(
        void *self,
        const cc_llm_chat_request_t *request,
        cc_llm_stream_callback_fn on_chunk,
        void *user_data
    );

    /**
     * destroy — 销毁 LLM 后端实例
     *
     * 释放后端的网络连接、HTTP 客户端等资源。
     *
     * @param self  后端私有数据
     */
    void (*destroy)(void *self);
};

#endif