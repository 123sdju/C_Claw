# C-Claw SDK 接口说明

本文只说明 SDK 对下游应用暴露的接口边界。应用示例、产品说明和旧平台应用文档不在
本分支保留。

## 1. 构建目标

SDK 主要输出静态库目标：

```cmake
add_subdirectory(cclaw)
target_link_libraries(my_agent PRIVATE c_claw_runtime)
```

`c_claw_runtime` 包含 core、adapters 和当前平台实现。应用不应直接链接内部 object
target。

## 2. Runtime Builder

头文件：

```c
#include "cc/app/cc_runtime_builder.h"
```

核心接口：

```c
cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
);

cc_agent_runtime_t *cc_runtime_builder_runtime(cc_runtime_builder_t *builder);
cc_agent_manager_t *cc_runtime_builder_agent_manager(cc_runtime_builder_t *builder);
const cc_runtime_diagnostics_t *cc_runtime_builder_diagnostics(cc_runtime_builder_t *builder);

cc_result_t cc_runtime_builder_reload(
    cc_runtime_builder_t *builder,
    const cc_config_t *config
);

void cc_runtime_builder_request_shutdown(cc_runtime_builder_t *builder);
void cc_runtime_builder_destroy(cc_runtime_builder_t *builder);
```

所有权规则：

- `config` 和 `features` 由应用持有，builder 只借用。
- builder 创建的 runtime、store、provider、registry、manager 等资源由 builder 释放。
- 从 builder 取出的 runtime/manager/logger 指针都是借用指针，builder 销毁后立即失效。

## 3. Feature Set

头文件：

```c
#include "cc/app/cc_runtime_features.h"
```

应用必须提供 `cc_runtime_feature_set_t`：

```c
typedef struct cc_runtime_feature_set {
    const cc_llm_provider_descriptor_t *llm_providers;
    size_t llm_provider_count;

    const cc_tool_descriptor_t *tools;
    size_t tool_count;

    cc_runtime_session_store_create_fn create_session_store;
    cc_runtime_memory_store_create_fn create_memory_store;
    cc_runtime_policy_create_fn create_policy_engine;
    cc_runtime_sandbox_create_fn create_sandbox;

    cc_runtime_plugin_load_fn load_plugins;
    cc_runtime_plugin_destroy_fn destroy_plugins;
    cc_runtime_mcp_load_fn load_mcp;
    cc_runtime_mcp_destroy_fn destroy_mcp;
} cc_runtime_feature_set_t;
```

`cc_runtime_builder_create()` 只通过这张表发现能力。SDK 不提供默认 gateway，也不替应用决定
启用哪些 provider 或 tool。

## 4. 配置接口

头文件：

```c
#include "cc/util/cc_config.h"
```

常用接口：

```c
cc_result_t cc_config_load(const char *path, cc_config_t *out_config);
void cc_config_destroy(cc_config_t *config);
```

应用决定配置文件位置。配置可包含 `model`、`storage`、`agents`、`queue`、`tools`、
`plugins`、`skills`、`mcp`、`memory`、`sandbox`、`system` 等段。运行时配置不能打开
编译期关闭的能力。

## 5. Agent Runtime

头文件：

```c
#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_agent_manager.h"
```

单 runtime 可直接处理消息；启用 multi-agent/run-queue 时，应用应优先使用
`cc_agent_manager_t`，以复用 session 串行、lane 并发和 cancel token 语义。

常用接口：

```c
cc_result_t cc_agent_manager_handle_message(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    char **out_response
);

cc_result_t cc_agent_manager_submit(
    cc_agent_manager_t *manager,
    const char *agent_id,
    const char *session_id,
    const char *user_input,
    cc_run_id_t *out_run_id
);

cc_result_t cc_agent_manager_collect(
    cc_agent_manager_t *manager,
    cc_run_id_t run_id,
    char **out_response
);
```

`out_response` 成功时由调用方释放。

## 6. Provider 接口

端口头文件：

```c
#include "cc/ports/cc_llm_provider.h"
```

SDK 内置 HTTP provider 工厂：

```c
#include "cc/adapters/cc_llm_providers.h"

cc_result_t cc_openai_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);

cc_result_t cc_ollama_provider_create(
    const char *base_url,
    const char *model,
    cc_llm_provider_t *out_provider
);

cc_result_t cc_anthropic_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);
```

应用把 provider 工厂包装成 `cc_runtime_llm_create_fn`，再写入
`cc_llm_provider_descriptor_t`。

## 7. Tool 接口

端口头文件：

```c
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"
```

SDK 内置工具工厂：

```c
#include "cc/adapters/cc_builtin_tools.h"

cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
cc_result_t cc_http_request_tool_create(cc_tool_t *out_tool);
```

记忆工具工厂：

```c
#include "cc/ports/cc_memory_tool_factory.h"

cc_result_t cc_memory_tool_create(cc_memory_store_t *store, cc_tool_t *out_tool);
```

工具创建成功后，应用通常把 `cc_tool_t` 交给 runtime builder 注册；注册失败时必须调用
tool vtable 的 `destroy` 清理。

## 8. Storage 和 Memory 接口

会话存储：

```c
#include "cc/ports/cc_storage_factory.h"

cc_result_t cc_storage_factory_create_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
);
```

长期记忆：

```c
#include "cc/ports/cc_memory_tool_factory.h"

cc_result_t cc_memory_store_factory_create(
    cc_memory_store_t *out_store,
    const char *backend,
    const char *path
);
```

支持的 memory backend：`json_file`、`sqlite`、`inmem`、`noop`/`none`。

## 9. Policy 和 Sandbox 接口

默认 policy 工厂：

```c
#include "cc/adapters/cc_default_policy_engine.h"

cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);
```

Sandbox 是端口接口：

```c
#include "cc/ports/cc_sandbox.h"
```

SDK 分支不提供 shell 工具实现；如果下游应用提供高风险工具，应自行实现 sandbox 或传入
自己的 `cc_runtime_sandbox_create_fn`。

## 10. Plugin 和 MCP 边界

Plugin：

```c
#include "cc/app/cc_plugin_protocol.h"
```

SDK 只提供 JSON-RPC 2.0 envelope 编解码。进程启动、stdin/stdout pipe、worker pool、
超时和崩溃重启由应用实现。

MCP：

```c
#include "cc/app/cc_mcp_runtime_manager.h"
#include "cc/app/cc_sse_parser.h"
```

SDK 提供 MCP runtime、JSON-RPC id 匹配、TTL、tool bridge 和 SSE parser。stdio/HTTP
transport 由应用实现并通过 feature set 注入。

## 11. 平台端口

平台端口集中在：

```text
cclaw/ports/include/cc/ports/
```

常见端口：

- `cc_filesystem_t`
- `cc_path`
- `cc_thread_t` / `cc_mutex_t` / `cc_cond_t`
- `cc_process_t`
- `cc_http_client_t`
- `cc_event_bus_t`

新增平台时，只在 `cclaw/platforms/<platform>` 中实现这些端口，不要让 core 或 adapter
直接包含设备 SDK 头文件。
