/**
 * 学习导读：cclaw/core/include/cc/core/cc_stream_chunk.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 LLM 流式输出 chunk，重点看文本、reasoning、tool call
 *           增量字段如何被 CLI/gateway 消费。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_stream_chunk.h — 流式输出增量数据块模块
 *
 * @file    cc/core/cc_stream_chunk.h
 * @brief   定义流式 LLM 输出的增量数据块结构及回调类型。
 *
 * 本模块为 LLM Provider 的流式接口（chat_stream）和 Agent 运行时
 * 的流式主循环提供统一的数据交换格式。当 LLM 以 SSE（Server-Sent
 * Events）方式逐步返回响应时，每个事件被解析为不同的 chunk 类型，
 * 通过回调函数传递给上层。
 *
 * ─── Chunk 类型说明 ──────────────────────────────────────────────────
 *
 *   CC_STREAM_CHUNK_TEXT        — 普通文本增量（用户可见的回复）
 *   CC_STREAM_CHUNK_THINKING    — 思考内容增量（reasoning_content）
 *   CC_STREAM_CHUNK_TOOL_START  — 工具调用开始（LLM 决定调用工具）
 *   CC_STREAM_CHUNK_TOOL_DELTA  — 工具调用参数增量（arguments JSON 片段）
 *   CC_STREAM_CHUNK_TOOL_END    — 工具调用参数收集完毕
 *   CC_STREAM_CHUNK_FINISHED    — 流结束（finish_reason 为 stop 或
 *                                 无 chunk_type）
 *
 * ─── 流式数据流转 ────────────────────────────────────────────────────
 *
 *   LLM SSE Stream
 *     │
 *     ▼
 *   cc_openai_provider.chat_stream()
 *     │ 解析 SSE → 识别 chunk_type
 *     │
 *     ▼
 *   cc_llm_stream_callback_fn (chunk, user_data)
 *     │
 *     ▼
 *   cc_agent_runtime_handle_message_stream()
 *     │ 累积文本/参数 + 发布事件总线 + 执行工具
 *     │
 *     ▼
 *   事件总线 → CLI / Web Gateway 实时展示
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   无任何外部依赖。
 */

#ifndef CC_STREAM_CHUNK_H
#define CC_STREAM_CHUNK_H

/**
 * cc_stream_chunk_type_t — 流式 chunk 类型枚举
 *
 * 每个从 LLM Provider 发出的流式数据块必属于以下类型之一。
 * 上层调用者根据 type 决定如何解释 content 字段。
 */
typedef enum {
    CC_STREAM_CHUNK_TEXT = 0,       /**< 普通文本增量：content 为回复文本片段
                                     *   多个 TEXT chunk 按顺序拼接即为完整回复 */
    CC_STREAM_CHUNK_THINKING = 1,   /**< 思考内容增量：content 为 reasoning_content
                                     *   片段。仅 DeepSeek-R1 等模型支持 */
    CC_STREAM_CHUNK_TOOL_START = 2, /**< 工具调用开始：tool_name 和 tool_id 有效
                                     *   表示 LLM 决定调用一个工具 */
    CC_STREAM_CHUNK_TOOL_DELTA = 3, /**< 工具调用参数增量：content 为 arguments
                                     *   JSON 片段，需累积拼接 */
    CC_STREAM_CHUNK_TOOL_END = 4,   /**< 工具调用参数收集完毕：
                                     *   tool_name / tool_id / content(完整参数) 有效 */
    CC_STREAM_CHUNK_FINISHED = 5,   /**< 流结束：无有效数据，仅通知上层流已关闭 */
} cc_stream_chunk_type_t;

/**
 * cc_stream_chunk_t — 流式增量数据块
 *
 * 每个 chunk 代表 LLM 流式输出中的一个有意义的数据增量。
 * 不同 type 下各字段的有效性不同：
 *
 *   TEXT / THINKING:  content 有效
 *   TOOL_START:       tool_name + tool_id 有效
 *   TOOL_DELTA:       content(参数片段) + tool_id 有效
 *   TOOL_END:         tool_name + tool_id + content(完整参数) 有效
 *   FINISHED:         所有字段可能为空
 */
typedef struct cc_stream_chunk {
    cc_stream_chunk_type_t type; /**< chunk 类型，决定哪些字段有效 */
    char *content;               /**< 文本增量或参数增量（根据 type 解释） */
    char *tool_name;             /**< 工具名称（TOOL_START / TOOL_END 时有效） */
    char *tool_id;               /**< LLM 返回的工具调用 ID（关联 tool 消息） */
} cc_stream_chunk_t;

/**
 * cc_llm_stream_callback_fn — 流式 chunk 回调函数类型
 *
 * LLM Provider 每收到一个 SSE chunk 就调用此回调一次。
 * 回调内部实现应尽量轻量，因为 SSE 是高频事件（每次文本增量 ~2-5 字符）。
 *
 * @param chunk     当前收到的增量数据块（回调结束后内容可能释放）
 * @param user_data 调用方自定义数据（通常为 cc_agent_runtime_t 指针）
 */
typedef void (*cc_llm_stream_callback_fn)(
    const cc_stream_chunk_t *chunk,
    void *user_data
);

/*
 * ═══════════════════════════════════════════════════════════════
 * 流式事件总线事件名称常量
 * ═══════════════════════════════════════════════════════════════
 *
 * 这些事件由 Agent 流式主循环内部发布，CLI / Web Gateway 通过
 * 订阅这些事件实现实时展示。
 */

/** 收到文本增量，payload: {"text":"增量文本"} */
#define CC_EVENT_STREAM_TEXT       "stream.text"

/** 收到思考内容增量，payload: {"content":"思考片段"} */
#define CC_EVENT_STREAM_THINKING   "stream.thinking"

/** LLM 开始调用工具，payload: {"tool_name":"...","tool_id":"..."} */
#define CC_EVENT_STREAM_TOOL_START "stream.tool.start"

/** 工具调用参数增量，payload: {"tool_id":"...","arguments":"增量"} */
#define CC_EVENT_STREAM_TOOL_DELTA "stream.tool.delta"

/** 工具调用完成，payload: {"tool_name":"...","tool_id":"...","arguments":"完整JSON"} */
#define CC_EVENT_STREAM_TOOL_END   "stream.tool.end"

/** 流结束，payload: "{}" */
#define CC_EVENT_STREAM_FINISHED   "stream.finished"

#endif
