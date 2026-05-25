# Plugin System

插件系统把外部进程暴露为 C-Claw tool。当前边界是：协议语义在 SDK，进程和管道在桌面 app。

## SDK 与 App 边界

SDK 负责：

- `cc_plugin_protocol`：JSON-RPC 2.0 request/response envelope 编解码。
- tool schema bridge：插件声明的 `name`、`description`、`parameters` 被注册为 `cc_tool_t`。
- registry generation：runtime builder reload 成功后发布新工具快照，已经开始的 run 继续使用自己的快照。

POSIX/Windows app 负责：

- 启动外部进程、维护 stdin/stdout pipe、捕获 stderr。
- `workers` 个子进程的 round-robin 分发。
- `timeoutMs`、`restartOnCrash` 等平台行为。
- config polling watcher 和 `/reload` CLI 命令。

ESP profile 默认关闭 `CC_ENABLE_PLUGIN`、`CC_ENABLE_PLUGIN_WORKERS` 和
`CC_ENABLE_PLUGIN_HOT_RELOAD`，不会编译进程型 plugin 代码。

## 配置

插件只从 `config.json.plugins.entries` 加载：

```json
{
  "plugins": {
    "hotReload": true,
    "reloadDebounceMs": 300,
    "entries": {
      "weather": {
        "enabled": true,
        "command": "python3",
        "args": ["apps/posix/cli/plugins/weather_tool.py"],
        "workers": 2,
        "timeoutMs": 30000,
        "maxInFlight": 2,
        "restartOnCrash": true,
        "skills": ["apps/posix/cli/plugins/weather/skills"],
        "tools": [
          {
            "name": "weather.query",
            "description": "Query weather by city",
            "parameters": {
              "type": "object",
              "properties": { "city": { "type": "string" } },
              "required": ["city"]
            }
          }
        ]
      }
    }
  }
}
```

`enabled=false` 的条目会被跳过。loader 会校验启用条目的 `command`、`workers`、
`maxInFlight` 和工具 schema 基本形态；这些属于配置结构错误，启动或 reload 会
直接失败，因为继续运行会得到不可预测的配置语义。

外部进程启动失败属于非致命 tool 诊断：该插件的工具不会注册进本次 registry
generation，但 runtime 会继续启动或完成 reload，其他 builtin/plugin/MCP tool
仍可用。诊断会写入 `cc_runtime_diagnostics_t`，app 在启动后和 `/reload` 后打印摘要。

## 并发模型

单 worker 内部是串行 pipe：同一子进程一次只处理一个 JSON-RPC 调用。

`workers>1` 时，同一插件的调用会 round-robin 到多个子进程。外层
`cc_tool_executor_pool_t` 仍会按 `plugin.<id>` 或 `tool.<name>` lane 控制并发和
timeout，因此 app 的 worker 数和 SDK 的 lane 上限应一起配置。

工具调用的取消是协作式的。`cc_tool_context_t.cancel_token` 会传到 tool adapter；
进程型 plugin 由 POSIX/Windows app 在 pipe 等待点检查 token。若请求已经写入
子进程但 run 被取消，worker 会被复位；默认 `restartOnCrash=true` 时立即启动
干净 worker，避免下一次调用读到上一次请求的迟到响应。`ctx.timeout_ms` 同样会
映射到 pipe read timeout。

## Reload 行为

`/reload` 和 watcher 都调用 `cc_runtime_builder_reload()`：

- 新 config 先完成解析和语义校验。
- builder 构建新的 registry、tool pool、skill catalog 和 MCP/plugin 状态。
- registry 构建成功后才交换 generation；失败的外部插件工具不会进入新 generation。
- swap 后旧 generation 会被 builder 暂存到销毁阶段，避免正在运行的 run 读到已释放工具。

如果 LLM 后续仍然调用未注册的插件工具，`cc_tool_executor_execute()` 会返回
标准工具结果 `ok=0`，错误文本为 `Tool not found: <name>`。这不是进程级错误，
Agent 主循环会把该 tool result 写回上下文，让模型可以换工具或向用户解释。
