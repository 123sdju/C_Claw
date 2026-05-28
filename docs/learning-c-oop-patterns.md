# C 语言面向对象与设计模式学习文档

这篇文档结合 C-Claw 讲 C 语言里的面向对象和设计模式。面试时不要说“C 不能面向对象”，
更准确的说法是：C 没有语言级 class，但可以用结构体、函数指针表和不透明指针实现面向对象风格。

## C 语言 OOP 的基本模型

C-Claw 里最常见的接口形态：

```c
typedef struct cc_xxx {
    void *self;
    const cc_xxx_vtable_t *vtable;
} cc_xxx_t;
```

含义：

- `self`：具体实现对象，相当于 C++ 里的 `this` 指针。
- `vtable`：函数指针表，相当于虚函数表。
- core 只依赖接口，不知道具体实现类型。
- adapter/platform 持有私有结构体并实现 vtable。

面试表达：

> 我用 `struct + vtable + void *self` 在 C 里实现运行时多态。core 调接口，adapter 提供实现。
> 这样 LLM provider、session store、tool、filesystem、thread 都可以替换，适合嵌入式跨平台 SDK。

## 接口隔离原则

项目把能力拆成多个 port：

- `cc_llm_provider_t`
- `cc_tool_t`
- `cc_session_store_t`
- `cc_memory_store_t`
- `cc_event_bus_t`
- `cc_filesystem_t`
- `cc_thread_t`
- `cc_http_client_t`
- `cc_policy_engine_t`

好处：

- core 不依赖具体平台。
- 单元测试可以用 fake vtable。
- MCU profile 可以不实现不需要的接口。
- adapter 可以独立演进。

面试表达：

> 我没有写一个巨大的全能接口，而是按职责拆 port。这符合接口隔离原则，也方便嵌入式裁剪：
> MCU 没有 HTTP 就不编译 HTTP 工具，没有 shell 就不实现 process port。

## 依赖注入

runtime 创建时注入依赖：

```text
cc_agent_runtime_deps_t
  llm
  tool_registry
  store
  policy
  sandbox
  event_bus
  logger
  memory_store
```

这就是 C 语言里的依赖注入。

面试表达：

> runtime 不自己 new provider，也不直接打开 SQLite 或 curl，而是由 builder 或应用注入依赖。
> 这样 core 可以用 fake provider 做测试，也可以在不同平台替换 store 和 filesystem。

常见追问：

- 为什么不在 runtime 里直接创建 provider？
  - 会把 core 和具体厂商、网络、配置耦合，破坏可移植性。
- 依赖由谁释放？
  - runtime 释放自己深拷贝的配置；注入端口由 builder 或应用按所有权释放。

## 策略模式

`cc_policy_engine_t` 是策略模式：

- runtime/tool executor 不知道具体安全规则。
- 默认策略可以判断 shell、删除文件等高风险工具。
- 下游可以替换策略实现。
- 策略返回 allowed、require_approval、reason。

面试表达：

> 安全策略不是写死在工具执行器里，而是抽成 policy engine。这样不同产品可以用不同策略：
> 开发板 demo 可以宽松，量产设备可以默认拒绝高风险工具。

## 适配器模式

Adapter 模式在项目中很明显：

- OpenAI/Ollama/Anthropic adapter：把 typed messages 转成厂商协议。
- SQLite/JSON/memory store adapter：把 session/memory port 落到不同存储。
- POSIX/FreeRTOS/ESP32 platform adapter：把 thread/filesystem/http port 落到平台 API。

面试表达：

> core 使用稳定的 typed message 和 vtable 接口；adapter 负责协议转换和平台细节。
> 比如 OpenAI 和 Anthropic 的 HTTP JSON 不一样，但 core 看到的都是 `cc_llm_provider_t`。

## 工厂模式

工厂模式用于根据配置创建默认实现：

- storage factory 根据配置选择 memory/json/sqlite store。
- runtime builder 根据 feature/profile 组合 provider、tool registry、store、event bus。
- built-in tool factory 创建 file/http/memory tool。

面试表达：

> 工厂把“根据配置选实现”的逻辑集中起来，runtime 主流程不需要知道 SQLite 和 JSON store 的创建细节。

## 观察者模式

event bus 和 observability 是观察者模式：

- publisher 不知道 subscriber 是日志、指标还是调试 UI。
- subscriber 按 event name 订阅。
- async mode 可以让观测逻辑不阻塞 runtime。
- payload 使用统一 schema，降低下游解析成本。

面试表达：

> 实时输出和观测是分开的。stream callback 给业务 UI，event bus 给日志和调试工具。
> 这就是观察者模式在 C SDK 里的应用。

## 状态机

stream loop 是状态机：

```text
TEXT        -> 累积 assistant 文本
THINKING    -> 累积 reasoning
TOOL_START  -> 记录当前工具名和 id
TOOL_DELTA  -> 拼接工具参数
TOOL_END    -> 执行工具并写回 tool message
FINISHED    -> 结束当前 provider stream
ERROR       -> 标记错误并上报
```

面试表达：

> 流式 provider 不一定一次性给完整 tool call，所以 runtime 需要一个状态机累计 tool 参数。
> 当 TOOL_END 到达时再执行工具，执行结果写回历史，然后进入下一轮 LLM 请求。

## 资源所有权模式

C 没有 RAII，所以项目靠明确 cleanup 约定：

- `cc_result_free()`
- `cc_message_cleanup()` / `cc_message_destroy()`
- `cc_tool_result_cleanup()`
- `cc_llm_response_free()`
- `cc_media_artifact_list_cleanup()`
- store/list result 的 free array 函数

面试表达：

> 我在 public API 里明确“谁分配谁释放”。成功返回的 `char *out_response` 由调用方 free，
> 结构体内部字段用 cleanup 函数释放。这是 C SDK 稳定性的关键。

## 错误处理模式

项目使用 `cc_result_t`：

- `code` 表示稳定错误分类。
- `message` 是可读错误信息。
- `detail` 放 HTTP status、retry_after、recoverable、redacted body。
- tool 内部失败尽量变成 `cc_tool_result_t`，让模型可恢复。

面试表达：

> 我区分系统错误和可恢复工具错误。比如 provider 失败会返回 `cc_result_t`；
> tool 参数错误会返回 tool result，让模型下一轮可以修正参数。

## 面试高频问答

### C 语言怎么实现多态？

回答：

> 用 `void *self` 保存具体对象，用 vtable 保存函数指针。调用方只拿接口结构，
> 调用 `vtable->method(self, ...)`。这和 C++ 虚函数思想类似，但 ABI 更透明。

### 这种方式有什么缺点？

回答：

> 缺点是没有编译器自动检查 self 类型，也没有构造/析构语法糖，所以需要严格的创建/销毁函数、
> size 字段、cleanup 约定和测试覆盖。

### 为什么适合嵌入式？

回答：

> C ABI 稳定，能和 BSP、RTOS、驱动、厂商 SDK 集成。vtable 又能实现可替换接口，
> 避免 core 直接依赖 Linux 或某个芯片平台。

### 项目里用了哪些设计模式？

回答：

> 端口是接口隔离，runtime deps 是依赖注入，policy engine 是策略模式，
> provider/platform/storage 是适配器模式，builder/factory 是工厂模式，
> event bus 是观察者模式，stream loop 是状态机。
