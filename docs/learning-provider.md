# Provider 学习文档

Provider 是 LLM 协议适配层。SDK core 只传 typed messages，provider adapter 负责转换成
目标协议。

## 请求输入

`cc_llm_chat_request_t` 重要字段：

- `messages` / `message_count`：typed message 数组。
- `media_limits`：当前 runtime 生效的媒体限制。
- `tools_json`：工具 schema，仍使用 JSON，因为工具参数 schema 天然是 JSON Schema。
- `thinking_mode`：是否请求 provider 返回 reasoning content。
- `cancel_token`：协作式取消。
- `timeout_ms`：runtime 传入的 provider timeout；0 表示 provider 默认值。

## 响应输出

`cc_llm_response_t` 可同时包含：

- `text`：普通文本回复。
- `content`：多模态 content part 输出。
- `artifacts`：图片、音频、文件等 artifact 输出。
- `tool_calls`：多个工具调用。
- `reasoning_content`：provider 支持时返回的推理内容。

## 能力声明

Provider 可实现 vtable 的 `capabilities` 方法，返回自身支持的输入/输出 modality、
tool calling、reasoning、streaming 和限制。runtime 与 gateway 可用它展示当前可用能力。

runtime 创建阶段会 fail-fast 校验 text、tool calling 和多模态配置。正式 stream callback
路径要求 provider 支持 streaming；不支持时返回 `CC_ERR_UNSUPPORTED`，避免实时输出静默降级。

## 错误恢复

SDK 不在 provider adapter 内自动 retry。HTTP provider 只做错误分类，并通过
`cc_result_t.detail` 暴露结构化元数据：

- 429 -> `CC_ERR_RATE_LIMIT`
- 5xx / 网络失败 -> `CC_ERR_NETWORK`
- timeout -> `CC_ERR_TIMEOUT`
- cancel -> `CC_ERR_CANCELLED`
- 响应 JSON 无法解析 -> `CC_ERR_JSON`

429 会解析 `Retry-After` 为 `retry_after_ms`；5xx 标记 `recoverable=true`；4xx 默认
`recoverable=false`。`raw_redacted_body` 只保存脱敏后的 body，错误 message 也不能包含原始
secret。下游应用根据这些错误码和 metadata 决定 backoff、重试、提示用户或保留 partial UI
状态。

## 嵌入式面试关注点

Provider 层适合讲“协议适配器”和“错误恢复边界”：

- core 只传 typed messages，HTTP provider adapter 负责厂商 JSON 协议。
- OpenAI、Ollama、Anthropic 的协议差异被隔离在 adapter。
- `capabilities` 让 runtime 在 create 阶段 fail-fast。
- HTTP 429/5xx/timeout/cancel/JSON parse 有稳定错误分类。
- SDK 不内置 retry/backoff，避免在设备侧隐藏网络策略和功耗策略。

面试表达：

> Provider 是典型适配器模式。core 不关心厂商 HTTP JSON 格式，只依赖 `cc_llm_provider_t`。
> 错误也不在 adapter 里无限重试，而是结构化暴露给上层，让设备应用根据网络、电量和业务策略决定。

常见追问：

- 为什么不在 SDK 里自动 retry？
  - 嵌入式设备的 retry 会影响功耗、流量和实时性，应该由产品层决定 backoff 策略。
- 为什么要 capabilities？
  - 避免配置启用了 stream/tool/multimodal，但 provider 不支持时运行时才失败。
- MCU 没有 HTTPS 资源怎么办？
  - 可以让应用 gateway 代理 provider，SDK 保留 runtime 和 provider port，不强制内置 HTTP。
