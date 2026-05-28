# 嵌入式 Linux 面试学习文档

这篇文档用于把 C-Claw 讲成一个嵌入式 Linux 软件开发项目。重点不是“我写了一个聊天应用”，
而是“我用 C 做了一个可移植 Agent Runtime SDK，能在 Linux 上通过端口层适配线程、文件、
网络、进程和存储能力”。

## 项目一句话

C-Claw 是一个纯 C SDK，核心层不绑定 UI、Web、CLI 或业务 gateway。嵌入式 Linux 应用可以把它
作为库链接进自己的进程，由应用负责按键、屏幕、语音、网络配置和业务工具。

面试表达：

> 这个项目按嵌入式 SDK 的方式设计：core 只写平台无关逻辑，ports 定义系统能力接口，
> platforms/posix 把接口落到 pthread、文件系统、curl 和进程 API。这样同一套 runtime
> 可以在 Linux、RTOS 或 MCU profile 下按能力裁剪。

## Linux 侧分层

```text
应用进程
  负责 UI、设备输入、配置、API key、业务工具。

cclaw/core
  Agent 主循环、消息模型、工具调度、事件、资源限制、上下文构造。

cclaw/ports
  抽象 Linux 能力：线程、文件系统、路径、HTTP、进程、存储。

cclaw/platforms/posix
  pthread、mutex、condition variable、POSIX 文件、realpath、libcurl、fork/exec。

cclaw/adapters
  JSON/SQLite store、HTTP LLM provider、内置文件/HTTP/memory 工具。
```

面试要点：这是典型的“分层 + 端口适配”结构，Linux 相关代码只出现在 platform/adapter 层，
core 不直接 `#include <pthread.h>` 或 `#include <curl/curl.h>`。

## 线程与并发

项目里的并发能力主要体现在：

- `cc_thread.h` 抽象线程、互斥锁、条件变量。
- `cc_event_bus` 支持同步和异步模式。
- `cc_run_queue` 支持 worker 队列和 per-session 串行。
- `cc_tool_executor_pool` 用 lane 做工具并发隔离。
- 多 session 场景下，消息 ID 和 event bus handler 状态需要线程安全。

面试回答模板：

> 我没有在 core 里直接写 pthread，而是定义了 thread port。POSIX 平台用 pthread 实现，
> FreeRTOS 平台可以映射到 task、mutex、semaphore。这样 core 只依赖 `cc_mutex_t` 这类抽象，
> 并发策略可以复用，平台同步原语可以替换。

常见追问：

- 为什么 event bus 要有 async 模式？
  - 避免日志、调试 UI 或指标上报阻塞 agent 主循环。
- 为什么 per-session 要串行？
  - 同一个会话历史有顺序语义，两个 run 同时写入会造成上下文错乱。
- 为什么工具池按 lane 限流？
  - shell、MCP、plugin、HTTP 这类能力风险和耗时不同，需要隔离并发预算。

## 文件系统与路径安全

嵌入式 Linux 里文件能力经常涉及权限边界。C-Claw 的策略是：

- core 通过 `cc_filesystem_t` 和 `cc_path` port 访问文件。
- 文件工具必须检查 workspace 边界。
- 写不存在文件时，也要先 canonical parent dir。
- 支持 symlink 的平台要解析 symlink，不能只做字符串前缀判断。
- `../`、workspace prefix 绕过、symlink 指向外部目录都应拒绝。

面试表达：

> 文件工具不是直接拼字符串打开路径，而是先经过路径归一化和 workspace 边界检查。
> 这在嵌入式 Linux 上很重要，因为设备上可能有配置、证书和日志目录，工具不能越权读写。

可以展开的技术点：

- `realpath` 只能解析已存在路径，所以写新文件要 canonical parent。
- prefix 判断必须带路径分隔符语义，`/tmp/ws2` 不能被当成 `/tmp/ws` 内部。
- symlink 是路径安全里的高风险点，必须让 platform path 层处理。

## HTTP、网络和 redirect

Linux profile 可以启用 HTTP client。C-Claw 的约束是：

- `http.request` 默认 deny。
- allowlist 支持 host、host:port、`*.domain`、scheme://host、scheme://host:port。
- userinfo URL、格式非法、scheme/port 不匹配都拒绝。
- localhost、loopback、private IPv4、link-local 默认拒绝，除非显式 allow。
- 如果 HTTP client 自动跟随 redirect，最终 URL 也必须重新校验。

面试表达：

> 我把网络访问做成 policy，而不是让工具随便请求 URL。这样 SDK 能用于设备场景，
> 避免 LLM 工具访问内网地址、localhost 管理接口或 metadata service。

## 进程和 shell

嵌入式 Linux 上 shell 能力很强，也很危险。C-Claw 的处理方式：

- shell/process port 只在 profile 显式启用时编译。
- 默认策略把 shell 视为高风险工具。
- 高风险工具需要 approval handler；没有 handler 时 deny。
- tool result 有最大字节限制，避免命令输出撑爆内存。
- cancel token 要传播到长时间运行的工具。

面试表达：

> 我没有把 shell 当成普通工具，而是做了 policy + approval + sandbox + resource limit。
> 设备侧 shell 一旦暴露给模型，必须默认拒绝，只有明确审批后才能执行。

## CMake 与 SDK 发布

Linux SDK 常见要求是“能被下游工程稳定集成”。C-Claw 目前覆盖：

- profile preset：`core-minimal`、`desktop-agent`、`multimodal-full`、`mcu-text`、`mcu-mm-lite`。
- `CClaw::runtime` CMake export target。
- `find_package(CClaw CONFIG REQUIRED)`。
- `pkg-config` 文件。
- static/shared build。
- consumer install CTest。

面试表达：

> 我不只写了源码，还补了安装导出和 consumer 测试。这样能证明 SDK 安装后被外部 CMake
> 工程正确 include 和 link，这比只在源码树里能编译更接近发布质量。

## 可以写进简历的项目描述

> 基于 C99 设计并实现跨平台 Agent Runtime SDK，采用 ports/adapters/platforms 分层，
> 在嵌入式 Linux profile 下适配 POSIX thread、filesystem、path、curl HTTP 和 process
> 能力；实现工具安全策略、workspace 路径校验、network allowlist、资源限制、结构化错误
> 和统一 observability schema；提供 CMake install/export、pkg-config 和 consumer 构建测试。

## 面试常见追问

### 为什么不用 C++？

回答思路：

> 嵌入式项目里 C ABI 更稳定，和 RTOS、BSP、驱动、厂商 SDK 集成成本低。项目用
> `struct + vtable + void *self` 实现面向对象风格，既能保留 C 的可移植性，也能支持
> provider、store、tool、platform 的多态替换。

### 怎么保证 core 不依赖 Linux？

回答思路：

> core 只依赖 ports 里的抽象类型；Linux 相关系统调用在 platforms/posix。编译 profile
> 决定哪些能力进入最终 SDK，所以 MCU profile 可以不编译 HTTP、shell、SQLite 等重能力模块。

### 怎么定位线上问题？

回答思路：

> 通过统一 observability schema，把 run、LLM、tool、approval、stream、error 都发布为
> 稳定 JSON。下游可以接日志、指标或调试 UI；实时输出仍走 stream callback，观测和业务输出
> 分离。
