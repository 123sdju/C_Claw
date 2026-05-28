# C-Claw 工程总览

这份文档先从工程结构解释 C-Claw SDK，再给出建议的逐文件阅读顺序。后续补注释时，每个源码文件都应该围绕这里的模块边界展开，而不是只在文件顶部放模板说明。

## 一句话定位

C-Claw 是一个纯 C Agent Runtime SDK。它不绑定 CLI、Web、UART 或图形界面，也不绑定某个具体 LLM 厂商；下游应用通过 `add_subdirectory(cclaw)` 复用 runtime，并自己提供 gateway、配置加载、API key、硬件采集和 UI 展示。

## 分层结构

```text
应用层（仓库外）
  负责用户入口、配置文件、API key、界面、硬件和业务工具。

cclaw/core/
  SDK 的通用核心。包含消息模型、Agent 主循环、上下文构建、工具执行、事件、配置和 token 估算。

cclaw/ports/
  抽象端口层。用 struct + vtable + void *self 表达可替换接口，例如 LLM、tool、storage、filesystem、thread。

cclaw/adapters/
  默认适配器层。把端口接口落到具体实现，例如 OpenAI/Ollama/Anthropic HTTP provider、JSON/SQLite store、内置文件工具。

cclaw/platforms/
  平台实现层。提供 POSIX、Windows、ESP32、FreeRTOS 的线程、文件、路径、HTTP、进程等能力。

cclaw/profiles/
  编译期能力裁剪层。不同 profile 决定哪些源文件和宏会进入最终 SDK。

cclaw/tests/
  行为测试。适合反向学习接口契约，尤其是并发、配置、工具、MCP、消息序列化等边界。
```

## 核心运行链路

一次用户请求大致经过下面的路径：

1. 下游 gateway 调用 `cc_agent_runtime_handle_message()` 或流式接口。
2. runtime 把用户消息写入 `cc_session_store_t`。
3. `cc_context_builder_build_messages()` 从 store 加载历史消息，注入 system prompt 和长期记忆，并按 token 预算压缩或截断。
4. runtime 从工具注册表构造 tools schema，并把 typed messages 交给 `cc_llm_provider_t`。
5. provider adapter 把 SDK 的 typed messages 转成目标厂商协议，请求 LLM。
6. 如果 LLM 返回 tool call，runtime 调用 `cc_tool_executor` 执行工具，把工具结果写回 session store，然后进入下一轮。
7. 如果 LLM 返回最终文本，runtime 写入 assistant 消息并把文本返回给 gateway。

## 逐文件阅读顺序

建议按“数据模型 -> 端口契约 -> 主流程 -> 适配器 -> 平台与测试”的顺序读：

1. `cclaw/core/include/cc/core/cc_result.h`：统一错误返回和错误字符串所有权。
2. `cclaw/core/include/cc/core/cc_message.h` 与 `cclaw/core/src/core/cc_message.c`：消息、content parts、tool calls 与 JSON 序列化。
3. `cclaw/core/include/cc/core/cc_tool_call.h` 与 `cclaw/core/src/core/cc_tool_call.c`：工具调用的 ID、名称、参数和结果关联。
4. `cclaw/ports/include/cc/ports/cc_llm_provider.h`：LLM provider 的 vtable 契约。
5. `cclaw/ports/include/cc/ports/cc_session_store.h`：会话历史如何持久化。
6. `cclaw/core/include/cc/app/cc_agent_runtime.h` 与 `cclaw/core/src/app/cc_agent_runtime.c`：Agent 主循环。
7. `cclaw/core/include/cc/app/cc_context_builder.h` 与 `cclaw/core/src/app/cc_context_builder.c`：LLM 请求上下文如何拼出来。
8. `cclaw/core/src/app/cc_tool_executor.c`：工具查找、策略检查、审批和执行。
9. `cclaw/adapters/src/llm/cc_http_llm_provider.c`：HTTP provider 公共传输层。
10. `cclaw/adapters/src/llm/cc_openai_provider.c`、`cc_ollama_provider.c`、`cc_anthropic_provider.c`：各厂商协议差异。
11. `cclaw/adapters/src/storage/*`：不同 session/memory/artifact store 的实现。
12. `cclaw/platforms/*/src/*`：平台端口如何适配到操作系统或 RTOS。
13. `cclaw/tests/**`：用测试确认每个模块的契约和边界。

## 嵌入式面试阅读路径

如果目标是嵌入式软件开发，建议按下面三篇扩展文档准备项目表达：

1. `docs/learning-embedded-linux-interview.md`：把 POSIX、线程、文件、网络、进程和 CMake 发布讲成嵌入式 Linux SDK 能力。
2. `docs/learning-mcu-rtos-interview.md`：把 profile 裁剪、FreeRTOS/ESP32 port、内存限制和任务同步讲成 MCU/RTOS 能力。
3. `docs/learning-c-oop-patterns.md`：把 C 语言 `struct + vtable + void *self`、依赖注入和设计模式讲清楚。
4. `docs/learning-observability.md`：把统一事件 schema、stream callback 和 event bus 的职责边界讲清楚。

面试时可以先讲总体分层，再按岗位方向展开 Linux 或 MCU/RTOS 细节，最后用设计模式说明为什么
这个 C 项目不是简单堆函数，而是一个可替换、可测试、可裁剪的 SDK。

## 注释补充原则

- 先解释模块在链路中的角色，再解释函数内部步骤。
- 优先补所有权、错误路径、线程安全、跨模块调用关系。
- 不给显而易见的语句写注释；注释应该回答“为什么这样做”和“读者应该注意什么”。
- 源码大改后，先更新对应学习文档，再更新文件级和函数级注释。
- 第三方 `vendor` 代码只在构建文档中说明来源和用途，不逐行改注释。
