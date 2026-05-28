# Runtime 学习文档

Runtime 是 SDK 的编排核心。它不直接知道具体 provider、工具或存储实现，而是通过
`cc_runtime_feature_set_t` 和端口 vtable 获得能力。

## 主流程

1. gateway 收到用户输入。
2. runtime 将用户消息写入 session store。
3. context builder 从 store 加载 typed messages，并注入 system prompt。
4. runtime 构造 `cc_llm_chat_request_t`，传入 typed messages、tools schema、模型参数和 media limits。
5. provider 返回 `cc_llm_response_t`。
6. 如果有 `tool_calls`，runtime 执行工具、写回 tool result，再进入下一轮。
7. 如果有文本输出，runtime 写入 assistant 消息并返回给 gateway。

## 所有权规则

- `cc_agent_runtime_create()` 深拷贝配置中的字符串和多模态 MIME 白名单。
- 注入的 provider、store、registry、event bus 等端口由 builder 或应用持有。
- `char **out_response` 成功返回时由调用方释放。
- typed message 数组由构建者释放：逐个 `cc_message_cleanup()` 后释放数组。

## Observability

Runtime 只通过 `cc_observability_publish()` 发布业务事件。事件 bus 是观测通道，
stream callback 才是实时输出通道。统一 payload 包含 `schema_version`、`event`、
`session_id`、`run_id`、`step`、`status`、可选 `error` 和 `attributes`。

面试可以这样讲：

> 我把实时输出和观测解耦。UI 直接消费 stream callback，日志和调试 UI 订阅 event bus。
> 所有事件都是统一 schema，所以下游不用为每种事件写一套解析逻辑。

## 嵌入式面试关注点

- Runtime 不直接依赖 Linux API，平台能力来自 ports。
- 同步和流式路径共享 tool executor、context builder、limits 和 memory 逻辑。
- 取消采用协作式 token，适合 RTOS，避免强杀线程导致资源泄漏。
- stream partial、取消、超限和 provider 错误默认不落库。
- provider 能力在 create 阶段 fail-fast，避免运行时静默降级。

常见追问：

- 为什么要 max_steps？
  - 防止模型和工具循环调用导致设备资源耗尽。
- 为什么 stream partial 不落库？
  - partial 可能是不完整句子或错误输出，落库会污染会话历史和长期记忆。
- Runtime 怎么测试？
  - 注入 fake provider、memory session store 和 fake tool registry，不需要真实网络或硬件。
