# STM32MP135 Board App 配置

STM32MP135 board app 使用统一的 `config.json` 主配置模型。字段说明见
`docs/configuration.md`；这里记录 board app、桌面兼容能力和核心 SDK 的边界。

## 归属边界

- 核心 SDK：`agents`、`queue`、tool registry snapshot、tool executor pool、
  skill catalog、MCP protocol/runtime manager/SSE parser、plugin JSON-RPC envelope。
- Board app：外部进程 plugin、热重载 watcher、MCP stdio/HTTP/SSE/streamable
  HTTP transport、shell/local sandbox、CLI 交互命令，以及摄像头、音频、CAN、ADC
  等板级工具。
- 多模态边界：SDK 保存 `artifacts_json` / `content_parts_json` 并按 provider 输出；
  图片、音频文件的读取和 base64 编码由 board app 的 media 模块负责。

## 板级工具

`board.camera` 支持四种 `operation`：

- `capture`：采集一帧 RGB565，保存为 BMP，默认同步显示到 `/dev/fb0`，并返回 image artifact。
- `preview_start`：启动后台预览线程，持续采集 `/dev/video0` 并刷新 `/dev/fb0`。
- `preview_status`：查询预览线程状态、已显示帧数和错误次数。
- `preview_stop`：停止后台预览线程。

示例：

```json
{"operation":"preview_start","device":"/dev/video0","fb":"/dev/fb0","width":640,"height":480,"preview_interval_ms":100}
```

`board.audio` 录制 WAV 并返回 audio artifact；`board.can` 和 `board.adc` 返回普通 JSON，
不产生媒体 artifact。

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
cmake --preset stm32mp135-board
cmake --build --preset stm32mp135-board
ctest --preset stm32mp135-board
```

该 preset 的 build 根是 `build/app/posix/stm32mp135_board`，可执行文件输出到
`build/app/posix/stm32mp135_board/bin/c-claw-board`，运行期默认数据在
`build/app/posix/stm32mp135_board/runtime`。这样 app 产物、SDK 最小构建和板级应用
产物不会混在同一个平铺目录中。
