# c-claw

纯 C 语言 AI Agent 运行时。项目采用 Ports & Adapters 架构，用
`struct + vtable + void *self` 在 C 中实现接口、多态和依赖注入。

当前代码重点支持：

- ReAct 工具调用循环：用户消息 -> LLM -> 工具 -> 工具结果 -> LLM
- LLM 后端：OpenAI 兼容 API、Ollama、Anthropic，底层通过统一 HTTP LLM provider + 协议策略接入
- 工具系统：文件读写、Shell、HTTP 请求、长期记忆、外部插件工具
- 存储后端：SQLite、JSON 文件、内存
- 插件系统：JSON-RPC 2.0 over stdin/stdout，插件可用任意语言实现
- 平台抽象：POSIX、Windows、ESP32 只提供端口适配
- 应用/设备 profile：通过 CMake 选择 app/board、平台和功能，不再把桌面能力硬编码进核心
- 上下文窗口管理：历史消息截断和可选 LLM 摘要压缩
- 流式输出：CLI 支持 `/stream on|off`

## 快速开始

### 依赖

桌面默认构建需要：

- CMake >= 3.16
- C99 编译器，例如 GCC 或 Clang
- Threads
- libcurl，用于 HTTP-backed LLM 适配器和 HTTP 工具
- Python 3，仅用于示例插件测试

SQLite 使用源码集成，默认 POSIX 构建会编入；也可以通过 CMake 关闭。

### 构建

```bash
cmake --preset posix-cli
cmake --build --preset posix-cli
ctest --test-dir build/app/posix/cli --output-on-failure
```

### 运行

```bash
./build/app/posix/cli/bin/c-claw --help
./build/app/posix/cli/bin/c-claw
./build/app/posix/cli/bin/c-claw ask "列出当前工作区文件"
```

## 构建 Profile

应用、设备、平台和功能由 CMake profile 决定。默认 `CC_PROFILE=posix-cli`
构建 POSIX CLI 应用；Windows CLI 和 ESP32-S3 QEMU 使用各自 profile。

```bash
# 默认桌面构建
cmake --preset posix-cli

# 最小核心构建
cmake --preset core-minimal

# Windows CLI：仅用于 Windows 主机或已配置 Windows 交叉工具链的环境
cmake --preset windows-cli

# ESP32-S3 QEMU board
. "$IDF_PATH/export.sh"
./scripts/esp32_s3_qemu.sh doctor
./scripts/esp32_s3_qemu.sh build
./scripts/esp32_s3_qemu.sh qemu
```

日常 Linux 验证以 `posix-cli` 和 `core-minimal` 为准；`windows-cli` 需要 Windows
主机或带有 Windows 目标 libcurl 的交叉编译环境，不作为默认 Linux 必过项。

构建输出按层放在 `build/` 下，避免 SDK 测试、桌面应用和板级固件混在同一层：

| 入口 | 构建目录 | 主要产物 |
|------|----------|----------|
| POSIX CLI | `build/app/posix/cli` | `bin/c-claw`、POSIX tests |
| Windows CLI | `build/app/windows/cli` | `bin/c-claw.exe` 或 `bin/c-claw` |
| core-minimal | `build/sdk/core-minimal` | SDK 最小裁剪 tests |
| ESP32-S3 QEMU | `build/app/esp32/esp32_s3_qemu` | `.bin`、`.elf`、`qemu_flash.bin` |

ESP-IDF 生成的 `sdkconfig` 也放在 `build/app/esp32/esp32_s3_qemu/sdkconfig`；
源码目录只保留 `apps/esp32/esp32_s3_qemu/sdkconfig.defaults`。

常用开关：

| CMake 选项 | 说明 |
|------------|------|
| `CC_PROFILE` | `posix-cli`、`windows-cli`、`core-minimal`、`esp32-s3-qemu` |
| `CC_TARGET_PLATFORM` | `auto`、`posix`、`windows`、`esp32` |
| `CC_ENABLE_CLI` | 构建 CLI gateway 和 `c-claw` 可执行文件 |
| `CC_ENABLE_SHELL` | 编入 `shell_run` 工具和 local sandbox |
| `CC_ENABLE_PLUGIN` | 编入外部进程插件系统 |
| `CC_ENABLE_DOCKER_SANDBOX` | 编入 Docker sandbox 适配器 |
| `CC_ENABLE_SQLITE` | 编入 SQLite 会话存储和 SQLite 记忆后端 |
| `CC_ENABLE_OPENAI` | 编入 OpenAI 兼容 LLM 适配器 |
| `CC_ENABLE_OLLAMA` | 编入 Ollama LLM 适配器 |
| `CC_ENABLE_ANTHROPIC` | 编入 Anthropic LLM 适配器 |
| `CC_ENABLE_FILE_TOOLS` | 编入文件读写工具 |
| `CC_ENABLE_HTTP_TOOL` | 编入 `http.request` 工具 |
| `CC_ENABLE_MEMORY` | 编入长期记忆工具和记忆存储 |

注意：`esp32` profile 必须在 ESP-IDF 环境中配置，避免主机 POSIX 源文件伪装成
设备实现。当前已提供 ESP-IDF filesystem/path/thread/http client 适配，QEMU board
可通过 `chat-real` 编入 OpenAI-compatible provider。

## 配置

运行时配置文件由编译 profile 指定，不再默认读取仓库根目录 `config.json`。
POSIX CLI 默认路径是 `apps/posix/cli/config/config.json`；Windows CLI 默认路径是
`apps/windows/cli/config/config.json`；ESP32 QEMU 默认路径是 `/sdcard/cclaw/config.json`。
缺失时使用编译 profile 对应的默认值：
默认 provider 由编译 profile 选择：桌面默认优先 `openai`，也可以在配置中切换
为 `ollama` 或 `anthropic`。禁用 SQLite 时默认走 JSON；禁用长期记忆时默认使用
`noop`。

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
      "systemPromptFile": "apps/posix/cli/config/soul.md",
      "skills": ["core"]
    },
    "list": []
  },
  "queue": {
    "lanes": {
      "main": 4,
      "subagent": 8,
      "plugin": 4,
      "mcp": 4
    },
    "perSessionConcurrency": 1,
    "mode": "steer",
    "debounceMs": 500,
    "maxPendingPerSession": 20
  },
  "tools": {
    "enabled": ["read", "write", "http", "shell", "memory"],
    "defaultTimeoutMs": 30000,
    "perTool": {
      "shell_run": {
        "concurrency": 1,
        "timeoutMs": 30000
      }
    }
  },
  "plugins": {
    "hotReload": true,
    "reloadDebounceMs": 300,
    "entries": {}
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
  "sandbox": {
    "type": "local",
    "timeout_ms": 30000,
    "shell_requires_approval": true
  },
  "memory": {
    "backend": "json_file",
    "active": {
      "enabled": true,
      "writeSummary": true,
      "maxValueChars": 1600,
      "category": "active_summary"
    }
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

`model.stream_mode` 表示默认使用 LLM 流式请求；`model.thinking_mode=1` 时会强制开启 stream。

详见 [apps/posix/cli/docs/config.md](apps/posix/cli/docs/config.md)。

## 插件系统

插件以独立子进程运行，通过 JSON-RPC 2.0 over stdin/stdout 和主进程通信。
插件依赖 `CC_ENABLE_PLUGIN=ON`，在无外部进程能力的设备 profile 中会被裁剪。

插件配置归属具体应用，主入口统一在 `config.json.plugins.entries`；ESP32-S3 QEMU
profile 不编入外部进程插件。

`config.json` 插件段示例：

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
        "tools": [
          {
            "name": "weather_query",
            "description": "查询指定城市的天气信息",
            "parameters": {
              "type": "object",
              "properties": {
                "city": { "type": "string", "description": "城市名称" }
              },
              "required": ["city"]
            }
          }
        ]
      }
    }
  }
}
```

协议约定：

- stdin 每行一个 JSON-RPC 请求
- stdout 每行一个 JSON-RPC 响应
- 每次响应后 flush

```json
{"jsonrpc":"2.0","id":"1","method":"weather_query","params":{"city":"Beijing"}}
```

```json
{"jsonrpc":"2.0","id":"1","result":{"city":"Beijing","temperature":22}}
```

## 项目结构

```text
cclaw/         可移植基础 SDK：core、ports、adapters、platforms、profiles
apps/          产品工程：posix/cli、windows/cli、esp32/esp32_s3_qemu 等
build/         统一构建输出：posix-cli、core-minimal、esp32-s3-qemu 等
               app 构建放在 build/app/<platform>/<app>，
               SDK 裁剪构建放在 build/sdk/<profile>
```

## 内置工具

| 工具名 | 编译开关 | 功能 |
|--------|----------|------|
| `file_read` | `CC_ENABLE_FILE_TOOLS` | 读取 workspace 内文件 |
| `file_write` | `CC_ENABLE_FILE_TOOLS` | 写入 workspace 内文件 |
| `shell_run` | `CC_ENABLE_SHELL` | 通过 sandbox 执行 shell 命令 |
| `memory` | `CC_ENABLE_MEMORY` | 长期记忆 CRUD |
| `http.request` | `CC_ENABLE_HTTP_TOOL` | 执行 GET/POST/自定义 HTTP 请求 |
| 插件工具 | `CC_ENABLE_PLUGIN` | 由 `config.json.plugins.entries` 注册 |

`tools.enabled` 可以在运行时进一步裁剪已编入的工具。支持的常用别名：
`read` -> `file_read`，`write` -> `file_write`，`shell` -> `shell_run`，
`http` -> `http.request`。

工具按依赖分三层放置：通用 file/http/memory 工具在 `cclaw/adapters/src/tools/common`；
桌面 shell/plugin 工具在对应 `apps/<platform>/cli/src/tools`；
GPIO/I2C/SPI/ADC/PWM 等硬件工具在对应 `apps/<mcu>/<board>/main/tools`。
内置工具由应用/板级 `cc_runtime_feature_set_t` descriptor 表统一声明和注册。
新增内置能力时应添加对应层级的工厂和 app/board descriptor，而不是在平台端口层手工接线。
`apps/posix/cli/src/main.c` 只负责加载配置、创建 runtime builder、启动当前 CLI gateway。

## 文档

- [cclaw/docs/architecture.md](cclaw/docs/architecture.md)：SDK 架构、平台 profile、扩展点
- [apps/posix/cli/docs/config.md](apps/posix/cli/docs/config.md)：POSIX CLI 配置和构建选项
- [apps/esp32/esp32_s3_qemu/README.md](apps/esp32/esp32_s3_qemu/README.md)：ESP32-S3 QEMU app 说明
- [pure_c_claw_architecture_design.md](pure_c_claw_architecture_design.md)：纯 C 架构设计备忘

## License

Apache-2.0
