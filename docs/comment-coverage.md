# C-Claw 逐函数中文注释覆盖清单

本文件用于记录逐文件、逐函数中文注释的人工进度。状态只在我已经完整读取对应文件并手工补充注释后标为“完成”；未处理或只做过粗略注释的文件保持“待处理”。

## 覆盖规则

- 覆盖范围：仓库自维护 `.c` / `.h` 文件、测试文件和必要测试 fixture。
- 排除范围：`vendor/` 第三方代码。
- 注释方式：逐文件读取后使用手工 patch 添加注释，不使用脚本、正则批量插注释或模板化改写。
- 注释重点：函数职责、所有权、线程安全、失败路径、资源释放、跨模块关系，以及嵌入式面试中可以讲清楚的设计点。

## 当前进度

- 目标文件数：162
- 已完成：162
- 当前批次：全部批次完成

## 已完成文件

| 文件 | 状态 | 说明 |
| --- | --- | --- |
| `cclaw/core/include/cc/core/cc_result.h` | 完成 | 错误码、结构化错误、result 生命周期和 public API 已补中文注释。 |
| `cclaw/core/src/core/cc_result.c` | 完成 | result 构造、detail 深拷贝、释放和错误码映射函数已逐函数注释。 |
| `cclaw/core/include/cc/core/cc_message.h` | 完成 | message role、结构所有权、创建/拷贝/JSON API 已补中文契约说明。 |
| `cclaw/core/src/core/cc_message.c` | 完成 | message 创建、深拷贝、content/tool call 管理、JSON 序列化解析已逐函数注释。 |
| `cclaw/core/include/cc/core/cc_media.h` | 完成 | 多模态类型、artifact/content part 所有权、限制策略和 JSON API 已补中文注释。 |
| `cclaw/core/src/core/cc_media.c` | 完成 | artifact/list/content parts 的校验、拷贝、序列化、摘要函数已逐函数注释。 |
| `cclaw/core/include/cc/core/cc_tool_call.h` | 完成 | tool call、tool result、LLM response 的所有权和执行语义已补中文注释。 |
| `cclaw/core/src/core/cc_tool_call.c` | 完成 | tool call/list/result/LLM response 的创建、拷贝、释放和 JSON 解析已逐函数注释。 |
| `cclaw/core/include/cc/core/cc_memory_entry.h` | 完成 | memory entry 字段所有权、时间字段和释放规则已补中文注释。 |
| `cclaw/core/src/core/cc_memory_entry.c` | 完成 | memory entry 初始化、单项释放、数组释放已逐函数注释。 |
| `cclaw/core/include/cc/core/cc_session.h` | 完成 | session 状态、字段所有权、workspace 边界和创建/销毁 API 已补中文注释。 |
| `cclaw/core/src/core/cc_session.c` | 完成 | session 创建失败回滚和销毁边界已逐函数注释，并补齐 out 参数/OOM 检查。 |
| `cclaw/core/include/cc/core/cc_stream_chunk.h` | 完成 | stream chunk 类型、回调生命周期和实时输出/观测边界已补中文注释。 |
| `cclaw/core/include/cc/core/cc_observability.h` | 完成 | 统一事件常量、payload schema、attributes 和发布 API 已补中文注释。 |
| `cclaw/core/src/core/cc_observability.c` | 完成 | 事件 JSON 构造、attributes 校验、error/detail 注入和发布入口已逐函数注释。 |
| `cclaw/core/include/cc/cclaw.h` | 完成 | SDK 聚合头的 public API 边界、模块分组和 extension-only 定位已补中文注释。 |
| `cclaw/core/include/cc/core/cc_version.h` | 完成 | 语义版本、废弃标记和版本查询函数已补中文注释。 |
| `cclaw/core/include/cc/core/cc_types.h` | 完成 | 公共基础类型占位头的边界和扩展约束已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_event_bus.h` | 完成 | event bus 回调、同步/异步模式、背压和发布订阅语义已补中文注释。 |
| `cclaw/core/src/core/cc_event_bus.c` | 完成 | event bus 订阅快照、异步队列、worker、flush 和 redaction 发布路径已逐函数注释。 |
| `cclaw/ports/include/cc/ports/cc_tool_registry.h` | 完成 | tool registry 所有权、freeze、查找、schema 构建和列举 API 已补中文注释。 |
| `cclaw/core/src/core/cc_tool_registry.c` | 完成 | tool registry 固定容量、mutex、freeze、schema 构建和名称复制路径已逐函数注释，并补齐空指针/OOM 检查。 |
| `cclaw/ports/include/cc/ports/cc_memory_store.h` | 完成 | memory store vtable、结构化 query、结果所有权和 adapter 扩展规则已补中文注释。 |
| `cclaw/core/src/core/cc_memory_store.c` | 完成 | memory store wrapper、query fallback、过滤和结果释放路径已逐函数注释。 |
| `cclaw/ports/include/cc/ports/cc_tool.h` | 完成 | tool OOP 接口、runtime services、调用上下文、审批回调和 vtable 契约已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_llm_provider.h` | 完成 | provider 能力矩阵、chat 请求、同步/流式 vtable 和错误恢复边界已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_http_client.h` | 完成 | HTTP request/response/header 所有权、body 回调、timeout/cancel 和安全 redirect 边界已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_filesystem.h` | 完成 | filesystem port、vtable、路径安全配合和返回资源所有权已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_path.h` | 完成 | 路径拼接、canonical、workspace 边界、dirname 和存在性查询已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_thread.h` | 完成 | 线程、mutex、cond 的不透明句柄、等待语义和平台移植边界已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_process.h` | 完成 | 进程执行、管道、timeout/cancel、结果所有权和高风险操作边界已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_env.h` | 完成 | 环境变量读取/解析/设置和 MCU 配置替代语义已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_logger.h` | 完成 | 日志级别、logger 生命周期、线程安全和 redaction 约束已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_policy_engine.h` | 完成 | 风险等级、审批决策、策略 vtable 和 reason 所有权已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_sandbox.h` | 完成 | sandbox command/result、run/destroy vtable、timeout 和高风险命令边界已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_platform.h` | 完成 | 平台枚举、自动识别、能力矩阵、资源限制和功能裁剪宏已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_platform_check.h` | 完成 | profile 能力依赖、工具/sandbox/network/storage 编译期检查已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_session_store.h` | 完成 | session store vtable、消息/tool 持久化和返回数组所有权已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_storage_factory.h` | 完成 | session store factory 的配置借用、输出所有权和 SDK adapter 边界已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_artifact_store.h` | 完成 | artifact store vtable、put/get/list/remove 所有权和多模态资源边界已补中文注释。 |
| `cclaw/ports/include/cc/ports/cc_memory_tool_factory.h` | 完成 | memory 工具创建和 memory store factory 的 adapter 选择语义已补中文注释。 |
| `cclaw/core/include/cc/app/cc_agent_runtime.h` | 完成 | runtime limits、配置、依赖注入、run/stream 入口、取消和落库语义已补中文注释。 |
| `cclaw/core/include/cc/app/cc_cancel_token.h` | 完成 | cancel source/token 生命周期、线程安全取消和可选 token 语义已补中文注释。 |
| `cclaw/core/src/app/cc_cancel_token.c` | 完成 | cancel source/token 内部结构、构造回滚、销毁和查询路径已逐函数注释。 |
| `cclaw/core/include/cc/app/cc_runtime_features.h` | 完成 | runtime feature set、诊断、provider/tool/storage/plugin/MCP factory 扩展点已补中文注释。 |
| `cclaw/core/include/cc/app/cc_app_features.h` | 完成 | 默认 feature set 静态描述符入口已补中文注释。 |
| `cclaw/core/include/cc/app/cc_runtime_builder.h` | 完成 | runtime builder、reload report、热重载、shutdown 和组件访问 API 已补中文注释。 |
| `cclaw/core/include/cc/app/cc_context_builder.h` | 完成 | LLM 上下文构建、历史消息/系统提示合并和返回数组所有权已补中文注释。 |
| `cclaw/core/include/cc/app/cc_session_manager.h` | 完成 | session manager 的 store 持有、会话确保、消息追加和列举所有权已补中文注释。 |
| `cclaw/core/include/cc/app/cc_run_queue.h` | 完成 | run queue lane/action、并发配置、任务提交/收集/中断和取消 token 语义已补中文注释。 |
| `cclaw/core/include/cc/app/cc_tool_executor.h` | 完成 | 工具执行选项、schema/policy/approval/限流/timeout 和结果错误语义已补中文注释。 |
| `cclaw/core/include/cc/app/cc_tool_executor_pool.h` | 完成 | 工具执行池 lane policy、ticket、acquire/release/cancel 和 timeout 查询已补中文注释。 |
| `cclaw/core/include/cc/app/cc_tool_registry_snapshot.h` | 完成 | registry 快照、generation、引用计数和 reload 期间旧 run 生命周期已补中文注释。 |
| `cclaw/core/include/cc/app/cc_memory_context.h` | 完成 | memory 检索注入、prompt block 输出所有权和 vector adapter 解耦语义已补中文注释。 |
| `cclaw/core/include/cc/app/cc_agent_manager.h` | 完成 | 多 agent 路由、run queue 调度、同步/异步提交、切换/中断/列举 API 已补中文注释。 |
| `cclaw/core/include/cc/app/cc_plugin_protocol.h` | 完成 | plugin JSON-RPC 请求构造、响应解析和 result/error 释放 API 已补中文注释。 |
| `cclaw/core/include/cc/app/cc_mcp_runtime_manager.h` | 完成 | MCP manager、transport vtable、factory、工具加载和 JSON-RPC id 匹配已补中文注释。 |
| `cclaw/core/include/cc/app/cc_sse_parser.h` | 完成 | SSE parser、data 回调、feed/finish 和跨 chunk 缓存语义已补中文注释。 |
| `cclaw/core/include/cc/app/cc_skill_catalog.h` | 完成 | skill catalog 加载、prompt 构建、allowlist 和名称列举所有权已补中文注释。 |
| `cclaw/core/include/cc/util/cc_config.h` | 完成 | 主配置、agents/tools/plugins/MCP/multimodal 嵌套结构和加载/校验/销毁 API 已补中文注释。 |
| `cclaw/core/include/cc/util/cc_json.h` | 完成 | JSON AST 不透明类型、解析/序列化、对象数组访问和 create/set 所有权已补中文注释。 |
| `cclaw/core/include/cc/util/cc_string_builder.h` | 完成 | string builder 缓冲所有权、append/take/deinit/clear/cstr 语义已补中文注释。 |
| `cclaw/core/include/cc/util/cc_memory.h` | 完成 | SDK allocator 封装、字符串复制、realloc/free 和内存统计扩展点已补中文注释。 |
| `cclaw/core/include/cc/util/cc_redaction.h` | 完成 | JSON-aware redaction、敏感 key 和返回字符串所有权已补中文注释。 |
| `cclaw/core/include/cc/util/cc_network_policy.h` | 完成 | 网络 allowlist、private network 默认拒绝、URL 匹配和 redirect 复校验已补中文注释。 |
| `cclaw/core/include/cc/util/cc_token_counter.h` | 完成 | 粗略 token 估算、messages JSON 估算和嵌入式轻量化取舍已补中文注释。 |
| `cclaw/adapters/include/cc/adapters/cc_builtin_tools.h` | 完成 | 文件读写工具、HTTP 工具、network policy/allowlist 创建和 URL 检查 API 已补中文注释。 |
| `cclaw/adapters/include/cc/adapters/cc_default_policy_engine.h` | 完成 | 默认 policy engine 创建、shell 审批和 destroy 所有权已补中文注释。 |
| `cclaw/adapters/include/cc/adapters/cc_http_llm_provider.h` | 完成 | HTTP LLM 协议 vtable、请求所有权、stream 格式和通用 provider 创建已补中文注释。 |
| `cclaw/adapters/include/cc/adapters/cc_llm_providers.h` | 完成 | OpenAI/Ollama/Anthropic provider 创建函数和参数所有权已补中文注释。 |
| `cclaw/core/src/util/cc_memory.c` | 完成 | allocator 封装、累计分配统计、strdup/realloc/free 语义已逐函数注释。 |
| `cclaw/core/src/util/cc_string_builder.c` | 完成 | builder 初始化、扩容、append/appendf/take/deinit/clear/cstr 已逐函数注释。 |
| `cclaw/core/src/util/cc_token_counter.c` | 完成 | ASCII/UTF-8 粗略 token 估算和 messages JSON 估算入口已逐函数注释。 |
| `cclaw/core/src/util/cc_redaction.c` | 完成 | 敏感 key 匹配、JSON 递归脱敏、非法 JSON fallback 和统一脱敏入口已逐函数注释。 |
| `cclaw/core/src/util/cc_network_policy.c` | 完成 | URL/allowlist 解析、host/port/scheme 匹配、private network 拒绝和策略入口已逐函数注释。 |
| `cclaw/core/src/util/cc_logger.c` | 完成 | logger 创建/销毁、级别映射、线程安全输出、格式化和 redaction 已逐函数注释。 |
| `cclaw/core/src/util/cc_json.c` | 完成 | cJSON 封装、parse/stringify、对象数组遍历、类型判断和节点创建/挂接已逐函数注释。 |
| `cclaw/core/src/util/cc_config.c` | 完成 | 配置默认值、JSON section parser、嵌套结构释放、校验、加载覆盖和 system prompt 构建已逐函数注释。 |
| `cclaw/core/src/app/cc_plugin_protocol.c` | 完成 | plugin JSON-RPC 请求构造、响应 result/error 拆分和释放函数已逐函数注释。 |
| `cclaw/core/src/app/cc_sse_parser.c` | 完成 | SSE parser 缓冲、行处理、event flush、feed/finish 生命周期已逐函数注释。 |
| `cclaw/core/src/app/cc_memory_context.c` | 完成 | memory 检索失败降级、top-10 prompt block 构造和输出所有权已逐函数注释。 |
| `cclaw/core/src/app/cc_session_manager.c` | 完成 | session id/time helper、manager 创建/销毁、ensure/append/list 行为已逐函数注释。 |
| `cclaw/core/src/app/cc_tool_registry_snapshot.c` | 完成 | registry snapshot generation、引用计数、可选 registry 所有权和 release 销毁路径已逐函数注释。 |
| `cclaw/core/src/app/cc_tool_executor_pool.c` | 完成 | tool lane、执行池创建/销毁、动态 lane、acquire/cancel/release/timeout 已逐函数注释。 |
| `cclaw/core/src/app/cc_agent_manager.c` | 完成 | agent 路由表、pending run、submit/collect、current agent、interrupt/reset/list 已逐函数注释。 |
| `cclaw/core/src/app/cc_context_builder.c` | 完成 | message_vec、历史 token 估算、LLM 压缩回退、system/memory 头部和最终上下文构建已逐函数注释。 |
| `cclaw/core/src/app/cc_tool_executor.c` | 完成 | 工具 observability、policy/schema 错误结果、JSON Schema 最小校验、lane 映射和执行主路径已逐函数注释。 |
| `cclaw/core/src/app/cc_run_queue.c` | 完成 | job/session 状态、lane/session 并发、worker loop、submit/collect/run/interrupt/pending 计数已逐函数注释。 |
| `cclaw/core/src/app/cc_skill_catalog.c` | 完成 | skill entry/catalog、目录加载、upsert、配置来源、allowlist prompt 和名称列举已逐函数注释。 |
| `cclaw/core/src/app/cc_mcp_runtime_manager.c` | 完成 | MCP server runtime、JSON-RPC 请求/匹配、初始化、tool adapter、tools/list 注册和诊断降级已逐函数注释。 |
| `cclaw/core/src/app/cc_agent_runtime_internal.h` | 完成 | runtime 内部结构、tool step helper 和完整 assistant final 落库边界已补中文注释。 |
| `cclaw/core/src/app/cc_agent_runtime.c` | 完成 | runtime 创建/销毁、同步/流式主循环、工具步骤、观测事件、取消/限流和 partial 不落库边界已逐函数注释。 |
| `cclaw/core/src/app/cc_runtime_builder.c` | 完成 | builder 组合根、feature 工厂、工具 registry/pool、skills、热重载 generation 退休和销毁顺序已逐函数注释。 |
| `cclaw/adapters/src/sandbox/cc_policy_engine_impl.c` | 完成 | 默认 policy engine、shell/file_delete 审批策略、decision reason 所有权和 vtable 封装已逐函数注释。 |
| `cclaw/adapters/src/tools/common/cc_file_read_tool.c` | 完成 | file_read 工具对象、schema、workspace 路径边界、可恢复工具错误和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/tools/common/cc_file_write_tool.c` | 完成 | file_write 工具对象、schema、parent 目录边界检查、可恢复写入错误和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/tools/common/cc_http_request_tool.c` | 完成 | http.request 工具对象、network policy 深拷贝、allowlist 检查、HTTP 端口调用和可恢复状态错误已逐函数注释。 |
| `cclaw/adapters/src/tools/common/cc_memory_tool.c` | 完成 | memory 工具对象、operation 分发、memory store 端口调用、可恢复业务错误和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_memory_session_store.c` | 完成 | 内存 session store、消息/tool 记录深拷贝、mutex 临界区、数组扩容/压缩和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_inmem_memory_store.c` | 完成 | 内存 memory store、key/category/session 过滤、结构化 query、score、数组所有权和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_memory_artifact_store.c` | 完成 | 内存 artifact store、artifact 深拷贝、按 session 过滤、数组扩容/删除压缩和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_storage_factory.c` | 完成 | session store 工厂、编译期后端裁剪、storage_type 分发、SQLite 降级和 out_store 所有权已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_memory_store_factory.c` | 完成 | memory store 工厂、noop/inmem/json/sqlite 后端选择、编译期裁剪和长期记忆扩展边界已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_json_file_store.c` | 完成 | JSON 文件 session store、load/modify/save 临界区、消息/tool 记录序列化、session 过滤克隆和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_json_file_memory_store.c` | 完成 | JSON 文件 memory store、启动加载、整体保存、key/category 检索、删除压缩和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_sqlite_session_store.c` | 完成 | SQLite session store、schema/pragma、兼容迁移、prepared statement、事务清理和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/storage/cc_sqlite_memory_store.c` | 完成 | SQLite memory store、表/索引初始化、set/get/search/list/delete、updated_at 绑定和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/llm/cc_http_llm_provider.c` | 完成 | 通用 HTTP LLM provider、HTTP 错误 detail、Retry-After、SSE/NDJSON stream framing、capabilities 和 vtable 创建/销毁已逐函数注释。 |
| `cclaw/adapters/src/llm/cc_openai_provider.c` | 完成 | OpenAI protocol、header 构造、多模态 content part 转换、同步响应解析、SSE delta/tool chunk 映射和 provider 创建已逐函数注释。 |
| `cclaw/adapters/src/llm/cc_anthropic_provider.c` | 完成 | Anthropic protocol、messages/system/tools 转换、多模态 block/fallback、同步响应和 stream event 映射已逐函数注释。 |
| `cclaw/adapters/src/llm/cc_ollama_provider.c` | 完成 | Ollama protocol、NDJSON stream、images 数组转换、同步/流式 tool call 映射和 provider 创建已逐函数注释。 |
| `cclaw/platforms/posix/src/cc_posix_thread.c` | 完成 | POSIX thread/mutex/cond 不透明句柄、递归 mutex、join-once 语义和 timedwait deadline 已逐函数注释。 |
| `cclaw/platforms/posix/src/cc_posix_path.c` | 完成 | POSIX 路径拼接、存在/不存在路径 canonical、workspace prefix 边界、dirname 和 exists 已逐函数注释。 |
| `cclaw/platforms/posix/src/cc_posix_filesystem.c` | 完成 | POSIX filesystem 端口、read/write/exists/list/mkdir/remove、返回数组所有权和默认端口创建已逐函数注释。 |
| `cclaw/platforms/posix/src/cc_posix_process.c` | 完成 | POSIX fork/exec 命令执行、非阻塞 stdout/stderr 捕获、timeout kill、管道进程读写/取消/销毁已逐函数注释。 |
| `cclaw/platforms/posix/src/cc_curl_http_client.c` | 完成 | POSIX curl HTTP client、body/header 回调、stream on_body、Retry-After header 保存、timeout/cancel 和响应释放已逐函数注释。 |
| `cclaw/platforms/esp32/src/cc_esp32_thread.c` | 完成 | ESP32 FreeRTOS task 包装、join 信号量、mutex、二值信号量条件变量和 broadcast 退化语义已逐函数注释。 |
| `cclaw/platforms/esp32/src/cc_esp32_path.c` | 完成 | ESP32 路径拼接、realpath fallback、PATH_MAX 预算、workspace 边界、dirname 和 VFS exists 已逐函数注释。 |
| `cclaw/platforms/esp32/src/cc_esp32_filesystem.c` | 完成 | ESP32 VFS filesystem、read/write/exists/list/mkdir/remove、目录数组所有权和默认端口创建已逐函数注释。 |
| `cclaw/platforms/esp32/src/cc_esp32_http_client.c` | 完成 | ESP32 esp_http_client、事件式 header/body、stream on_body、cancel/max_response_bytes 和响应释放已逐函数注释。 |
| `cclaw/platforms/freertos/src/cc_freertos_thread.c` | 完成 | FreeRTOS task 包装、可配置栈、join 信号量、mutex 和二值信号量条件变量已逐函数注释。 |
| `cclaw/platforms/freertos/src/cc_freertos_path.c` | 完成 | FreeRTOS 轻量路径拼接、词法 canonical、prefix 边界、dirname 和无文件存在 API 的裁剪语义已逐函数注释。 |
| `cclaw/platforms/freertos/src/cc_freertos_filesystem.c` | 完成 | FreeRTOS FatFs/unsupported filesystem、路径映射、父目录创建、读写/list/remove 和裁剪错误语义已逐函数注释。 |
| `cclaw/platforms/freertos/src/cc_freertos_lwip_http_client.c` | 完成 | FreeRTOS lwIP HTTP/1.0 client、URL/DNS、可选 mbedTLS、transport 读写、响应缓存和释放已逐函数注释。 |
| `cclaw/platforms/windows/src/cc_windows_thread.c` | 完成 | Windows thread HANDLE、CRITICAL_SECTION mutex、CONDITION_VARIABLE wait/signal/broadcast 已逐函数注释。 |
| `cclaw/platforms/windows/src/cc_windows_path.c` | 完成 | Windows 路径拼接、GetFullPathName canonical、大小写不敏感 workspace 边界、dirname 和 exists 已逐函数注释。 |
| `cclaw/platforms/windows/src/cc_windows_filesystem.c` | 完成 | Windows filesystem、stdio read/write、FindFirstFile list、CreateDirectory 递归 mkdir、DeleteFile 和默认端口创建已逐函数注释。 |
| `cclaw/platforms/windows/src/cc_windows_process.c` | 完成 | Windows CreateProcess、命令行参数引用、环境块、stdout/stderr 捕获、管道进程读写/取消/销毁已逐函数注释。 |
| `cclaw/platforms/windows/src/cc_curl_http_client.c` | 完成 | Windows curl HTTP client、body/header 回调、stream on_body、timeout/cancel、header 保存和响应释放已逐函数注释。 |
| `cclaw/tests/core/test_thread_mutex.c` | 完成 | 线程/互斥基础并发测试、共享计数器临界区和最终计数断言契约已逐函数注释。 |
| `cclaw/tests/core/test_event_bus_concurrent.c` | 完成 | event bus 并发 publish、handler 内重入 nested publish 和事件计数契约已逐函数注释。 |
| `cclaw/tests/core/test_event_bus_async.c` | 完成 | 异步 event bus 的 handler 隔离、FIFO、背压、destroy drain 和 nested publish 契约已逐函数注释。 |
| `cclaw/tests/core/test_cancel_token.c` | 完成 | cancel source/token 生命周期、借用 token、初始未取消和重复 cancel 幂等契约已逐函数注释。 |
| `cclaw/tests/core/test_result_detail.c` | 完成 | 结构化错误 detail 深拷贝、HTTP 429 metadata、recoverable 和 raw_redacted_body 契约已逐函数注释。 |
| `cclaw/tests/core/test_redaction.c` | 完成 | JSON-aware redaction、fallback 文本脱敏、大小写敏感 key 覆盖和 event bus payload 脱敏已逐函数注释。 |
| `cclaw/tests/core/test_observability_schema.c` | 完成 | observability schema、error detail、attributes object、publish redaction 和事件族稳定性已逐函数注释。 |
| `cclaw/tests/core/test_observability_no_legacy_events.c` | 完成 | 业务路径禁止旧事件名和直接 cc_event_bus_publish 的静态回归测试已逐函数注释。 |
| `cclaw/tests/core/test_sse_parser.c` | 完成 | SSE parser 注释行、跨 feed 拼接、多行 data 合并、DONE 和 finish 尾部处理已逐函数注释。 |
| `cclaw/tests/core/test_plugin_protocol_envelope.c` | 完成 | plugin JSON-RPC 请求 envelope、result/error 响应解析和输出所有权已逐函数注释。 |
| `cclaw/tests/core/test_memory_query_port.c` | 完成 | memory query port fallback、fake search store、category/session 过滤、score 和结果所有权已逐函数注释。 |
| `cclaw/tests/core/test_path_security.c` | 完成 | workspace 路径安全、目录穿越、prefix 绕过和 symlink 指向外部 parent 检查已逐函数注释。 |
| `cclaw/tests/core/test_config_missing_defaults.c` | 完成 | 缺失配置默认值、模型/UI 覆盖、thinking/stream 关系和 api_key_env 优先级测试已逐函数注释。 |
| `cclaw/tests/core/test_config_runtime_sections.c` | 完成 | queue/agents/tools/plugins/skills/MCP/memory/multimodal 配置解析和非法配置校验已逐函数注释。 |
| `cclaw/tests/core/test_tool_registry_freeze.c` | 完成 | dummy tool vtable、registry freeze 后禁止写入和多线程 find/schema 读路径测试已逐函数注释。 |
| `cclaw/tests/core/test_runtime_request_config.c` | 完成 | runtime 同步/stream request 配置透传、summary 压缩配置和 fake LLM 记录断言已逐函数注释。 |
| `cclaw/tests/core/test_runtime_concurrent_sessions.c` | 完成 | 共享 runtime 多线程独立 session 执行、fake LLM 和内存 session store 并发契约已逐函数注释。 |
| `cclaw/tests/core/test_runtime_stream_callback.c` | 完成 | stream callback 实时输出、observability 独立事件、finished schema 和 max_stream_bytes 超限错误已逐函数注释。 |
| `cclaw/tests/core/test_logger_concurrent.c` | 完成 | 多线程共享 logger 写日志、内部锁和销毁边界测试已逐函数注释。 |
| `cclaw/tests/core/test_run_queue_session_serial.c` | 完成 | run queue 同 session per_session_concurrency 串行化和 in_flight 收敛测试已逐函数注释。 |
| `cclaw/tests/core/test_run_queue_async_interrupt.c` | 完成 | run queue 异步 submit、run_id interrupt、cancel token 传播、collect 取消结果和队列计数收敛已逐函数注释。 |
| `cclaw/tests/core/test_tool_executor_pool_lane.c` | 完成 | tool executor pool per-tool lane 并发、timeout 查询和 acquire cancel 传播已逐函数注释。 |
| `cclaw/tests/core/test_skill_catalog_prompt.c` | 完成 | skill catalog 从配置目录加载 SKILL.md、allowlist prompt 构建和未允许技能排除已逐函数注释。 |
| `cclaw/tests/core/test_mcp_jsonrpc_matcher.c` | 完成 | MCP JSON-RPC 数字/字符串 id 响应匹配和错配检测测试已逐函数注释。 |
| `cclaw/tests/core/test_mcp_runtime_manager_fake_transport.c` | 完成 | MCP fake transport、工具加载/调用、cancel 拦截、TTL reset/reinitialize 和 diagnostics 降级已逐函数注释。 |
| `cclaw/tests/adapters/test_tool_schema_validation.c` | 完成 | tool executor JSON Schema required/type/enum/additionalProperties 校验和 call 不执行契约已逐函数注释。 |
| `cclaw/tests/adapters/test_tool_executor_policy_approval.c` | 完成 | tool executor policy/approval 测试、无 handler deny、handler deny/approve、缺失工具错误和 observability 事件已逐函数注释。 |
| `cclaw/tests/adapters/test_http_allowlist.c` | 完成 | HTTP/network allowlist 纯函数测试、默认 deny、host/port/scheme/wildcard/userinfo/private IP 语义已逐函数注释。 |
| `cclaw/tests/adapters/test_memory_store_concurrent.c` | 完成 | 内存 session store 多线程追加、消息深拷贝、load_messages 返回数组所有权和 join 后断言已逐函数注释。 |
| `cclaw/tests/adapters/test_message_envelope_serialization.c` | 完成 | message envelope 历史消息、tool_calls/reasoning_content、context builder 和 JSON 非二次转义契约已逐函数注释。 |
| `cclaw/tests/adapters/test_multimodal_content_parts.c` | 完成 | 多模态 content parts、artifact 深拷贝、tool result artifact 和 context builder JSON 保真已逐函数注释。 |
| `cclaw/tests/platform/posix/test_process_capture.c` | 完成 | POSIX process 大 stdout 捕获、pipe 读取、防死锁、timeout 和 process result 语义已逐函数注释。 |
| `cclaw/tests/packaging/consumer/main.c` | 完成 | packaging consumer fixture、聚合头 include、安装导出目标和版本符号 smoke test 已逐函数注释。 |

后续每处理完一个文件就追加到本表并同步更新“已完成”计数。
