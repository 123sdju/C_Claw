# 配置项说明

配置文件路径：`config.json`。主配置模型把 `model`、`storage`、`agents`、
`queue`、`tools`、`plugins`、`skills`、`mcp`、`memory`、`sandbox`、`cli`
放在同一个文件里。运行时能力首先由 CMake 决定：未编译进来的能力不能靠
`config.json` 打开。

## 构建裁剪

| CMake 选项 | 桌面默认 | ESP 默认 | 说明 |
|------------|----------|----------|------|
| `CC_ENABLE_MULTI_AGENT` | 开 | 开 | 多 agent manager 核心映射 |
| `CC_ENABLE_RUN_QUEUE` | 开 | 开 | 同 session 串行、跨 lane 并发的 run queue |
| `CC_ENABLE_TOOL_POOL` | 开 | 关 | tool/plugin/MCP lane 并发池 |
| `CC_ENABLE_SKILLS` | 开 | 开 | 静态 skill catalog 能力 |
| `CC_ENABLE_SKILL_WATCHER` | 开 | 关 | 文件 watcher，属于 app 层 |
| `CC_ENABLE_PLUGIN` | 开 | 关 | 外部进程 plugin 工具 |
| `CC_ENABLE_PLUGIN_HOT_RELOAD` | 开 | 关 | plugin generation 热重载 |
| `CC_ENABLE_PLUGIN_WORKERS` | 开 | 关 | plugin worker pool |
| `CC_ENABLE_MCP` | 开 | 关 | MCP runtime/tool bridge |
| `CC_ENABLE_MCP_STDIO` | 开 | 关 | stdio MCP transport |
| `CC_ENABLE_MCP_HTTP` | 开 | 关 | HTTP/SSE/streamable HTTP transport |
| `CC_ENABLE_SUBAGENTS` | 开 | 关 | subagent service |
| `CC_ENABLE_ACTIVE_MEMORY` | 开 | 关 | active memory hook，run 后写入摘要或事实 |
| `CC_ENABLE_SHELL` | 开 | 关 | shell 工具和 local sandbox |
| `CC_ENABLE_SQLITE` | POSIX 开 | 关 | SQLite 存储 |

ESP profile 默认关闭 plugin、hot reload、stdio MCP、watcher、shell、SQLite，
只保留轻量 core runtime、JSON/in-memory 存储、可选静态 skills 和 memory。

## config.json 示例

```json
{
  "model": {
    "provider": "openai",
    "model": "gpt-4o-mini",
    "base_url": "https://api.openai.com",
    "api_key": "",
    "max_tokens": 4096,
    "temperature": 0.7,
    "thinking_mode": 0,
    "stream_mode": 0
  },
  "storage": {
    "type": "sqlite"
  },
  "agents": {
    "defaults": {
      "id": "default",
      "workspace": "./workspace",
      "agentDir": ".agents/default",
      "systemPromptFile": "soul.md",
      "skills": ["core"]
    },
    "list": []
  },
  "queue": {
    "lanes": { "main": 4, "subagent": 8, "plugin": 4, "mcp": 4 },
    "perSessionConcurrency": 1,
    "mode": "steer",
    "debounceMs": 500,
    "maxPendingPerSession": 20
  },
  "tools": {
    "enabled": ["read", "write", "shell", "memory"],
    "defaultTimeoutMs": 30000,
    "perTool": {
      "shell_run": { "concurrency": 1, "timeoutMs": 30000 }
    }
  },
  "plugins": {
    "hotReload": true,
    "reloadDebounceMs": 300,
    "entries": {
      "weather": {
        "enabled": true,
        "command": "python3",
        "args": ["./plugins/weather.py"],
        "workers": 1,
        "timeoutMs": 30000,
        "maxInFlight": 1,
        "restartOnCrash": true,
        "skills": ["./plugins/weather/skills"],
        "tools": [
          {
            "name": "weather.query",
            "description": "查询城市天气",
            "parameters": {
              "type": "object",
              "properties": { "city": { "type": "string" } },
              "required": ["city"]
            }
          }
        ]
      }
    }
  },
  "skills": {
    "load": {
      "watch": true,
      "watchDebounceMs": 250,
      "extraDirs": [".agents/skills", "~/.cclaw/skills"]
    }
  },
  "mcp": {
    "enabled": false,
    "sessionIdleTtlMs": 600000,
    "servers": {}
  },
  "memory": {
    "backend": "json_file",
    "path": "./data/memory.json",
    "active": {
      "enabled": true,
      "writeSummary": true,
      "maxValueChars": 1600,
      "category": "active_summary"
    }
  },
  "sandbox": {
    "type": "local",
    "timeout_ms": 30000,
    "shell_requires_approval": true
  },
  "system": {
    "max_steps": 10,
    "context_window_tokens": 8192,
    "context_compress_threshold": 80,
    "context_keep_recent": 20,
    "summary_max_tokens": 1024,
    "summary_temperature": 0.3
  },
  "cli": {
    "debug_mode": false
  }
}
```

## 分段说明

`model`：LLM provider、模型名、API 地址、token、temperature、thinking/stream 开关。
`thinking_mode=1` 时 loader 会强制打开 `stream_mode`，方便 CLI 展示推理增量。

`storage`：会话存储。`type` 支持 `sqlite`、`json`、`memory` 等 profile 可用后端。
`sqlite` 只在 `CC_ENABLE_SQLITE=ON` 时可用；ESP 通常用 `json` 或 `memory`。
`data_dir`、`path` 可选，缺失时使用编译 profile 的默认运行期目录。POSIX CLI
默认在 `build/app/posix/cli/runtime` 下生成数据，ESP32 QEMU 默认在 `/sdcard/cclaw`。

`agents`：多 agent 配置。当前代码字段名是 `defaults` 和 `list`：`defaults`
描述默认 agent，`list` 声明具名 agent。`skills` 是 per-agent allowlist，
skill catalog 只把允许的 skill 注入 prompt。

`queue`：run queue 配置。`perSessionConcurrency=1` 表示同 session 串行；`lanes`
控制 main/subagent/plugin/mcp 的并发上限。

`tools`：内置工具过滤和执行策略。`enabled` 为空或缺失时表示注册所有已编入工具；
`perTool` 记录并发与 timeout，供 tool executor pool 和 adapter 使用。

`plugins`：外部进程插件主配置入口。POSIX/Windows app 从 `plugins.entries` 加载。
热重载会以 registry generation 方式替换：新 run 使用新快照，旧 run 继续持有旧快照。

`skills`：AgentSkills 风格 `SKILL.md` 加载路径。桌面可以打开 watcher；ESP 默认只
使用静态目录，避免引入文件监听。

`mcp`：MCP client 配置。core SDK 负责 `initialize`、`tools/list`、
`tools/call`、session cache、TTL 和 tool bridge；POSIX/Windows app 只提供
transport factory。`servers` 支持：

- `stdio`：单管道串行 worker，适合本机 MCP server。
- `http`：普通 JSON-RPC request/response。
- `sse`：`text/event-stream` 响应，SDK SSE parser 会跨 chunk 合并 `data:`，
  并挑出匹配当前 JSON-RPC id 的 response。
- `streamable_http`：POST JSON-RPC request，`Accept: application/json, text/event-stream`；
  响应为 JSON 时按普通 JSON 解析，响应为 SSE 时按 event stream 解析。若 server
  返回 `Mcp-Session-Id`，transport 会缓存并在后续请求中带上；`sessionIdleTtlMs`
  到期或 reload dispose 会 reset 对应 session。

`memory`：长期记忆后端。桌面可用 `json_file`/`sqlite`/`inmem`；`active`
控制 run 后是否把当前输入/输出沉淀为可检索摘要。ESP 默认 `json_file` 或
`inmem`，并关闭 active memory。

`sandbox`：只影响 shell 等高风险工具。ESP profile 应使用 `none`。

`cli`：桌面 CLI 交互选项，不属于核心 SDK。

## 错误行为

`config.json` 解析后会在 core SDK 中做统一语义校验。校验失败时启动或 reload
返回 `CC_ERR_INVALID_ARGUMENT`，不会进入 app 层半加载状态。当前固定的错误包括：

- `queue.lanes` 并发数必须为正数，queue cap/debounce 必须非负。
- `plugins.entries.*` 启用时必须有 `command`，`workers`/`maxInFlight` 必须为正数。
- `mcp.servers.*.transport` 只能是 `stdio`、`http`、`sse`、`streamable_http`。
- `stdio` MCP server 必须有 `command`；HTTP/SSE/streamable HTTP server 必须有 `url`。
- timeout、TTL、watch debounce、active memory value cap 不能为负数。

外部能力启动失败与配置错误分开处理：

- JSON 语法错误、非法并发/timeout、重复 id、基础 runtime 创建失败会阻止启动或 reload。
- 单个 plugin 进程、MCP transport、MCP initialize/listTools 启动失败会记录到
  runtime diagnostics，但程序继续运行，失败 tool 不注册。
- 后续调用未注册 tool 时返回标准工具错误 `Tool not found: <name>`，Agent 主循环继续。
