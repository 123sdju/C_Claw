# Concurrency Model

POSIX 和 Windows 共享同一套 SDK 并发语义。平台层只提供 `cc_thread`、
`cc_mutex`、`cc_cond`、process 和 HTTP transport；queue、cancel、tool pool、
snapshot 和 MCP cache 不在两个 app 里重复实现。

## Run Queue

`cc_run_queue_t` 位于 core SDK：

- 同一个 `session_key` 默认串行，避免两次 turn 同时写同一段 session history。
- 不同 lane 可以并发：`main`、`subagent`、`plugin`、`mcp`。
- `steer`、`followup`、`collect`、`interrupt` 在 SDK 层定义，CLI 只调用 manager。

`session_key` 由 agent manager 组合为 `agent_id + session_id`。这样同一 session 在
不同 agent 下不会互相阻塞。

当前 `cc_agent_manager_submit()` 面向交互 turn，提交时使用 `STEER`：同 session 的
pending run 会被替换，running run 会收到 cancel token。需要 `FOLLOWUP` 或 `COLLECT`
语义的调用方可以直接使用 `cc_run_queue_submit_with_token()` 并传入对应 action。

## Cancel Token

`cc_cancel_token_t` 是协作式取消：

- 用户 `/interrupt`
- queue steer 替换 pending/running 输入
- tool timeout
- runtime shutdown
- reload dispose

这些场景都通过同一 token 语义表达。SDK 不强杀线程，也不直接关闭子进程；shell、
plugin worker、MCP transport 和 HTTP adapter 在自己的等待点检查 token 并释放平台资源。
tool pool 在等待 lane 空位时使用短超时条件变量等待，因此 pending tool call 也能
在 interrupt 后退出等待，而不是一直卡到其它工具释放 lane。

## Tool Pool

`cc_tool_executor_pool_t` 位于 core SDK，负责：

- lane 并发上限
- timeout 策略
- acquire/release
- in-flight 计数

lane 名称规则：

- `tool.<name>`：普通内置工具。
- `plugin.<id>`：插件工具。
- `mcp.<server>`：MCP server 工具。

executor acquire 成功后，会把 `timeout_ms`、`cancel_token`、`lane_name` 和
`generation` 写入 `cc_tool_context_t`。具体工具如何把 timeout 映射到 pipe、HTTP、
文件 I/O 或进程等待，由工具实现决定。

当前实现里 HTTP、MCP transport 和 plugin pipe 都会读取 `ctx.cancel_token`；
进程型 plugin 如果已经把请求写入子进程，取消读取时会复位该 worker，避免下一次
调用读到迟到的旧 JSON-RPC 响应。

## Snapshot 与 Reload

tool registry snapshot 是 generation + refcount 模型：

- run 开始时 acquire 当前 generation。
- reload 成功后，builder 发布新 generation。
- 已经开始的 run 持有旧 snapshot，直到 run 结束 release。
- reload 失败时不 swap，当前 generation 继续服务。

POSIX/Windows watcher 只负责发现文件变化和触发 reload；安全发布和 rollback 规则属于 SDK。

## 平台端口

POSIX 使用 pthread/cond 和 POSIX process/pipe，Windows 使用对应 Win32 thread/process
实现。两边必须保持同一语义：

- mutex/cond 支持多个 worker 等待与唤醒。
- process pipe transport 能把 timeout/cancel 映射为平台等待退出。
- HTTP client 支持 body callback 取消，SSE/streamable HTTP 不需要等完整 body 入内存。

ESP profile 默认关闭 plugin、stdio MCP、watcher、shell、SQLite 和 active memory，
并可关闭 tool pool，以减少线程、pipe 和存储依赖。
