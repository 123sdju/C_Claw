# 配置项说明

配置文件路径：`config.json`。所有字段都是可选的，缺失字段会使用默认值。
默认值会受编译 profile 影响，例如 `CC_ENABLE_SQLITE=OFF` 时默认存储类型是
`json`，而不是 `sqlite`。

## 构建配置

运行时能力首先由 CMake 决定。未编入的能力无法通过 `config.json` 开启。

| CMake 选项 | 默认值 | 说明 |
|------------|--------|------|
| `CC_TARGET_PLATFORM` | `auto` | 目标平台：`auto`、`posix`、`windows`、`esp32` |
| `CC_ENABLE_CLI` | 桌面开，ESP32 关 | 是否构建 CLI gateway 和 `c-claw` 可执行文件 |
| `CC_ENABLE_SHELL` | 桌面开，ESP32 关 | 是否编入 `shell_run` 和 local sandbox |
| `CC_ENABLE_PLUGIN` | 桌面开，ESP32 关 | 是否编入外部进程插件系统 |
| `CC_ENABLE_DOCKER_SANDBOX` | POSIX 开，其他关 | 是否编入 Docker sandbox 适配器 |
| `CC_ENABLE_SQLITE` | POSIX 开，MSVC/ESP32 关 | 是否编入 SQLite 会话存储和记忆存储 |
| `CC_ENABLE_OPENAI` | 桌面开，ESP32 关 | 是否编入 OpenAI 兼容 LLM 适配器 |
| `CC_ENABLE_OLLAMA` | 桌面开，ESP32 关 | 是否编入 Ollama LLM 适配器 |
| `CC_ENABLE_ANTHROPIC` | 桌面开，ESP32 关 | 是否编入 Anthropic LLM 适配器 |
| `CC_ENABLE_FILE_TOOLS` | 开 | 是否编入文件读写工具 |
| `CC_ENABLE_HTTP_TOOL` | 开 | 是否编入 `http.request` 工具 |
| `CC_ENABLE_MEMORY` | 开 | 是否编入长期记忆工具和记忆存储 |

示例：

```bash
cmake -S . -B build-min \
  -DCC_ENABLE_SHELL=OFF \
  -DCC_ENABLE_PLUGIN=OFF \
  -DCC_ENABLE_SQLITE=OFF \
  -DCC_ENABLE_DOCKER_SANDBOX=OFF
```

`CC_TARGET_PLATFORM=esp32` 需要从 ESP-IDF 环境配置；普通主机 CMake 会直接报错，
避免误把 POSIX 实现当作设备端实现。

## config.json 示例

```json
{
  "model": {
    "provider": "openai",
    "model": "gpt-4o-mini",
    "base_url": "https://api.openai.com",
    "api_key": "sk-...",
    "max_tokens": 4096,
    "temperature": 0.7,
    "thinking_mode": 0
  },
  "storage": {
    "type": "sqlite",
    "path": "./data/c-claw.db"
  },
  "workspace": {
    "path": "./workspace"
  },
  "sandbox": {
    "type": "local",
    "timeout_ms": 30000,
    "shell_requires_approval": true
  },
  "memory": {
    "backend": "json_file",
    "path": "./data/memory.json"
  },
  "tools": {
    "enabled": ["read", "write", "shell", "memory"]
  },
  "system": {
    "max_steps": 10,
    "soul_file": "soul.md",
    "user_file": "user.md",
    "system_prompt": null,
    "context_window_tokens": 8192,
    "context_compress_threshold": 80,
    "context_keep_recent": 20,
    "summary_max_tokens": 1024,
    "summary_temperature": 0.3
  }
}
```

## model 段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `provider` | string | `openai`、`anthropic`、`ollama` 或 `none` | 由编译开关决定，桌面默认优先 `openai` |
| `model` | string | 随 provider 变化 | 默认模型名随 provider 变化 |
| `base_url` | string | 随 provider 变化 | API 基础地址，不包含 provider 自己追加的 endpoint path |
| `api_key` | string/null | `null` | 远程 provider 通常需要，Ollama 可为空 |
| `max_tokens` | int | `4096` | 主对话单次回复最大 token 数 |
| `temperature` | number | `0.7` | 主对话生成温度；`0.0` 更确定，较高值更随机 |
| `thinking_mode` | int | `0` | 是否启用思考模式，`0` 关闭，`1` 开启 |

OpenAI 兼容配置示例：

```json
{
  "model": {
    "provider": "openai",
    "model": "gpt-4o-mini",
    "base_url": "https://api.openai.com",
    "api_key": "sk-..."
  }
}
```

Anthropic 配置示例：

```json
{
  "model": {
    "provider": "anthropic",
    "model": "claude-3-5-haiku-latest",
    "base_url": "https://api.anthropic.com",
    "api_key": "sk-ant-..."
  }
}
```

## storage 段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `type` | string | `sqlite`、`json` 或 `memory` | `sqlite` 仅在 `CC_ENABLE_SQLITE=ON` 时可用 |
| `path` | string | `./data/c-claw.db` 或 `./data/sessions.json` | 存储文件路径 |

可选值：

- `sqlite`：SQLite 会话存储。若编译时禁用或初始化失败，会降级到 JSON 文件存储。
- `json` / `local_file`：JSON 文件会话存储。
- `memory`：纯内存会话存储，进程退出后丢失。

## workspace 段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `path` | string | `./workspace` | 文件工具限定访问的工作区路径 |

## sandbox 段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `type` | string | `local` | `local`、`docker` 或 `none`；所选 sandbox 必须已编入 |
| `timeout_ms` | int | `30000` | 命令默认超时时间 |
| `shell_requires_approval` | bool | `true` | Shell 命令是否需要策略审批 |

注意：sandbox 只有在 `CC_ENABLE_SHELL=ON` 时才会实际创建并注册给 `shell_run`。
`docker` 需要 `CC_ENABLE_DOCKER_SANDBOX=ON`，`none` 适合无 shell 的裁剪 profile。

## memory 段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `backend` | string | `json_file` 或 `noop` | `CC_ENABLE_MEMORY=OFF` 时默认 `noop` |
| `path` | string | `./data/memory.json` | 持久化文件或数据库路径 |

可选值：

- `json_file`：JSON 文件长期记忆。
- `sqlite`：SQLite 长期记忆，仅 `CC_ENABLE_SQLITE=ON` 可用。
- `inmem`：纯内存长期记忆。
- `noop`：禁用长期记忆。当前实现会返回禁用错误，主程序会降级为不注册 memory 工具。
- `none`：`noop` 的兼容别名。

## tools 段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `enabled` | string[]/null | `null` | 为 `null` 或空时注册所有已编入工具；非空时只注册列表中的工具 |

支持的工具名和别名：

| 配置值 | 对应工具 |
|--------|----------|
| `file_read` 或 `read` | 文件读取 |
| `file_write` 或 `write` | 文件写入 |
| `shell_run` 或 `shell` | Shell 执行 |
| `memory` | 长期记忆 |
| `http.request` 或 `http` | HTTP 请求 |

插件工具由 `plugins.json` 声明；`tools.enabled` 当前只过滤主程序内置注册路径。

## system 段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `soul_file` | string | `soul.md` | Agent 人格定义文件路径 |
| `user_file` | string | `user.md` | 用户偏好文件路径 |
| `system_prompt` | string/null | `null` | 显式 system prompt。非空时优先于 `soul_file` 和 `user_file` |
| `max_steps` | int | `10` | Agent 最大推理步数 |
| `context_window_tokens` | int | `8192` | LLM 上下文 token 预算；`0` 表示不限制 |
| `context_compress_threshold` | int | `80` | 压缩触发百分比；`0` 禁用压缩 |
| `context_keep_recent` | int | `20` | 压缩时保留最近 N 条原始消息 |
| `summary_max_tokens` | int | `1024` | 上下文摘要压缩请求的最大生成 token 数 |
| `summary_temperature` | number | `0.3` | 上下文摘要压缩请求的生成温度，默认低于主对话以提高稳定性 |

## 上下文窗口管理

当历史消息较长时，`cc_context_builder` 使用两层策略：

1. Token 预算截断：超过 `context_window_tokens` 时优先丢弃旧消息。
2. LLM 摘要压缩：超过阈值时把旧消息压缩成摘要，再保留最近消息原文。

示例：

```json
{
  "system": {
    "context_window_tokens": 128000,
    "context_compress_threshold": 80,
    "context_keep_recent": 30,
    "summary_max_tokens": 1024,
    "summary_temperature": 0.3
  }
}
```

## plugins.json

插件配置独立于 `config.json`，仅在 `CC_ENABLE_PLUGIN=ON` 时加载。

```json
{
  "plugins": [
    {
      "name": "weather",
      "command": "python3",
      "args": ["./plugins/weather_tool.py"],
      "tools": [
        {
          "name": "weather_query",
          "description": "查询指定城市的天气信息",
          "parameters": {
            "type": "object",
            "properties": {
              "city": { "type": "string" }
            },
            "required": ["city"]
          }
        }
      ]
    }
  ]
}
```
