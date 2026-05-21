# POSIX CLI 配置

POSIX CLI 使用统一的 `config.json` 主配置模型。字段说明见
`docs/configuration.md`；这里仅记录桌面 app 和核心 SDK 的边界。

## 归属边界

- 核心 SDK：`agents`、`queue`、tool registry snapshot、tool executor pool、
  skill catalog、MCP protocol/runtime manager/SSE parser、plugin JSON-RPC envelope。
- POSIX CLI app：外部进程 plugin、热重载 watcher、MCP stdio/HTTP/SSE/streamable
  HTTP transport、shell/local sandbox、CLI 交互命令。
- ESP profile：默认关闭 plugin、hot reload、stdio MCP、watcher、shell、SQLite，
  避免把桌面 app 能力编进设备包。

## 插件配置

插件主配置入口是 `config.json.plugins.entries`。POSIX app 会直接按这些条目启动
plugin worker，并在 `/reload` 时用 registry generation 原子替换工具快照；如果新
配置加载失败，当前 generation 会继续服务正在运行和后续输入。

单个 plugin worker 或 MCP server 启动失败不会让 CLI 退出。失败条目会作为
runtime diagnostics 打印，相关工具不进入 registry；其它工具继续可用。后续模型
如果调用缺失工具，会得到 `Tool not found: <name>` 的普通工具错误。

```json
{
  "plugins": {
    "hotReload": true,
    "reloadDebounceMs": 300,
    "entries": {
      "echo": {
        "enabled": true,
        "command": "python3",
        "args": ["./plugins/echo.py"],
        "workers": 1,
        "timeoutMs": 30000,
        "maxInFlight": 1,
        "restartOnCrash": true,
        "skills": ["./plugins/echo/skills"],
        "tools": [
          {
            "name": "plugin.echo",
            "description": "Echo input",
            "parameters": { "type": "object" }
          }
        ]
      }
    }
  }
}
```

## 常用命令

```bash
cmake --preset posix-cli
cmake --build --preset posix-cli
ctest --preset posix-cli
```

该 preset 的 build 根是 `build/app/posix/cli`，可执行文件输出到
`build/app/posix/cli/bin/c-claw`，运行期默认数据在
`build/app/posix/cli/runtime`。这样 app 产物、SDK 最小构建和 ESP32 QEMU
产物不会混在同一个平铺目录中。
