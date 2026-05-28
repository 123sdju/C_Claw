# 工具学习文档

工具是 Agent 能力扩展点。LLM 不能直接读文件、发 HTTP 或操作设备，它只能返回
`tool_call`；runtime 再根据工具注册表找到对应 `cc_tool_t` 并执行。

## 1. 工具端口

工具端口定义在：

```c
#include "cc/ports/cc_tool.h"
```

一个工具由 `self + vtable` 组成：

- `self`：工具实现的私有状态。
- `name()`：返回工具名，必须稳定。
- `description()`：返回描述，给 provider 生成工具 schema。
- `parameters_schema_json()`：返回 JSON Schema。
- `call()`：执行工具。
- `destroy()`：释放工具私有状态。

这种写法相当于 C 语言里的接口。runtime 不知道工具内部实现，只按 vtable 调用。

## 2. 工具参数为什么仍是 JSON

多模态消息已经改成 typed model，但工具参数仍是 JSON 字符串。原因是工具参数天然由
JSON Schema 描述，且每个工具结构都不同：

```json
{
  "type": "object",
  "properties": {
    "path": { "type": "string" }
  },
  "required": ["path"]
}
```

这部分 JSON 是“工具参数协议”，不是旧的多模态 content JSON API。

## 3. 工具结果

工具结果使用 `cc_tool_result_t`：

```c
typedef struct cc_tool_result {
    int ok;
    char *text;
    char *error;
    char *metadata;
    cc_media_artifact_list_t artifacts;
} cc_tool_result_t;
```

字段含义：

- `ok`：非 0 表示工具成功。
- `text`：成功时返回给模型看的文本结果。
- `error`：失败时返回给模型看的错误信息。
- `metadata`：审计或调试信息，建议 JSON 字符串。
- `artifacts`：图片、音频、视频或文件输出。

释放时调用：

```c
cc_tool_result_cleanup(&result);
```

## 4. 返回 artifact 的工具

工具可以同时返回文本和媒体：

```c
cc_tool_result_t result = {0};
result.ok = 1;
result.text = strdup("生成了一张图片");

cc_media_artifact_t image;
cc_media_artifact_init(&image);
image.id = strdup("img_1");
image.kind = CC_MEDIA_IMAGE;
image.mime = strdup("image/png");
image.path = strdup("/tmp/out.png");

cc_tool_result_add_artifact(&result, &image);

cc_media_artifact_cleanup(&image);
cc_tool_result_cleanup(&result);
```

`cc_tool_result_add_artifact()` 会深拷贝 artifact。

## 5. runtime 如何处理工具结果

工具执行后，runtime 会做几件事：

1. 写入 tool call 审计记录。
2. 写入 tool result 审计记录。
3. 创建 `CC_ROLE_TOOL` 消息，把 `result.text` 或 `result.error` 放回对话历史。
4. 如果有 artifacts，追加 artifact observation，让下一轮模型能看到媒体摘要。

这样即使 provider 不支持多模态，也能从摘要中理解工具产物。

## 6. 策略和审批

`cc_policy_engine_t` 在工具执行前做安全判断：

- 允许：直接执行。
- 拒绝：返回错误，不执行工具。
- 需要审批：调用 gateway 注入的人工审批回调。

高风险工具应同时依赖：

- policy；
- sandbox；
- workspace 限制；
- 应用层权限；
- 平台能力裁剪。

内置默认策略把 `shell_run` / `shell.run` 和删除类文件工具视为高风险；需要审批但没有
approval handler 时，runtime 会返回可恢复 tool error，不执行工具。

## 7. 网络工具 allowlist

`http.request` 默认拒绝所有 URL。应用可以通过 `tools.networkAllowlist` 或
`cc_http_request_tool_create_with_allowlist()` 显式允许 host：

```json
{
  "tools": {
    "networkAllowlist": ["api.example.com", "*.trusted.local", "https://secure.example.com"]
  }
}
```

支持精确 host、`*.domain` 和带 scheme 的 origin。SDK 不提供业务 gateway，也不保存 API key
分发策略；这些属于下游应用。

allowlist entry 支持 `host`、`host:port`、`*.domain`、`scheme://host`、
`scheme://host:port`。URL 含 userinfo、scheme 不匹配、port 不匹配、格式非法都会拒绝。
`localhost`、loopback、private IPv4、link-local、IPv6 loopback 默认拒绝；只有显式 entry
匹配时才允许。HTTP adapter 如果跟随 redirect，必须对最终 URL 重新执行同一 policy。

## 8. 日志和事件脱敏

logger 和 event bus 会对基础敏感字段做 redaction，包括 `api_key`、`authorization`、
`token`、`secret`、`password`。合法 JSON 会优先按 object/array 递归处理；非法 JSON 才走
文本 fallback。工具 metadata 仍应避免写入不必要的敏感内容。

## 9. 嵌入式面试关注点

工具系统体现了 C 语言接口、多态、安全策略和资源控制：

- `cc_tool_t` 是 `self + vtable`，具体工具可以是文件、HTTP、memory 或设备私有能力。
- tool executor 不知道工具内部实现，只按 vtable 调用。
- policy engine 是策略模式，高风险工具可以 require approval。
- 工具参数先按 JSON Schema 最小子集校验，再执行。
- tool result 有最大字节限制，避免设备内存被大输出耗尽。

面试表达：

> 工具不是让模型直接操作系统，而是通过 tool registry 查找 C 函数接口，再经过 schema 校验、
> policy、approval、timeout 和资源限制。这个链路适合设备端暴露受控能力。

常见追问：

- 为什么工具参数还是 JSON？
  - 因为工具 schema 本身就是 JSON Schema，参数形状由工具定义，typed struct 反而会限制扩展。
- 为什么工具失败不一定让 run 失败？
  - 参数错误、权限拒绝这类错误可以反馈给模型，让模型下一轮修正或换方案。
- 设备私有工具怎么接？
  - 应用实现 `cc_tool_vtable_t`，注册到 registry，SDK core 不需要知道具体硬件。
