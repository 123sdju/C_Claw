# MCU/RTOS 面试学习文档

这篇文档用于把 C-Claw 讲成 MCU/RTOS 方向的项目。重点是资源裁剪、端口抽象、任务同步、
内存风险控制和“哪些能力不应该放进 SDK core”。

## 项目在 MCU/RTOS 上的定位

C-Claw 不是完整产品固件，而是可以被固件集成的 C Runtime SDK。MCU/RTOS 应用负责：

- 板级初始化、BSP、驱动。
- 摄像头、麦克风、屏幕、按键、网络模组。
- API key 和配置分发。
- 业务 gateway 和 UI。
- 设备私有工具。

SDK core 负责：

- 消息模型。
- agent run 主循环。
- provider/tool/store 端口契约。
- 工具安全策略。
- stream callback。
- memory query port。
- 资源限制和取消传播。

面试表达：

> 我把 SDK core 和设备业务分开。core 不碰 UART、屏幕、摄像头等硬件细节，只提供 C 接口和
> port 抽象。这样同一套 runtime 可以在 Linux 上调试，也可以裁剪后接入 FreeRTOS/ESP32。

## Profile 裁剪思路

MCU/RTOS 方向最重要的是“能裁剪”。

项目里可讲的 profile：

- `core-minimal`：最小核心能力，适合验证 API 和构建。
- `mcu-text`：面向 MCU 文本场景，关闭重型多模态、shell、SQLite 等能力。
- `mcu-mm-lite`：轻量多模态 profile，优先传 path/uri/id 引用，不鼓励 inline base64。
- `desktop-agent`：Linux 桌面开发和调试 profile，能力更全。
- `multimodal-full`：完整多模态 profile，用于高资源平台。

面试表达：

> 我通过 CMake profile 做编译期能力裁剪，而不是运行时判断一堆 feature flag。
> MCU profile 不编译不需要的源文件，可以减小镜像体积、降低 RAM 占用，也减少攻击面。

## FreeRTOS/ESP32 port 应该怎么讲

端口层的核心思想：

```text
cc_thread.h      -> task/mutex/semaphore 或 pthread
cc_filesystem.h  -> littlefs/fatfs/spiffs 或 POSIX FS
cc_path.h        -> 平台路径规范化
cc_http_client.h -> lwIP/mbedTLS/ESP HTTP client
cc_env.h         -> 固件配置来源
```

面试表达：

> FreeRTOS 里没有 POSIX pthread，我通过 port 把 task、mutex、condition 或 semaphore
> 包装成统一接口。core 不知道底层是 pthread 还是 FreeRTOS task，这就是可移植 SDK 的关键。

常见追问：

- condition variable 在 RTOS 上怎么做？
  - 可用 semaphore/event group 模拟，或限制 async event bus worker 数量。
- 没有文件系统怎么办？
  - session store 和 memory store 可以换成 RAM、flash KV、外部 SPI flash 或禁用持久化。
- 没有网络怎么办？
  - provider 可以由应用 gateway 代理，SDK 只保留协议和 runtime，不强制内置 HTTP。

## 内存管理风险

MCU/RTOS 面试里，动态内存一定会被追问。C-Claw 当前 C SDK 设计需要这样解释：

- core 里有 `malloc/free`，适合 Linux 和较高资源 MCU。
- MCU profile 应限制最大输入、最大输出、最大 tool result、最大 stream bytes。
- 长字符串、JSON、tools schema 都要有上限。
- 多模态不要 inline base64，优先引用外部存储中的资源。
- 对长期运行固件，后续可引入 arena、对象池或应用注入 allocator。

面试表达：

> 我知道动态内存在 MCU 上有碎片风险，所以通过 profile 和 runtime limits 限制输入输出大小。
> 多模态场景不把大文件塞进内存，而是传 URI/path/id。进一步优化可以把 allocator 也抽成 port，
> 让应用用固定内存池。

## 任务同步和取消传播

Agent runtime 里有多个可能长时间运行的步骤：

- provider 请求。
- tool 执行。
- stream 接收。
- event bus 异步投递。
- run queue worker。

RTOS 场景要保证：

- cancel token 能被 provider/tool 轮询。
- timeout 能传入 tool/provider context。
- event bus 不应无限积压。
- 每个 session 的 run 顺序可控。
- 工具并发要按 lane 限制。

面试表达：

> 我把取消做成协作式 cancel token，而不是强杀线程。RTOS 里强杀 task 可能导致锁和资源泄漏，
> 所以 provider/tool 应该周期性检查 token，并在安全点返回 `CC_ERR_CANCELLED`。

## 安全边界

MCU/RTOS 设备通常直接连接真实世界，工具能力要保守：

- shell/process 不适合 MCU profile。
- HTTP request 默认 deny。
- tool approval 默认保守。
- 文件写入必须受 workspace 限制。
- 日志和 event payload 必须 redaction。
- provider 429/5xx/timeout/cancel 要结构化返回，不在 SDK 内自动无限 retry。

面试表达：

> 在设备侧，LLM 不能直接控制高风险能力。SDK 只提供端口和安全策略，下游固件决定哪些工具可以暴露，
> 是否需要人工确认，是否需要云端 gateway 审核。

## MCU 方向简历表达

> 设计 C99 跨平台 Agent Runtime SDK，支持通过 CMake profile 裁剪到 MCU 文本和轻量多模态场景；
> 使用 ports/adapters 架构隔离 FreeRTOS/ESP32/POSIX 差异，实现线程、文件、路径、HTTP 等平台接口；
> 设计资源限制、协作式取消、stream callback、network allowlist 和工具审批机制，适配低资源和高安全要求的设备端运行环境。

## 常见面试问答

### 这个项目怎么体现嵌入式能力？

回答：

> 不是业务 UI，而是底层 SDK 架构：跨平台 port、profile 裁剪、线程同步、内存限制、文件路径安全、
> 网络 allowlist、取消和 timeout 传播。这些都是嵌入式软件开发里非常核心的能力。

### 如果 RAM 只有几百 KB 怎么办？

回答：

> 需要选择 `mcu-text` 或更小 profile，关闭多模态、HTTP 工具、SQLite、shell 和大 history；
> 限制 context window、stream bytes 和 tool result；store 可以换成 ring buffer 或 flash KV；
> 大媒体只传引用，不传 base64。

### FreeRTOS 没有进程概念怎么办？

回答：

> process port 本来就是可选能力。MCU profile 不编译 shell/process，业务工具通过 vtable 注册为普通 C 函数，
> 这样不会把 Linux 的进程模型强行带到 RTOS。

### 怎么避免工具阻塞实时任务？

回答：

> SDK 工具执行应放在非实时任务或 worker 中；实时采样、控制回路不应该调用 agent runtime。
> tool pool 和 timeout 能限制阻塞时间，cancel token 用于协作退出。
