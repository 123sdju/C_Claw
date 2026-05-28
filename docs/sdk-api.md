# SDK 接口总览

本文是 C-Claw SDK 的接口索引。应用层只依赖 public header，不直接访问 `*_internal.h`、
`src/` 实现文件或 vendor header。`cc/cclaw.h` 是聚合入口；`cc/core/cc_version.h`
是版本入口。

稳定 public header 收口为 `cc/cclaw.h`、`cc/core/*`、必要的 `cc/app/*`、`cc/ports/*`
和 adapter 工厂头。发布前允许破坏当前 struct/vtable 布局；长期暴露的 public struct
以 `size_t size` 为首字段，调用方用 `{0}` 初始化后填 `size = sizeof(obj)`。

## 1. 构建入口

```cmake
add_subdirectory(cclaw)
target_link_libraries(my_agent PRIVATE c_claw_runtime)
```

安装后可通过导出包复用：

```cmake
find_package(CClaw CONFIG REQUIRED)
target_link_libraries(my_agent PRIVATE CClaw::runtime)
```

聚合头和版本头：

```c
#include <cc/cclaw.h>
#include <cc/core/cc_version.h>
```

`cc_claw_version_string()` 返回 SDK 版本字符串。`CC_DEPRECATED(message)` 用于后续
public API 软废弃标记。

profile 只设置默认值，命令行仍可覆盖：

```bash
cmake -S . -B build/sdk/mcu-text -DCC_PROFILE=mcu-text -DCC_ENABLE_STREAMING=ON
```

GCC/Clang 下可用 `-DCC_ENABLE_SANITIZERS=ON` 打开 AddressSanitizer 和
UndefinedBehaviorSanitizer；预设名为 `core-minimal-sanitizers`。

## 2. Runtime 和 Builder

头文件：

```c
#include "cc/app/cc_agent_runtime.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_runtime_features.h"
```

应用通常通过 `cc_runtime_builder_create()` 组合 runtime。`config` 和
`cc_runtime_feature_set_t` 由应用持有，builder 只借用；builder 创建的 provider、
store、registry、runtime、manager 由 builder 销毁。

核心调用：

```c
cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
);

cc_agent_runtime_t *cc_runtime_builder_runtime(cc_runtime_builder_t *builder);
cc_agent_manager_t *cc_runtime_builder_agent_manager(cc_runtime_builder_t *builder);
void cc_runtime_builder_destroy(cc_runtime_builder_t *builder);
```

### 2.1 Runtime limits

`cc_agent_runtime_config_t.limits` 提供核心资源限制：

```c
typedef struct cc_runtime_limits {
    size_t size;
    int run_timeout_ms;
    int tool_timeout_ms;
    int provider_timeout_ms;
    size_t max_input_bytes;
    size_t max_output_bytes;
    size_t max_tool_result_bytes;
    size_t max_stream_bytes;
    int max_steps;
    int max_concurrency;
} cc_runtime_limits_t;
```

当前核心层会执行输入大小、输出大小、tool result 大小、stream 字节数和有效
`max_steps` 检查，并把 `provider_timeout_ms` / `tool_timeout_ms` 传入 provider/tool
请求上下文；触发时返回 `CC_ERR_LIMIT_EXCEEDED`。stream 取消、超限或 provider 错误时，
默认不落库 incomplete assistant final，只返回错误和已产生的 callback/event。

### 2.2 Stream callback

旧的 stream API 保留；新 API 允许下游直接接收 chunk，不需要把 event bus 当成实时
输出接口：

```c
typedef void (*cc_agent_runtime_stream_callback_fn)(
    const cc_stream_chunk_t *chunk,
    void *user_data
);

cc_result_t cc_agent_runtime_handle_message_stream_cb(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    const cc_agent_runtime_stream_options_t *options,
    char **out_response
);
```

chunk 类型包含 text、thinking、tool_start、tool_delta、tool_end、finished、artifact、
provider_warning 和 error。

### 2.3 Structured errors

`cc_result_t` 带 `size` 和可选 `cc_error_detail_t *detail`。HTTP provider 会在 detail 中
填充 `http_status`、`retry_after_ms`、`recoverable`、稳定 `error_code` 和 redacted raw
body。SDK 不自动 retry/backoff；下游应用根据错误分类决定恢复策略。

### 2.4 Observability

`cc_observability_event_t` / `cc_observability_publish()` 生成统一事件 payload：
`schema_version`、`event`、`session_id`、`run_id`、`step`、`status`、`error.code`、
`error.message`。标准 family 是 `run.*`、`llm.*`、`tool.*`、`approval.*`、`memory.*`、
`error.*`、`stream.*`。event bus 仍是观测机制，实时输出使用 stream callback。

## 3. 配置模型

头文件：

```c
#include "cc/util/cc_config.h"
```

```c
cc_result_t cc_config_load(const char *path, cc_config_t *out_config);
cc_result_t cc_config_load_default(cc_config_t *out_config);
cc_result_t cc_config_validate(const cc_config_t *config);
void cc_config_destroy(cc_config_t *config);
```

默认文本模式：

- text input/output 永远可用。
- image/audio/video/file input 默认关闭。
- image/audio/video/file output 默认关闭。
- 未配置 `multimodal` 段时不启用多模态。

多模态配置示例：

```json
{
  "multimodal": {
    "input": { "image": true, "file": true },
    "output": { "image": false, "audio": false },
    "limits": {
      "maxArtifacts": 4,
      "maxArtifactBytes": 10485760,
      "maxBase64Bytes": 2097152,
      "allowInlineBase64": false,
      "allowedMimePrefixes": ["image/", "application/pdf"]
    }
  }
}
```

## 4. Typed Message

头文件：

```c
#include "cc/core/cc_message.h"
```

`cc_message_t` 不公开 raw JSON 内容字段。文本也是一个 `cc_content_part_t`。

```c
cc_result_t cc_message_create_text(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const char *text,
    const char *tool_call_id,
    cc_message_t **out_message
);

cc_result_t cc_message_create_parts(
    const char *id,
    const char *session_id,
    cc_message_role_t role,
    const cc_content_parts_t *parts,
    const char *tool_call_id,
    cc_message_t **out_message
);

cc_result_t cc_message_add_content_part(cc_message_t *message, const cc_content_part_t *part);
cc_result_t cc_message_get_text_summary(const cc_message_t *message, char **out_summary);
void cc_message_cleanup(cc_message_t *message);
void cc_message_destroy(cc_message_t *message);
```

## 5. 多模态和 Artifact

头文件：

```c
#include "cc/core/cc_media.h"
```

媒体类型：

```c
typedef enum cc_media_kind {
    CC_MEDIA_TEXT,
    CC_MEDIA_IMAGE,
    CC_MEDIA_AUDIO,
    CC_MEDIA_VIDEO,
    CC_MEDIA_FILE
} cc_media_kind_t;
```

核心结构：

- `cc_content_part_t`：消息中的一个文本或 artifact part。
- `cc_content_parts_t`：part 列表。
- `cc_media_artifact_t`：SDK 拥有字段的媒体元数据和值对象。
- `cc_media_artifact_list_t`：artifact 列表。

### 5.1 所有权规则

`cc_media_artifact_t`、`cc_content_part_t` 和 `cc_content_parts_t` 都是“拥有型”结构：

- 结构里的 `char *` 字段由结构体拥有。
- `append` 和 `copy` 函数会深拷贝输入内容。
- 调用方传入的临时 artifact 可以在 append 后立即 cleanup。
- `cleanup` 函数只释放结构内部字段，不释放结构体指针本身。
- `destroy` 函数才释放通过 create 得到的堆对象。

推荐写法：

```c
cc_media_artifact_t image;
cc_media_artifact_init(&image);
image.id = strdup("img_1");
image.kind = CC_MEDIA_IMAGE;
image.mime = strdup("image/png");
image.path = strdup("/tmp/a.png");

cc_content_parts_t parts;
cc_content_parts_init(&parts);
cc_content_parts_append_text(&parts, "请描述图片", CC_CONTENT_PART_INPUT);
cc_content_parts_append_artifact(&parts, &image, CC_CONTENT_PART_INPUT);

cc_media_artifact_cleanup(&image);
cc_content_parts_cleanup(&parts);
```

### 5.2 artifact 字段说明

| 字段 | 含义 | 由谁使用 |
| --- | --- | --- |
| `id` | artifact 标识，用于关联、日志、store | 应用、runtime、store |
| `kind` | image/audio/video/file | runtime、provider |
| `mime` | MIME 类型，例如 `image/png` | 校验、provider |
| `path` | 本地文件路径引用 | 应用、平台工具 |
| `uri` | 远程或应用私有 URI | provider、gateway |
| `data_base64` | inline base64 内容 | provider，需显式开启 |
| `bytes` | 媒体大小 | limits 校验 |
| `width` / `height` | 图片/视频尺寸 | provider、UI、摘要 |
| `duration_ms` | 音频/视频时长 | provider、UI、摘要 |
| `created_at` | 创建时间 | store、日志 |
| `metadata` | 扩展元数据 | 应用、store |

### 5.3 校验和限制

`cc_media_artifact_validate()` 只做 SDK 层能稳定判断的校验：

- kind 不能是 `CC_MEDIA_TEXT`。
- MIME 必须存在，并匹配 `allowed_mime_prefixes`。
- path 不能包含 `..`。
- `bytes` 不能超过 `max_artifact_bytes`。
- inline base64 必须被 `allow_inline_base64` 允许。
- base64 长度不能超过 `max_base64_bytes`。

SDK 不解码 base64、不打开文件、不探测 URI。这些属于应用层或平台层。

常用接口：

```c
cc_result_t cc_content_parts_append_text(
    cc_content_parts_t *parts,
    const char *text,
    cc_content_part_direction_t direction
);

cc_result_t cc_content_parts_append_artifact(
    cc_content_parts_t *parts,
    const cc_media_artifact_t *artifact,
    cc_content_part_direction_t direction
);

cc_result_t cc_media_artifact_copy(const cc_media_artifact_t *src, cc_media_artifact_t *dst);
cc_result_t cc_media_artifact_validate(const cc_media_artifact_t *artifact, const cc_media_limits_t *limits);
cc_result_t cc_media_artifact_list_summarize(const cc_media_artifact_list_t *list, char **out_summary);
```

## 6. Tool Result 和 LLM Response

头文件：

```c
#include "cc/core/cc_tool_call.h"
```

`cc_tool_result_t` 使用 typed artifacts：

```c
typedef struct cc_tool_result {
    int ok;
    char *text;
    char *error;
    char *metadata;
    cc_media_artifact_list_t artifacts;
} cc_tool_result_t;
```

```c
cc_result_t cc_tool_result_add_artifact(cc_tool_result_t *result, const cc_media_artifact_t *artifact);
cc_result_t cc_tool_result_set_artifacts(cc_tool_result_t *result, const cc_media_artifact_list_t *artifacts);
cc_media_artifact_list_t cc_tool_result_take_artifacts(cc_tool_result_t *result);
void cc_tool_result_cleanup(cc_tool_result_t *result);
```

`cc_llm_response_t` 支持文本、content parts、artifact、多个 tool call 和 reasoning。

## 7. Provider 端口

头文件：

```c
#include "cc/ports/cc_llm_provider.h"
```

Provider 接收 typed messages：

```c
typedef struct cc_llm_chat_request {
    const char *model;
    const cc_message_t *messages;
    size_t message_count;
    const cc_media_limits_t *media_limits;
    const char *tools_json;
    int stream;
    int max_tokens;
    double temperature;
    int thinking_mode;
    cc_cancel_token_t *cancel_token;
    int timeout_ms;
} cc_llm_chat_request_t;
```

Provider 必须在 adapter 内部把 SDK typed model 转换成 OpenAI、Anthropic、Ollama
自己的协议格式。

如果 provider 实现了 `capabilities()`，runtime 创建阶段会校验 text/tool/multimodal
能力，配置要求未满足时返回 `CC_ERR_UNSUPPORTED`，避免运行中静默降级。
正式 stream callback API 要求 provider 支持 streaming；不支持时返回
`CC_ERR_UNSUPPORTED`，旧 stream wrapper 仍可按兼容路径退回同步输出。

HTTP provider 不内置 retry/backoff。429 映射为 `CC_ERR_RATE_LIMIT`，5xx 映射为
`CC_ERR_NETWORK`，取消和 timeout 保留底层 `cc_result_t`，JSON 解析失败返回
`CC_ERR_JSON`。

内置 provider 工厂：

```c
#include "cc/adapters/cc_llm_providers.h"

cc_result_t cc_openai_provider_create(const char *base_url, const char *api_key, const char *model, cc_llm_provider_t *out_provider);
cc_result_t cc_ollama_provider_create(const char *base_url, const char *model, cc_llm_provider_t *out_provider);
cc_result_t cc_anthropic_provider_create(const char *base_url, const char *api_key, const char *model, cc_llm_provider_t *out_provider);
```

## 8. Tool、Policy、Storage

工具端口：

```c
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"
```

核心执行器会在调用 tool 前对 `schema_json()` 做最小 JSON Schema 校验，当前支持
`type: object`、`required`、`properties.*.type`、`enum` 和
`additionalProperties: false`。校验失败会返回 `ok = 0` 的 tool result，具体 tool
不会被调用。

内置工具工厂：

```c
#include "cc/adapters/cc_builtin_tools.h"

cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
cc_result_t cc_http_request_tool_create(cc_tool_t *out_tool);
cc_result_t cc_http_request_tool_create_with_allowlist(
    const cc_config_string_list_t *network_allowlist,
    cc_tool_t *out_tool
);
int cc_http_request_url_allowed(
    const cc_config_string_list_t *network_allowlist,
    const char *url
);
```

`http.request` 默认 deny；只有 `tools.networkAllowlist` / `tools.network_allowlist`
显式允许的 host、`*.domain` 或带 scheme 的 origin 才能访问。

存储端口：

```c
#include "cc/ports/cc_storage_factory.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_artifact_store.h"
```

memory store 保留 `search()`，同时提供更明确的 query 端口：

```c
typedef struct cc_memory_query {
    const char *query;
    const char *category;
    const char *session_id;
    int limit;
} cc_memory_query_t;

typedef struct cc_memory_search_result {
    cc_memory_entry_t entry;
    double score;
} cc_memory_search_result_t;

cc_result_t cc_memory_store_query(
    cc_memory_store_t *store,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count
);
```

具体 embedding/vector store 仍由下游 adapter 实现；核心只定义查询契约和 fallback。

Policy：

```c
#include "cc/adapters/cc_default_policy_engine.h"

cc_result_t cc_policy_engine_create_default(int shell_requires_approval, cc_policy_engine_t *out_engine);
```

## 9. 流式事件与观测事件

头文件：

```c
#include "cc/core/cc_stream_chunk.h"
```

新增多模态相关 chunk：

- `CC_STREAM_CHUNK_ARTIFACT`
- `CC_STREAM_CHUNK_PROVIDER_WARNING`
- `CC_STREAM_CHUNK_ERROR`

应用 gateway 可用 stream callback 将 artifact 输出展示为图片、音频、文件链接或下载入口。
event bus 是观测机制，不是实时输出主通道；实时 UI 应优先使用 stream callback。

业务事件统一通过 `cc_observability_publish()` 生成 schema payload。常用事件名：

- `CC_OBS_EVENT_RUN_FINISHED`
- `CC_OBS_EVENT_LLM_REQUEST_START`
- `CC_OBS_EVENT_LLM_RESPONSE_FINISH`
- `CC_OBS_EVENT_TOOL_START`
- `CC_OBS_EVENT_TOOL_FINISH`
- `CC_OBS_EVENT_APPROVAL_REQUIRED`
- `CC_OBS_EVENT_STREAM_TEXT`
- `CC_OBS_EVENT_STREAM_FINISHED`

事件和 logger 会经过基础敏感字段 redaction，默认隐藏 `api_key`、`authorization`、
`token`、`secret`、`password` 等字段。
