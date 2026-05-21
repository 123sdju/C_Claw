# MCP Client Runtime

C-Claw 当前实现 MCP client/tool bridge，不实现 MCP server。SDK 负责协议状态机和工具桥接，
app/platform 负责具体 transport。

## SDK 与 Transport

SDK 类型：

- `cc_mcp_runtime_manager_t`：管理 server runtime、`initialize`、`tools/list`、
  `tools/call`、TTL、session reset 和 tool 注册。
- `cc_mcp_transport_t`：transport vtable，包含 `send_json`、`reset`、`destroy`、
  `is_serial`。
- `cc_sse_parser_t`：增量 SSE parser，支持跨 chunk、heartbeat、`data:` 多行和 `[DONE]`。

app/platform 类型：

- stdio transport：启动本机 MCP server 子进程，通过 pipe 收发 JSON-RPC。
- HTTP transport：用平台 HTTP client 发送 JSON-RPC，并根据 response 类型处理 JSON、
  SSE 或 streamable HTTP。

core manager 不包含 `fork`、`CreateProcess`、curl、Win32 或 ESP-IDF HTTP 头文件。

## 配置

```json
{
  "mcp": {
    "enabled": true,
    "sessionIdleTtlMs": 600000,
    "servers": {
      "fs": {
        "transport": "stdio",
        "command": "node",
        "args": ["server.js"],
        "cwd": "./mcp",
        "connectionTimeoutMs": 30000
      },
      "remote": {
        "transport": "streamable_http",
        "url": "http://127.0.0.1:8787/mcp",
        "connectionTimeoutMs": 30000
      }
    }
  }
}
```

工具名暴露为 `mcp.<server>.<tool>`。例如 server id 为 `fs`、MCP tool 名为
`read_file` 时，LLM 看到的 C-Claw tool 名称是 `mcp.fs.read_file`。

单个 MCP server 的 transport factory、`initialize` 或 `tools/list` 失败时，
该 server 的工具不会注册，但 runtime 不会退出；错误会进入
`cc_runtime_diagnostics_t`，CLI 会在启动或 `/reload` 后展示。配置语法错误、
非法 transport 字段或基础 runtime 创建失败仍然是致命错误。

## Transport 行为

`stdio` 是串行 transport。manager 会在 send 期间持有 server mutex，避免多线程同时
写同一根 pipe。

`http` 是一次 JSON-RPC request/response。HTTP transport 可以标记为并发；manager
只保护 request id、TTL 和 initialize 状态。

`sse` 读取 `text/event-stream` 响应。transport 使用 SDK SSE parser 逐 chunk 解析，
并返回第一条 JSON-RPC id 匹配当前 request 的 response。

`streamable_http` 使用 POST JSON-RPC request，并发送
`Accept: application/json, text/event-stream`。响应是 JSON 时按普通 response 解析；
响应是 SSE 时走同一套 SSE parser。若服务端返回 `Mcp-Session-Id`，transport 保存并在
后续请求中带上；`reset` 和 dispose 会清空该 session id。

## TTL 与 Cancel

`sessionIdleTtlMs` 到期后，下一次 MCP 调用会先调用 transport `reset`，再重新
`initialize`。reload 成功发布新 generation 时，旧 MCP manager 会被 retired，builder
销毁时统一释放 transport。

工具调用时 `cc_tool_context_t.cancel_token` 会传入 `tools/call`。transport 在等待 pipe、
HTTP body 或 SSE chunk 时应检查 token；取消只是一致的协作语义，不强杀 SDK 线程。

如果 MCP server 因启动失败没有注册工具，后续模型调用对应名称会走统一的缺失工具
语义：tool result 为 `ok=0`，错误为 `Tool not found: mcp.<server>.<tool>`，
主循环继续运行。

## 裁剪

`CC_ENABLE_MCP` 控制 SDK runtime/tool bridge。`CC_ENABLE_MCP_STDIO` 和
`CC_ENABLE_MCP_HTTP` 控制桌面 app transport。ESP profile 默认关闭 MCP；如果之后要在
设备端启用 HTTP MCP，可以保留 SDK manager，只提供 ESP HTTP transport，并继续关闭 stdio。
