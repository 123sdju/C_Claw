# 纯 C Agent Runtime 架构设计备忘

本文是 c-claw 的设计备忘，用来说明为什么当前代码采用纯 C、vtable、端口适配器、
构建 profile 和插件进程协议。更贴近当前代码的完整说明见
[cclaw/docs/architecture.md](cclaw/docs/architecture.md)。

## 1. 设计目标

核心目标：

1. 核心 runtime 使用 C99/C11 风格 C 实现，不依赖 C++。
2. 使用 `struct + vtable + void *self` 实现多态。
3. LLM、工具、存储、沙箱、平台能力都通过接口注入。
4. 桌面和设备端能力通过 CMake profile 裁剪。
5. 插件工具通过 JSON-RPC over stdio 扩展，不要求重新编译主程序。
6. 上层 app/core 不直接依赖 POSIX、Win32 或 ESP-IDF API。

非目标：

1. 当前不实现完整 GUI。
2. 当前不实现完整 MCP 协议。
3. 当前不实现动态库插件热加载。
4. 当前不提供完整 ESP32 固件工程；ESP32 需要在 ESP-IDF 环境中配置。

## 2. 当前目录形态

```text
cclaw/
  core/        核心对象、agent runtime、配置、JSON、日志、上下文构建
  ports/       llm、tool、http、store、sandbox、policy、event、logger 抽象
  adapters/    LLM、通用工具、存储、policy、SQLite/cJSON 等适配
  platforms/   POSIX / Windows / ESP32 平台实现
  profiles/    CMake profile 默认平台和功能裁剪
  tests/       core、adapter、platform 测试

apps/
  posix/cli/   POSIX CLI 入口、桌面 shell/plugin/sandbox 工具和配置
  posix/stm32mp135_board/ STM32MP135 board 入口、桌面插件能力和板级工具
```

## 3. 分层规则

```text
Gateway -> Application -> Ports -> Adapters -> Platform
```

约束：

- Core 不访问网络、数据库、系统命令。
- Application 只依赖 ports 和少量平台抽象。
- Adapter 可以依赖第三方库和平台层。
- Platform 隐藏 OS/设备 SDK 差异。
- `main.c` 是当前桌面 CLI 的组合根，不应放业务逻辑。

## 4. 纯 C 多态模式

以工具为例：

```c
typedef struct cc_tool {
    void *self;
    const cc_tool_vtable_t *vtable;
} cc_tool_t;

struct cc_tool_vtable {
    const char *(*name)(void *self);
    const char *(*description)(void *self);
    const char *(*schema_json)(void *self);
    cc_result_t (*call)(
        void *self,
        const char *args_json,
        const cc_tool_context_t *ctx,
        cc_tool_result_t *out_result
    );
    void (*destroy)(void *self);
};
```

好处：

- 上层只处理 `cc_tool_t`。
- 测试可以注入 mock。
- 新工具不需要改 runtime 主循环。
- 生命周期统一由 `destroy` 管理。

同样模式用于 LLM provider、session store、memory store、sandbox 和 policy engine。

## 5. Agent Runtime

主循环是 ReAct 风格：

```text
user input
  -> append user message
  -> build messages from store
  -> build tools schema
  -> call LLM
  -> if tool call: execute tool, store result, loop
  -> else: return text
```

重要决策：

- 每轮从 storage 重建上下文，便于崩溃恢复和多 gateway 共享。
- 工具调用和工具结果都持久化。
- 上下文窗口由 `cc_context_builder` 管理，支持截断和摘要压缩。
- 流式路径通过事件总线把 thinking/text/tool 片段发给 CLI。

## 6. 设备 Profile

适配不同设备时，不应直接改 app/core，而应调整 CMake profile 和 adapter。

当前 CMake 入口：

```bash
cmake --preset posix-cli
cmake --preset core-minimal
cmake --preset stm32mp135-board
```

关键开关：

```text
CC_ENABLE_CLI
CC_ENABLE_SHELL
CC_ENABLE_PLUGIN
CC_ENABLE_DOCKER_SANDBOX
CC_ENABLE_SQLITE
CC_ENABLE_OPENAI
CC_ENABLE_OLLAMA
CC_ENABLE_FILE_TOOLS
CC_ENABLE_HTTP_TOOL
CC_ENABLE_MEMORY
```

平台能力宏：

```text
CC_HAS_PROCESS_RUN
CC_HAS_PROCESS_PIPE
CC_HAS_FORK
CC_HAS_THREADS
CC_HAS_NETWORK
CC_HAS_REALPATH
```

设计重点：`CC_HAS_FORK` 不等于能执行进程。Windows 没有 fork，但有
`CreateProcess`，因此使用 `CC_HAS_PROCESS_RUN` / `CC_HAS_PROCESS_PIPE`
表达更高层能力。

HTTP client 是平台能力：POSIX/Windows 使用各自平台目录下的 curl adapter，
ESP32 使用 ESP-IDF HTTP client。

## 7. 存储设计

会话存储：

- SQLite：生产和桌面环境。
- JSON 文件：轻量持久化。
- Memory：测试和临时运行。

长期记忆：

- `json_file`
- `sqlite`
- `inmem`
- `noop`

SQLite 被禁用时，session store 会降级到 JSON；memory store 的 sqlite 后端会返回
明确的平台错误，主程序会选择不注册 memory 工具。

## 8. 工具设计

内置工具：

| 工具 | 说明 |
|------|------|
| `file_read` | workspace 内文件读取 |
| `file_write` | workspace 内文件写入 |
| `shell_run` | sandbox 中执行 shell 命令 |
| `memory` | 长期记忆 CRUD |
| `http.request` | 通过 HTTP client port 发起 HTTP 请求 |

工具注册有两层裁剪：

1. 编译层：CMake 是否编入对应源文件。
2. 运行层：profile 指定配置文件中的 `tools.enabled` 是否允许注册。

## 9. 插件设计

插件用于快速扩展工具能力：

```text
main process
  -> cc_plugin_manager
  -> cc_plugin_process
  -> external command
  -> JSON-RPC over stdin/stdout
```

插件优点：

- 任意语言实现。
- 崩溃不会直接破坏主进程内存。
- 不需要重新编译 c-claw。

插件限制：

- 依赖外部进程和管道。
- 不适合 ESP32 这类无进程模型设备。
- 插件不自动继承主进程 sandbox，需要用户自己管理隔离。

## 10. 新设备适配步骤

1. 在 `cclaw/platforms/<device>` 添加平台实现：filesystem、path、thread，必要时 process/http client。
2. 为该平台添加独立 `CMakeLists.txt`，封装源码、SDK 依赖和编译定义。
3. 在 `cclaw/CMakeLists.txt` 中加入 `CC_TARGET_PLATFORM` 映射，并新增 `cclaw/profiles/<device>.cmake`。
4. 关闭设备不支持的功能，例如 plugin、shell、SQLite。
5. 在 `apps/<mcu>/<board>` 添加设备 gateway，例如 UART 或 BLE。
6. 使用目标 SDK toolchain 验证，避免宿主 POSIX 实现伪装成设备实现。

## 11. 当前待改进点

- 将更多嵌入式 app profile 固化到 `cclaw/profiles/` 和 `apps/<mcu>/<board>`。
- 为 UART gateway 建立正式 port。
- 为 ESP32 补原生 HTTP client 适配器和完整上板工程骨架。
