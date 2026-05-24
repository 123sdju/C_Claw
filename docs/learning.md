# C-Claw 深度学习文档 — 设计模式与架构面试精讲

> 本文档基于 C-Claw 项目的实际源码，深度解析纯 C 语言中面向对象编程、设计模式、
> 架构设计的工程实践，是准备 C/C++ 系统编程面试的优质学习资料。

---

## 目录

0. [面试学习路线：先会讲，再深挖源码](#0-面试学习路线先会讲再深挖源码)
1. [项目速览：一个真实的工程案例](#1-项目速览一个真实的工程案例)
2. [C 语言多态：VTable 模式的完整剖析](#2-c-语言多态vtable-模式的完整剖析)
3. [设计模式实战](#3-设计模式实战)
4. [端口-适配器架构（Ports & Adapters）](#4-端口-适配器架构ports--adapters)
5. [错误处理：Go 风格的结果类型](#5-错误处理go-风格的结果类型)
6. [依赖注入与生命周期管理](#6-依赖注入与生命周期管理)
7. [线程平台抽象层：一次编写，三平台运行](#7-线程平台抽象层一次编写三平台运行)
8. [互斥锁实战详解：POSIX / Windows / ESP32](#8-互斥锁实战详解posix--windows--esp32)
9. [核心数据结构的四种线程安全策略](#9-核心数据结构的四种线程安全策略)
10. [并发测试体系：27 个 CTest 覆盖关键共享状态](#10-并发测试体系27-个-ctest-覆盖关键共享状态)
11. [事件总线深度解析：从内部实现到全链路应用](#11-事件总线深度解析从内部实现到全链路应用)
12. [面试高频问答](#12-面试高频问答)
13. [简历指标取舍：优先讲工程价值，数字只作证据](#13-简历指标取舍优先讲工程价值数字只作证据)
14. [面试准备路线：从项目介绍到源码深挖](#14-面试准备路线从项目介绍到源码深挖)
15. [性能与体积 Benchmark 准备清单](#15-性能与体积-benchmark-准备清单)

---

## 0. 面试学习路线：先会讲，再深挖源码

这份文档建议按"一条主线 + 三个亮点 + 两个风险点"来准备，而不是逐行背源码。

**一条主线**：C-Claw 是一个纯 C 实现的 AI Agent 运行时。用户输入进入 CLI Gateway，Runtime
构建上下文并调用 LLM，LLM 如果返回工具调用，就经过 Tool Executor 的注册表查找、策略检查和工具执行，
结果写回会话存储，再进入下一轮 ReAct 循环，直到模型返回最终文本。

**三个亮点**：

1. **纯 C 多态**：用 `struct + vtable + void *self` 实现端口抽象，替代 C++ 虚函数。
2. **端口-适配器架构**：核心 Runtime 只依赖 `ports/`，OpenAI、SQLite、POSIX/Windows/ESP32 都是外层适配。
3. **并发安全设计**：工具注册表用 freeze 模式，事件总线用快照模式，日志和存储用互斥锁保护。

**两个风险点**：

1. `cc_tool_t`、`cc_llm_provider_t` 这类值类型是浅拷贝，必须讲清楚 self 的所有权归谁。
2. 事件总线是同步分发，handler 不应该做长时间阻塞；快照模式解决的是重入和锁持有时间，不是异步队列。

**推荐阅读顺序**：

| 顺序 | 文件 | 目标 |
|------|------|------|
| 1 | [`README.md`](../README.md) | 先建立项目边界：能力、profile、构建方式 |
| 2 | [`apps/posix/cli/src/main.c`](../apps/posix/cli/src/main.c) | 看桌面入口如何加载配置和创建 runtime |
| 3 | [`cclaw/core/src/app/cc_runtime_builder.c`](../cclaw/core/src/app/cc_runtime_builder.c) | 看依赖注入和生命周期装配 |
| 4 | [`cclaw/core/src/app/cc_agent_runtime.c`](../cclaw/core/src/app/cc_agent_runtime.c) | 看 ReAct 主循环和流式循环 |
| 5 | [`cclaw/core/src/app/cc_tool_executor.c`](../cclaw/core/src/app/cc_tool_executor.c) | 看工具调用的查找、策略、事件、容错 |
| 6 | [`cclaw/ports/include/cc/ports/cc_tool.h`](../cclaw/ports/include/cc/ports/cc_tool.h) | 掌握 vtable 端口写法 |
| 7 | [`cclaw/core/src/core/cc_event_bus.c`](../cclaw/core/src/core/cc_event_bus.c) | 掌握 snapshot 并发设计 |
| 8 | [`cclaw/tests/core/test_event_bus_concurrent.c`](../cclaw/tests/core/test_event_bus_concurrent.c) | 用测试反推设计保证 |

**60 秒项目介绍模板**：

> C-Claw 是一个纯 C 的 AI Agent 运行时，核心目标是在桌面和 ESP32 这类不同平台上复用同一套 Agent 编排逻辑。它采用端口-适配器架构：核心层只依赖 LLM、工具、存储、策略、事件等端口，具体的 OpenAI/Ollama/Anthropic、SQLite/JSON、POSIX/Windows/ESP32 都在外层适配。项目里最有价值的工程点是用 `struct + vtable + void *self` 在 C 中实现多态和依赖注入；Runtime Builder 负责把 10 多个组件装配起来并集中管理生命周期；Agent Runtime 实现 ReAct 循环，LLM 产出工具调用后经 Tool Executor 做注册表查找、策略审查、执行和事件审计。并发方面，工具注册表用 freeze 进入只读状态，事件总线发布时先做 handler 快照再锁外回调，避免回调重入导致的死锁和长时间持锁。

---

## 1. 项目速览：一个真实的工程案例

### 1.1 一句话概括

C-Claw 是一个 **纯 C99/C11 实现的 AI Agent 运行时**，支持 OpenAI/Ollama/Anthropic 等
LLM 后端，可以在 Linux、Windows 和 ESP32 上运行，通过插件系统扩展能力。

### 1.1.1 当前工程状态

这一版代码已经按主线能力重构为多层工程，学习时建议把“源码分层”和“构建分层”一起记：

```text
cclaw/      SDK：协议、调度、生命周期、并发语义、可移植端口
apps/       应用和板级组合：CLI、进程型 plugin、MCP transport、ESP32 QEMU、STM32 Renode
build/app/  应用或板级构建产物
build/sdk/  SDK 裁剪构建产物
```

当前已验证的入口：

```text
POSIX CLI tests       build/app/posix/cli          27/27 passed
core-minimal tests    build/sdk/core-minimal       18/18 passed
ESP32-S3 QEMU         build/app/esp32/esp32_s3_qemu  CCLAW_QEMU_PASS
STM32H743 Renode      build/app/stm32/stm32h743i_eval_renode  CCLAW_STM32H743_RENODE_PASS
```

STM32H743I-EVAL Renode 目标使用 Renode 自带的 STM32H743 CPU 平台描述，
第一版只要求 UART、CPU/RAM/Flash 和 FreeRTOS tick 跑通。

ESP32-S3 QEMU 默认固件参考大小：

```text
c_claw_esp32_s3_qemu.bin  272,800 bytes，约 266.4 KiB
factory app partition     1 MiB，约 757.6 KiB free
```

`docs/study.md` 是本地忽略的个人学习统计快照，不作为仓库正式文档提交。需要长期
维护的学习材料放在本文档和 `cclaw/docs/*`；需要更新代码量数字时，应重新统计当前
工作区并确认是否要同步到正式文档。

Windows CLI 的源码和 CMake 结构已经跟 POSIX 对齐，但仍需要真实 Windows 主机或
Windows 交叉工具链做最终编译验收。面试或复盘时要把它讲成“结构已准备，平台验收待补”，
不要把未跑过的环境说成已验证。

### 1.2 为什么值得学？

| 学习点 | 对应面试场景 |
|--------|------------|
| VTable 多态 | "C 语言如何实现多态？" |
| 工厂模式 + 构建器模式 | "你用过哪些设计模式？" |
| 端口-适配器架构 | "如何设计可测试、可迁移的系统？" |
| 错误处理模式 | "C 中如何优雅处理错误？" |
| 内存所有权 | "如何避免内存泄漏和悬空指针？" |
| 并发安全 | "多线程下如何保证线程安全？" |

### 1.3 项目规模

```
cclaw/                  ← 可移植 SDK（约 120+ 个 C/H 文件）
  core/                 ← 核心：Agent 循环、消息模型、错误类型、json、config
  ports/                ← 17 个抽象端口头文件（LLM、工具、存储、沙箱、事件、HTTP、文件系统、线程...）
  adapters/             ← 适配实现：OpenAI/Ollama/Anthropic Provider、SQLite/JSON 存储、策略引擎...
  platforms/            ← 平台层：POSIX、Windows、ESP32、FreeRTOS
  tests/                ← 21 个单元/并发测试文件

apps/                   ← 应用入口（约 40+ 个 C/H 文件）
  posix/cli/            ← Linux/macOS CLI（gateway、feature、plugin、mcp、sandbox、tools + 4 个测试）
  windows/cli/          ← Windows CLI（同 posix 对齐结构）
  esp32/esp32_s3_qemu/  ← ESP32-S3 QEMU（GPIO tool、feature）
  stm32/stm32h743i_eval_renode/ ← STM32H743 Renode（FreeRTOS/lwIP/HAL smoke）
```

当前仓库约有 **160+ 个 C/H 文件**，其中 ports/ 下 17 个端口头文件定义全部抽象接口，
测试文件分布为核心层 15 个 + 适配器层 2 个 + 平台层 1 个 + CLI 应用层 4 个。面试时不用背规模数字，但要能说清楚 `core/ports/adapters/platforms/apps` 的分层边界。

---

## 2. C 语言多态：VTable 模式的完整剖析

> 这是整个项目最核心的设计手法，也是面试中最容易被追问的部分。

### 2.1 问题引入：为什么 C 需要多态？

假设你要写一个支持多种 LLM 后端的系统：

```c
// 不好：每次新增 provider 都要改 switch-case
char *chat_with_llm(const char *provider, const char *prompt) {
    if (strcmp(provider, "openai") == 0)
        return openai_chat(prompt);
    else if (strcmp(provider, "ollama") == 0)
        return ollama_chat(prompt);
    // 新增 Anthropic 又要加分支...
}
```

**好的做法**：让上层只依赖抽象接口，不感知具体实现。

### 2.2 核心模式：struct + vtable + void *self

C-Claw 使用统一的 **两段式结构** 实现多态：

```c
// ============================================================
// 第一步：定义"多态句柄"——这是使用者看到的类型
// ============================================================
struct cc_llm_provider {
    void *self;                              // 指向具体实现的私有数据
    const cc_llm_provider_vtable_t *vtable;  // 指向函数指针表
};

// ============================================================
// 第二步：定义"虚函数表"——这是抽象接口定义
// ============================================================
struct cc_llm_provider_vtable {
    cc_result_t (*chat)(void *self, const cc_llm_chat_request_t *req,
                        cc_llm_response_t *out);
    cc_result_t (*chat_stream)(void *self, const cc_llm_chat_request_t *req,
                               cc_llm_stream_callback_fn cb, void *user_data);
    void (*destroy)(void *self);
};
```

**使用方式**：

```c
// 上层代码只依赖抽象类型
void process_message(cc_llm_provider_t provider, const char *prompt) {
    cc_llm_response_t resp;
    // 通过 vtable 间接调用——这是"多态"的关键
    cc_result_t rc = provider.vtable->chat(provider.self, &req, &resp);
    //                      ^^^^^^              ^^^^^^^^^^^^
    //                      函数指针表           传入 self 让函数知道操作谁
    if (rc.code == CC_OK && resp.has_text) {
        printf("Response: %s\n", resp.text);
    }
}
```

### 2.3 与 C++ 虚函数的对照

| 概念 | C++ | C (C-Claw) |
|------|-----|------------|
| 类定义 | `class Animal` | `struct animal`（存 self + vtable） |
| 虚函数声明 | `virtual void speak()` | `void (*speak)(void *self)` |
| 虚函数表 | 编译器自动生成 | 手动定义 `vtable` 静态常量 |
| 构造函数 | `new Cat()` | 工厂函数 `cat_create()` |
| this 指针 | 隐式 `this` | 显式 `void *self` |
| 析构函数 | `virtual ~Animal()` | `void (*destroy)(void *self)` |

**C++ 等价物**：

```cpp
// C++ 版本（等价于上面的 C 代码）
class LLMProvider {
public:
    virtual Result chat(const Request& req, Response& out) = 0;
    virtual Result chatStream(const Request& req, Callback cb, void* data) = 0;
    virtual ~LLMProvider() = default;
};

// 使用
void processMessage(LLMProvider& provider, const string& prompt) {
    Response resp;
    provider.chat(req, resp);  // 编译器自动查 vtable
}
```

### 2.4 具体实现示例：以 `cc_tool_t` 为例

**接口定义**（[cc_tool.h](../cclaw/ports/include/cc/ports/cc_tool.h)）：

```c
// 多态句柄（值语义，可直接拷贝）
struct cc_tool {
    void *self;
    const cc_tool_vtable_t *vtable;
};

// 虚函数表——相当于 C++ 的抽象类
struct cc_tool_vtable {
    const char *(*name)(void *self);           // 获取工具名
    const char *(*description)(void *self);    // 获取描述
    const char *(*schema_json)(void *self);    // 获取参数 schema
    cc_result_t (*call)(                       // 执行工具
        void *self,
        const char *args_json,
        const cc_tool_context_t *ctx,
        cc_tool_result_t *out_result
    );
    void (*destroy)(void *self);               // 析构
};

// ── 运行时服务集合（限制工具对 runtime 的访问范围）──
typedef struct cc_runtime_services {
    cc_event_bus_t *event_bus;          // 可选：发布进度事件
    cc_logger_t *logger;                // 可选：诊断日志
    cc_memory_store_t *memory_store;    // 可选：长期记忆
    cc_tool_executor_pool_t *tool_pool; // 可选：并发池
    cc_tool_approval_fn approve_tool_call; // 可选：人工审批回调
    void *approval_user_data;
} cc_runtime_services_t;

// ── 工具调用上下文（执行时传入的环境信息）──
typedef struct cc_tool_context {
    const char *session_id;
    const char *workspace_dir;
    const char *user_id;
    const cc_runtime_services_t *services;  // 受限服务集，避免工具拿到完整 runtime
    cc_cancel_token_t *cancel_token;        // 协作式取消
    int timeout_ms;                         // 策略超时
    const char *lane_name;                  // tool pool lane 名称
    unsigned long generation;               // registry 版本号
} cc_tool_context_t;
```

**具体实现之一：文件读取工具**

```c
// file_read 工具的私有数据
typedef struct {
    cc_filesystem_t fs;  // 文件系统能力
} cc_file_read_tool_t;

static const char *file_read_name(void *self) {
    (void)self;
    return "file_read";
}

static const char *file_read_description(void *self) {
    (void)self;
    return "Read the contents of a file";
}

static const char *file_read_schema_json(void *self) {
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Path to the file to read\"}"
        "},"
        "\"required\":[\"path\"]"
    "}";
}

static cc_result_t file_read_call(
    void *self, const char *args_json,
    const cc_tool_context_t *ctx, cc_tool_result_t *out
) {
    cc_file_read_tool_t *tool = (cc_file_read_tool_t *)self;
    // 真实代码会解析 args_json，读取 path 字段
    // 再通过 cc_path_join + cc_path_is_within 限定在 workspace_dir 内
    // 最后调用 tool->fs.vtable->read_text 读取文件
    return cc_result_ok();
}

static void file_read_destroy(void *self) {
    // 如果 self 指向堆内存：free(self)
    // 如果 self 指向栈变量：无需操作（本项目统一用堆分配）
    free(self);
}

// ── 虚函数表实例（静态全局表，所有 file_read 实例共享）──
static cc_tool_vtable_t file_read_vtable = {
    .name        = file_read_name,
    .description = file_read_description,
    .schema_json = file_read_schema_json,
    .call        = file_read_call,
    .destroy     = file_read_destroy
};

// ── 工厂函数：创建工具实例 ──
cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool) {
    cc_file_read_tool_t *self = calloc(1, sizeof(cc_file_read_tool_t));
    if (!self) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create file read tool");

    self->fs = fs;

    out_tool->self = self;
    out_tool->vtable = &file_read_vtable;
    return cc_result_ok();
}
```

注意这里有两个面试很容易追问的点：

- `cc_tool_t` 是值类型，但 `self` 指向堆对象；注册表保存的是浅拷贝。
- 工具执行的业务失败通常不通过 `cc_result_t` 向上中断，而是写入 `cc_tool_result_t.ok/error`，
  让 LLM 在下一轮看到错误并修正调用参数。

### 2.5 高级技巧：两层 VTable（协议分层）

C-Claw 的 LLM Provider 在适配器层使用 **两层 VTable** 将 HTTP 传输和 API 协议解耦：

```
cc_llm_provider_t          ← 端口层：vtable->chat() / chat_stream() + destroy()
     ↑ 工厂注入/组合
cc_http_llm_provider       ← 适配器层：共享的 HTTP 传输实现（SSE/NDJSON 流式分帧）
     ↑ 策略注入
cc_llm_protocol_t          ← 适配器层：API 协议的 vtable 策略
  ├── vtable->build_request()  → OpenAI/Ollama/Anthropic 各自构造请求
  └── vtable->parse_response() → 各自解析响应格式
```

```c
// 内层协议 vtable（定义在 cc_http_llm_provider.h）
struct cc_llm_protocol {
    void *self;
    const cc_llm_protocol_vtable_t *vtable;
};

struct cc_llm_protocol_vtable {
    const char *(*name)(void *self);
    cc_result_t (*build_request)(/* 构造 URL + body + headers */);
    cc_result_t (*parse_response)(/* 解析 HTTP body → cc_llm_response_t */);
    cc_result_t (*parse_stream_event)(/* 解析 SSE/NDJSON → cc_stream_chunk_t */);
    void (*destroy)(void *self);
};

// OpenAI 协议适配器
static const cc_llm_protocol_vtable_t openai_protocol_vtable = {
    .name               = openai_protocol_name,
    .build_request      = openai_build_request,     // POST /v1/chat/completions
    .parse_response     = openai_parse_response,     // JSON → cc_llm_response_t
    .parse_stream_event = openai_parse_stream_event, // SSE → cc_stream_chunk_t
    .destroy            = openai_protocol_destroy,
};

// Anthropic 协议适配器
static const cc_llm_protocol_vtable_t anthropic_protocol_vtable = {
    .name               = anthropic_protocol_name,
    .build_request      = anthropic_build_request,     // POST /v1/messages
    .parse_response     = anthropic_parse_response,     // 不同的 JSON 格式
    .parse_stream_event = anthropic_parse_stream_event, // SSE 格式也不同
    .destroy            = anthropic_protocol_destroy,
};
```

**这为什么是高级技巧？**

- 端口层 `cc_llm_provider_t` 只暴露 `chat/chat_stream/destroy` 三个虚函数，对上层保持极简
- 新增 LLM 只需实现内层 protocol vtable，通过 `cc_http_llm_provider_create()` 组合即可
- HTTP 传输层可独立测试（注入 mock protocol），Protocol 可独立测试（注入 mock HTTP client）
- 符合 **策略模式（Strategy Pattern）** + **开闭原则（OCP）**

---

## 3. 设计模式实战

### 3.1 工厂方法（Factory Method）

**场景**：根据配置字符串选择存储后端。

```c
cc_result_t cc_storage_factory_create_store(
    const cc_config_t *config,
    cc_session_store_t *out_store
) {
    if (!config || !out_store)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null config or out_store");

    memset(out_store, 0, sizeof(cc_session_store_t));

    if (config->storage_type && strcmp(config->storage_type, "memory") == 0)
        return cc_memory_session_store_create(out_store);

    if (config->storage_type &&
        (strcmp(config->storage_type, "json") == 0 ||
         strcmp(config->storage_type, "local_file") == 0)) {
        const char *path = config->storage_path ? config->storage_path : CC_DEFAULT_STORAGE_PATH;
        return cc_json_file_store_create(path, out_store);
    }

    if (config->storage_type && strcmp(config->storage_type, "sqlite") == 0) {
#if CC_STORAGE_SQLITE
        cc_result_t rc = cc_sqlite_session_store_create(config->storage_path, out_store);
        if (rc.code == CC_OK) return rc;
        cc_result_free(&rc);  // SQLite 失败时继续降级到 JSON
#else
        // SQLite 未编译时继续降级到 JSON
#endif
    }

    // 未指定、未知类型、SQLite 不可用/初始化失败：统一使用 JSON 文件存储
    return cc_json_file_store_create(
        config->storage_path ? config->storage_path : CC_DEFAULT_STORAGE_PATH,
        out_store);
}
```

**关键设计**：工厂只通过外部构造函数声明连接后端实现，调用方最终只拿到统一的
`cc_session_store_t`。这里的面试亮点不是"用了工厂"，而是**配置选择 + 编译裁剪 +
SQLite 失败降级到 JSON** 这三个工程细节。

### 3.2 构建器模式（Builder Pattern）

**场景**：`cc_agent_runtime` 需要聚合 10+ 个依赖组件，创建过程复杂。

```c
// 构建器是不透明类型，定义在 .c 文件中
struct cc_runtime_builder {
    const cc_runtime_feature_set_t *features;
    cc_logger_t *logger;
    cc_event_bus_t *event_bus;
    cc_filesystem_t fs;
    cc_session_store_t store;
    cc_llm_provider_t llm;
    cc_policy_engine_t policy;
    cc_memory_store_t memory_store;       // 可选长期记忆存储（值类型）
    cc_sandbox_t sandbox;
    cc_tool_registry_t *tool_registry;
    cc_tool_executor_pool_t *tool_pool;   // 工具并发池（lane 上限/timeout）
    cc_run_queue_t *run_queue;            // 多 Agent run queue（可选）
    cc_agent_manager_t *agent_manager;    // 多 Agent 编排入口（可选）
    cc_skill_catalog_t *skill_catalog;    // Skill catalog（可选）
    cc_agent_runtime_t *runtime;
    char *system_prompt;
    void *plugin_state;
    void *mcp_state;                   // MCP 加载器返回的不透明状态
};

// 单步构建——一次性装配所有组件
cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
) {
    // 1. 分配构建器
    // 2. 创建 logger → event_bus → filesystem
        // 3. 通过 features 工厂创建 store/llm/policy/sandbox/memory
        // 4. 遍历 features->tools 描述符表，逐一创建工具并注册
        // 5. 加载插件（通过 features->load_plugins）
        // 6. 加载 MCP servers（通过 features->load_mcp）
        // 7. 创建 tool_pool、run_queue、agent_manager、skill_catalog
        // 8. 组合 system_prompt
        // 9. 构建并发布 tool_registry_snapshot
        // 10. 将全部依赖注入 cc_agent_runtime_t
}

// 获取构建好的 product
cc_agent_runtime_t *cc_runtime_builder_runtime(cc_runtime_builder_t *builder);

// 级联销毁（builder 拥有所有子组件的生命周期）
void cc_runtime_builder_destroy(cc_runtime_builder_t *builder);
```

### 3.3 注册表模式（Registry Pattern）

**场景**：`cc_tool_registry_t` 提供工具的中央索引、查找和 Schema 生成。

```c
// 不透明类型，内部当前是固定容量数组（MAX_TOOLS=64）+ mutex
typedef struct cc_tool_registry cc_tool_registry_t;

cc_result_t cc_tool_registry_create(cc_tool_registry_t **out_registry);
cc_result_t cc_tool_registry_add(cc_tool_registry_t *reg, cc_tool_t tool);
cc_result_t cc_tool_registry_freeze(cc_tool_registry_t *reg);  // 冻结后只读
cc_result_t cc_tool_registry_find(cc_tool_registry_t *reg,
                                   const char *name, cc_tool_t *out);
cc_result_t cc_tool_registry_build_schema_json(cc_tool_registry_t *reg,
                                                char **out_json);
void cc_tool_registry_destroy(cc_tool_registry_t *reg);
```

**关键设计**：`freeze()` 冻结后变为只读状态，支持并发安全的 `find`/`build_schema`/`count` 操作。

### 3.4 观察者模式（Observer Pattern）

**场景**：`cc_event_bus_t` 实现发布-订阅，CLI Gateway 通过事件总线接收 LLM 和工具的实时状态。

```
事件类型：
  stream.thinking    → 模型的思考过程片段
  stream.text        → 用户可见的文本增量
  stream.tool.start  → 工具调用开始
  stream.tool.end    → 工具调用结束
  stream.finished    → 流式响应结束
  llm.request.started  → LLM 请求前
  llm.response.received → LLM 响应后
  tool.call.started    → 工具执行前
  tool.call.finished   → 工具执行后
  agent.finished       → Agent 完成
```

### 3.5 策略模式（Strategy Pattern）

**场景**：`cc_policy_engine_t` 封装安全检查策略。不同部署环境可注入不同的策略实现：
- 开发环境：所有操作自动批准
- 生产环境：高风险操作需要用户确认
- 服务器环境：Shell 命令自动拒绝

```c
struct cc_policy_engine {
    void *self;
    const cc_policy_engine_vtable_t *vtable;
};

struct cc_policy_engine_vtable {
    cc_result_t (*check_tool_call)(void *self, const cc_tool_call_t *call,
                                    const cc_tool_context_t *ctx,
                                    cc_policy_decision_t *out);
    void (*destroy)(void *self);
};
```

### 3.6 本项目使用的设计模式汇总

| 模式 | 应用位置 | resume 亮点 |
|------|---------|------------|
| **VTable 多态** | 所有 `cc_*_t` 端口 | "在纯 C 中手写虚函数表实现面向对象" |
| **工厂方法** | `cc_storage_factory` | "配置驱动选择实现，带编译时降级" |
| **构建器** | `cc_runtime_builder` | "聚合 10+ 依赖，级联生命周期管理" |
| **注册表** | `cc_tool_registry` | "freeze 冻结机制保证并发只读安全" |
| **观察者** | `cc_event_bus` | "CLI 通过事件总线实时渲染流式输出" |
| **策略** | `cc_policy_engine` | "运行时可替换的安全检查策略" |
| **依赖注入** | `cc_runtime_feature_set` | "通过描述符表注入所有可替换组件" |
| **代理/编排器** | `cc_agent_manager` | "多 Agent submit/collect/interrupt + cancel token 级联" |

### 3.7 多 Agent 编排（cc_agent_manager）

`cc_agent_manager_t` 是 SDK 层的多 Agent 编排入口。它不创建 LLM、store 或 plugin 进程，只保存
`agent_id → runtime` 的映射，并把每次 `handle_message` 放入 `cc_run_queue_t` 以支持
并发调度和协作式取消：

```c
// manager 选项
typedef struct cc_agent_manager_options {
    cc_agent_runtime_t *default_runtime;    // 默认 agent runtime
    cc_run_queue_t *queue;                  // 共享的 run queue
    int owns_queue;                         // manager 是否负责销毁 queue
    const char *default_agent_id;           // 默认 agent id
    cc_run_queue_action_t default_action;   // 默认提交动作
} cc_agent_manager_options_t;

// 核心接口
cc_result_t cc_agent_manager_add_agent(manager, agent_id, runtime);
cc_result_t cc_agent_manager_submit(manager, agent_id, session_id, user_input, &run_id);
cc_result_t cc_agent_manager_collect(manager, run_id, &out_response);
cc_result_t cc_agent_manager_interrupt(manager, agent_id, session_id);
cc_result_t cc_agent_manager_reset_session(manager, agent_id, session_id);
```

**关键设计**：
- `submit` 把交互输入映射为 run queue 的 `STEER` action——同 session 的 pending run 被替换，running run 收到 cancel token
- `reset_session` 先取消 pending/running run，再通过 store 的 `clear_session` 清空历史消息
- manager 本身不启动 watcher、不读取文件系统——热重载 plugin 和 skill 是 app/gateway 层的职责

---

## 4. 端口-适配器架构（Ports & Adapters）

### 4.1 核心思想

```
┌──────────────────────────────┐
│    Application (Core)        │ ← 业务逻辑：Agent 循环
│    只依赖 Ports，不依赖具体实现│
├──────────────────────────────┤
│    Ports (接口)               │ ← cc_tool_t、cc_llm_provider_t...
│    纯抽象，不含平台代码        │
├──────────────────────────────┤
│    Adapters (适配器)          │ ← OpenAI、SQLite、Docker、插件...
│    实现 Ports，可使用第三方库  │
├──────────────────────────────┤
│    Platform (平台层)          │ ← POSIX、Windows、ESP32
│    封装 OS 差异               │
└──────────────────────────────┘
```

### 4.2 依赖倒置原则（DIP）

```c
// ─── 正确方向：上层依赖抽象 ───
// cc_agent_runtime.c —— 核心业务逻辑
#include "cc/ports/cc_llm_provider.h"  // 只依赖端口（抽象）
#include "cc/ports/cc_tool.h"          // 不依赖具体实现

// cc_openai_provider.c —— 具体实现
#include "cc/ports/cc_llm_provider.h"  // 实现端口
#include "cc/adapters/cc_http_llm_provider.h"  // 可依赖第三方

// ✗ 错误方向（本项目中不存在）：
// #include "cc_http_llm_provider.h"  ← core 不应该看到 adapter
// #include <curl/curl.h>              ← core 不应该看到平台库
```

### 4.3 编译时能力裁剪

通过 CMake profile + 编译宏，在编译时决定哪些能力存在：

```c
// feature_set 描述符表
static const cc_llm_provider_descriptor_t llm_descriptors[] = {
#if CC_LLM_OPENAI
    { "openai",    1, openai_provider_create    },
#endif
#if CC_LLM_OLLAMA
    { "ollama",    1, ollama_provider_create    },
#endif
#if CC_LLM_ANTHROPIC
    { "anthropic", 1, anthropic_provider_create },
#endif
};

// 运行时根据配置名匹配：
for (size_t i = 0; i < count; i++) {
    if (desc[i].compiled && strcmp(desc[i].name, config->provider) == 0) {
        return desc[i].create(config, out);
    }
}
```

未编译的能力不仅不存在于二进制中，配置也无法在运行时开启。

---

## 5. 错误处理：Go 风格的结果类型

### 5.1 统一错误码模型

C-Claw 定义了类似 Go/Rust 的 **值语义错误类型**：

```c
typedef enum cc_error_code {
    CC_OK = 0,                    // 唯一成功码
    CC_ERR_UNKNOWN,               // 未知错误
    CC_ERR_INVALID_ARGUMENT,      // 参数无效（NULL、越界...）
    CC_ERR_OUT_OF_MEMORY,         // malloc 失败
    CC_ERR_NOT_FOUND,             // 查找的资源不存在
    CC_ERR_PERMISSION_DENIED,     // 策略引擎拒绝
    CC_ERR_IO,                    // 文件读写失败
    CC_ERR_NETWORK,               // HTTP 请求失败
    CC_ERR_JSON,                  // JSON 解析失败
    CC_ERR_TIMEOUT,               // 超时
    CC_ERR_CANCELLED,             // 用户取消
    CC_ERR_MODEL,                 // LLM 返回异常
    CC_ERR_TOOL,                  // 工具执行失败
    CC_ERR_STORAGE,               // 存储故障
    CC_ERR_PLATFORM               // OS API 故障
} cc_error_code_t;

typedef struct cc_result {
    cc_error_code_t code;    // 0 = 成功，非 0 = 错误类型
    char *message;           // 人类可读描述（堆分配，调用方释放）
} cc_result_t;
```

### 5.2 使用模式

```c
// ── 创建结果 ──
cc_result_t ok = cc_result_ok();                           // 成功
cc_result_t err = cc_result_error(CC_ERR_IO, "file not found");
cc_result_t err2 = cc_result_errf(CC_ERR_JSON,
                                   "parse error at line %d", line);

// ── 传播错误（类似 Go 的 if err != nil）──
cc_result_t rc = do_something();
if (rc.code != CC_OK) {
    // 不修改 rc，直接向上传播
    // 或者：包装后重新抛出
    cc_result_t wrapped = cc_result_errf(CC_ERR_TOOL,
        "tool execution failed: %s", rc.message);
    cc_result_free(&rc);
    return wrapped;
}

// ── 清理 ──
cc_result_free(&rc);  // 幂等：可以安全重复调用
```

### 5.3 与 errno 对比

| 方面 | errno | cc_result_t |
|------|-------|------------|
| 线程安全 | 需要 errno 线程本地存储 | 按值传递，天然线程安全 |
| 信息量 | 仅整型码 | 整型码 + 格式化字符串 |
| 忘记检查 | 编译器不警告 | 返回值类型强迫显式检查 |
| 内存管理 | 无 | message 需手动释放 |

---

## 6. 依赖注入与生命周期管理

### 6.1 所有权模型

C-Claw 的核心所有权规则：**"谁创建，谁销毁"**。

```c
// ── 所有权转移规则 ──

// 规则 1：值语义浅拷贝 + 所有权转移给注册表
cc_tool_t file_tool = {0};
cc_file_read_tool_create(fs, &file_tool);
cc_tool_registry_add(registry, file_tool);  // registry 保存浅拷贝并接管 self
// file_tool 和 registry 内的副本指向同一个 self
// add 成功后调用方不能再单独 destroy file_tool
// destroy 由 cc_tool_registry_destroy 统一负责

// 规则 2：指针语义 — 调用方拥有，runtime 借用
cc_tool_registry_t *reg = ...;  // 调用方拥有
cc_agent_runtime_create(&deps, &options, &runtime);
// runtime 内部只存 reg 指针，不拥有所有权
// 调用方负责：cc_tool_registry_destroy(reg)

// 规则 3：builder 级联销毁
cc_runtime_builder_destroy(builder);
// 内部自动销毁：logger、event_bus、store、llm、policy、
// sandbox、memory_store、tool_registry（含所有已注册工具）
```

### 6.2 依赖注入的三种方式

```c
// ── 方式 1：描述符表注入（编译时可选能力）──
typedef struct cc_llm_provider_descriptor {
    const char *name;                      // 配置中的名称
    int compiled;                          // 编译宏决定是否可用
    cc_runtime_llm_create_fn create;       // 工厂函数指针
} cc_llm_provider_descriptor_t;

// ── 方式 2：依赖聚合结构体注入（cc_agent_runtime_t 的构造）──
typedef struct cc_agent_runtime_deps {
    cc_llm_provider_t llm;            // 值拷贝 vtable
    cc_tool_registry_t *tool_registry; // 指针借用
    cc_session_store_t store;          // 值拷贝 vtable
    cc_policy_engine_t policy;         // 值拷贝 vtable
    cc_sandbox_t sandbox;              // 值拷贝 vtable
    cc_event_bus_t *event_bus;         // 指针借用（可选）
    cc_logger_t *logger;               // 指针借用（可选）
    cc_memory_store_t *memory_store;   // 指针借用（可选）
    cc_tool_executor_pool_t *tool_pool;// 指针借用（可选，工具并发控制）
    cc_tool_approval_fn approve_tool_call; // 函数指针
    void *approval_user_data;
} cc_agent_runtime_deps_t;

// ── 方式 3：Feature Set 注入（构建器使用的总入口）──
typedef struct cc_runtime_feature_set {
    const cc_llm_provider_descriptor_t *llm_providers;
    size_t llm_provider_count;
    const cc_tool_descriptor_t *tools;
    size_t tool_count;
    cc_runtime_session_store_create_fn create_session_store;
    cc_runtime_memory_store_create_fn create_memory_store;   // 可选
    cc_runtime_policy_create_fn create_policy_engine;
    cc_runtime_sandbox_create_fn create_sandbox;              // 可选
    cc_runtime_plugin_load_fn load_plugins;                   // 可选
    cc_runtime_plugin_destroy_fn destroy_plugins;             // 可选
    cc_runtime_mcp_load_fn load_mcp;                          // 可选：MCP transport 注入
    cc_runtime_mcp_destroy_fn destroy_mcp;                    // 可选
} cc_runtime_feature_set_t;
```

---

## 7. 线程平台抽象层：一次编写，三平台运行

> 项目通过不透明类型 + 平台适配层，用同一套接口在 POSIX、Windows、ESP32 上管理线程。

### 7.1 抽象接口设计

```c
// 不透明类型 — 调用者不知道底层是什么
typedef void *cc_thread_t;    // POSIX: pthread_t*, Windows: HANDLE, ESP32: TaskHandle_t
typedef void *cc_mutex_t;     // POSIX: pthread_mutex_t*, Windows: CRITICAL_SECTION*, ESP32: SemaphoreHandle_t
typedef void *cc_cond_t;      // POSIX: pthread_cond_t*, Windows: CONDITION_VARIABLE*, ESP32: 通过事件组模拟

// 统一 API — 线程
cc_result_t cc_thread_create(cc_thread_fn_t fn, void *arg, cc_thread_t *out_thread);
cc_result_t cc_thread_join(cc_thread_t thread);

// 统一 API — 互斥锁
cc_result_t cc_mutex_create(cc_mutex_t *out_mutex);
void        cc_mutex_destroy(cc_mutex_t mutex);
void        cc_mutex_lock(cc_mutex_t mutex);
void        cc_mutex_unlock(cc_mutex_t mutex);

// 统一 API — 条件变量（用于 run queue、tool pool 等等待唤醒场景）
cc_result_t cc_cond_create(cc_cond_t *out_cond);
void        cc_cond_destroy(cc_cond_t cond);
void        cc_cond_wait(cc_cond_t cond, cc_mutex_t mutex);
int         cc_cond_timedwait(cc_cond_t cond, cc_mutex_t mutex, int timeout_ms);
void        cc_cond_signal(cc_cond_t cond);
void        cc_cond_broadcast(cc_cond_t cond);
```

**关键设计决策：为什么条件变量放在 port 层？**

run queue、plugin worker pool、MCP runtime sweep 都需要阻塞等待能力。
如果只用轮询 sleep，会在桌面浪费 CPU，也会让 ESP 这类设备更耗电。
`cc_cond_timedwait` 配合协作式取消令牌：core 队列或 tool pool 可以每隔几十毫秒醒来
查询 `cc_cancel_token`，而不需要平台层知道上层取消令牌的具体语义。

**关键设计决策：为什么 `cc_mutex_t` 用 `void *` 而非结构体封装？**

```c
// 方案 A（本项目采用）：不透明指针
typedef void *cc_mutex_t;       // 调用方完全不知道大小，必须堆分配

// 方案 B（备选）：固定大小的 char 数组
typedef char cc_mutex_t[64];     // 平台最大 sizeof 预留，可以栈分配
                                  // 缺点：浪费栈空间，== 比较语义模糊

// 方案 A 的优势：
// 1. 调用方 sizeof(cc_mutex_t) == sizeof(void*) → 可以嵌入其他 struct
// 2. 编译防火墙：上层不依赖 pthread.h / windows.h / FreeRTOS.h
// 3. 二进制兼容：升级 pthread 库不需要重新编译上层代码
```

### 7.2 平台适配对照

| 能力 | POSIX (Linux/macOS) | Windows | ESP32 (FreeRTOS) |
|------|---------------------|---------|-------------------|
| 线程创建 | `pthread_create` | `CreateThread` | `xTaskCreate` |
| 线程等待 | `pthread_join` + `free(handle)` | `WaitForSingleObject` + `CloseHandle` | 二值信号量 `xSemaphoreTake` |
| 互斥锁 | `pthread_mutex_t`（堆分配） | `CRITICAL_SECTION`（堆分配） | `xSemaphoreCreateMutex` |
| 递归语义 | 显式设置 `PTHREAD_MUTEX_RECURSIVE` | `CRITICAL_SECTION` 内建支持 | 当前实现不提供递归语义 |

---

## 8. 互斥锁实战详解：POSIX / Windows / ESP32

> 三个平台实现了完全相同的 `cc_mutex_*` 接口，但内部细节有重要的工程考量。

### 8.1 POSIX 实现 — 为什么用递归锁？

```c
// 文件: cclaw/platforms/posix/src/cc_posix_thread.c

cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    pthread_mutex_t *mutex = malloc(sizeof(pthread_mutex_t));
    if (!mutex)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate mutex");

    // ═══ 关键：设置递归锁属性 ═══
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    //             允许同一线程重复加锁，不会自死锁 ^^^^^^^^

    int rc = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if (rc != 0) {
        free(mutex);
        return cc_result_error(CC_ERR_PLATFORM, "Failed to initialize mutex");
    }
    *out_mutex = mutex;
    return cc_result_ok();
}

void cc_mutex_lock(cc_mutex_t mutex) {
    if (mutex) pthread_mutex_lock((pthread_mutex_t *)mutex);
}

void cc_mutex_unlock(cc_mutex_t mutex) {
    if (mutex) pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

void cc_mutex_destroy(cc_mutex_t mutex) {
    if (!mutex) return;
    pthread_mutex_destroy((pthread_mutex_t *)mutex);
    free(mutex);
}
```

**为什么不使用普通互斥锁（`PTHREAD_MUTEX_NORMAL`）？**

主要是为了让 POSIX 平台与 Windows `CRITICAL_SECTION` 的行为更接近：同一线程如果在某条调用链里重复进入同一把锁，
不会立刻自死锁。注意，**事件总线的嵌套 publish 并不依赖递归锁**，它真正的安全点是快照模式：
`publish` 在锁内复制 handler 列表，释放锁后才执行回调。递归锁只是平台抽象层的防御性选择，不能替代正确的锁边界设计。

### 8.2 Windows 实现 — 为什么用 CRITICAL_SECTION？

```c
// 文件: cclaw/platforms/windows/src/cc_windows_thread.c

cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    CRITICAL_SECTION *cs = malloc(sizeof(CRITICAL_SECTION));
    if (!cs)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "...");
    InitializeCriticalSection(cs);
    *out_mutex = cs;
    return cc_result_ok();
}

void cc_mutex_lock(cc_mutex_t mutex) {
    if (mutex) EnterCriticalSection((CRITICAL_SECTION *)mutex);
}

void cc_mutex_unlock(cc_mutex_t mutex) {
    if (mutex) LeaveCriticalSection((CRITICAL_SECTION *)mutex);
}

void cc_mutex_destroy(cc_mutex_t mutex) {
    if (!mutex) return;
    DeleteCriticalSection((CRITICAL_SECTION *)mutex);
    free(mutex);
}
```

| 比较维度 | CRITICAL_SECTION | Windows Mutex |
|---------|------------------|---------------|
| 作用域 | 同一进程内 | 可跨进程 |
| 速度 | 用户态，仅竞争时进内核 | 始终内核态 |
| 递归 | 内建支持 | 内建支持 |
| 本项目选择 | ✅ | ❌ 不需要跨进程 |

### 8.3 ESP32 实现 — FreeRTOS 互斥信号量

```c
// 文件: cclaw/platforms/esp32/src/cc_esp32_thread.c

cc_result_t cc_mutex_create(cc_mutex_t *out_mutex)
{
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    // 创建 FreeRTOS 互斥信号量 — 自动启用优先级继承
    if (!mutex)
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create mutex");
    *out_mutex = mutex;
    return cc_result_ok();
}

void cc_mutex_lock(cc_mutex_t mutex) {
    if (mutex) xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
    //                                        无限等待，不设置超时 ^^^^^^^^^^^^
}

void cc_mutex_unlock(cc_mutex_t mutex) {
    if (mutex) xSemaphoreGive((SemaphoreHandle_t)mutex);
}
```

**ESP32 平台的独特安全考量**：

- `xSemaphoreCreateMutex()` 自动启用 **优先级继承** — 防止优先级反转
- 当前实现使用普通 FreeRTOS mutex，不是 recursive mutex；上层不能依赖同线程重复加锁
- 资源有限时 `xSemaphoreCreateMutex()` 可能返回 NULL — 通过 `cc_result_t` 传播错误
- ESP32 线程 join 通过二值信号量模拟（FreeRTOS 无 `pthread_join`）

---

## 9. 核心数据结构的四种线程安全策略

> 项目没有使用 C11 `_Atomic`、CAS 操作或读写锁，所有同步完全依赖 `cc_mutex_t`。
> 但在使用方式上，有四种不同的策略模式。

### 9.1 策略一：Freeze + Snapshot 模式（工具注册表）

**核心思想**：两层机制配合——freeze 保证单个 registry 不可变，snapshot 在此基础上用 generation 实现热重载。

**Freeze 层（cc_tool_registry）**：将可变状态分两阶段 — 初始化阶段可写，冻结后变为只读。每个 registry 实例独立 freeze，不可逆。

```c
// 文件: cclaw/core/src/core/cc_tool_registry.c

struct cc_tool_registry {
    cc_tool_t tools[64];   // 固定容量数组
    size_t count;
    int frozen;            // 0 = 可添加，1 = 冻结（单向标志，不可逆）
    cc_mutex_t mutex;
};

// ─── 写入操作：冻结前可用，冻结后拒绝 ───
cc_result_t cc_tool_registry_add(cc_tool_registry_t *registry, cc_tool_t tool)
{
    cc_mutex_lock(registry->mutex);
    
    if (registry->frozen) {           // ← 第一个检查点
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Tool registry is frozen");
    }
    if (registry->count >= 64) {
        cc_mutex_unlock(registry->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Tool registry full");
    }
    
    registry->tools[registry->count++] = tool;  // 值拷贝（浅拷贝 vtable+self）
    cc_mutex_unlock(registry->mutex);
    return cc_result_ok();
}

// ─── 冻结操作：单向标志 ───
cc_result_t cc_tool_registry_freeze(cc_tool_registry_t *registry)
{
    cc_mutex_lock(registry->mutex);
    registry->frozen = 1;          // 单向，不可逆
    cc_mutex_unlock(registry->mutex);
    return cc_result_ok();
}

// ─── 读取操作：持锁查找，返回浅拷贝 ───
cc_result_t cc_tool_registry_find(
    cc_tool_registry_t *registry, const char *name, cc_tool_t *out_tool)
{
    cc_mutex_lock(registry->mutex);
    for (size_t i = 0; i < registry->count; i++) {
        if (strcmp(registry->tools[i].vtable->name(registry->tools[i].self), name) == 0) {
            *out_tool = registry->tools[i];   // ← 浅拷贝！调用者拿到独立副本
            cc_mutex_unlock(registry->mutex);
            return cc_result_ok();
        }
    }
    cc_mutex_unlock(registry->mutex);
    return cc_result_errf(CC_ERR_NOT_FOUND, "Tool not found: %s", name);
}
```

**Freeze 模式的线程安全保证**：

```
时间线 ──────────────────────────────────────────────────────────→

[初始化阶段]                     [运行阶段 — freeze 后]
  add("file_read")    ──freeze()──→  find("file_read") ─┐
  add("shell_run")                  find("shell_run")   ├─ 并发安全
  add("memory")                     build_schema_json() ─┘
  持锁写入                          持锁读取（只读路径）
  可并发 add（锁保护）               不能 add（frozen 检查拒绝）
```

**面试要点**：为什么冻结后 `find` 返回**浅拷贝**而不是指针？

```c
// 危险做法 — 返回内部指针：
cc_tool_t *cc_tool_registry_find_raw(...) {
    cc_mutex_lock(mutex);
    for (...) {
        if (match) {
            cc_mutex_unlock(mutex);   // ← 释放锁
            return &registry->tools[i]; // ← 悬空！其他线程可能修改 tools
        }
    }
}

// 安全做法 — 返回浅拷贝：
cc_result_t cc_tool_registry_find(..., cc_tool_t *out_tool) {
    cc_mutex_lock(mutex);
    for (...) {
        if (match) {
            *out_tool = registry->tools[i]; // 值拷贝 vtable+self 指针
            cc_mutex_unlock(mutex);         // 锁释放后 out_tool 仍然有效
            return cc_result_ok();
        }
    }
}
```
问：为什么不用 `const cc_tool_t *` 指向内部位置？
答：无法保证线程安全——锁释放后内部指针变成悬空引用。返回浅拷贝让调用者在锁外安全使用。

**Snapshot 层（cc_tool_registry_snapshot）**：在 freeze 的基础上，用 generation（版本号）+ ref_count（引用计数）实现工具热重载。它不是替代 freeze，而是把多个已冻结的 registry 串成一个"版本链"：

```c
// 文件: cclaw/core/src/app/cc_tool_registry_snapshot.c

struct cc_tool_registry_snapshot {
    cc_tool_registry_t *registry;   // 本 generation 的已冻结注册表
    unsigned long generation;       // 单调递增版本号
    int owns_registry;              // 是否负责销毁 registry
    size_t ref_count;               // 引用计数（当前在用此 snapshot 的 run 数）
    cc_mutex_t mutex;
};
```

**热重载流程**（runtime_builder 的 `cc_runtime_builder_reload()`）：

```
1. 构建新 tool_registry（空）
2. 遍历 features→tools 注册新工具
3. cc_tool_registry_freeze(new_registry)     ← 新 registry 冻结！
4. cc_tool_registry_snapshot_create(          ← 包装为 snapshot
       new_registry, generation+1, owns=1)
5. 原子 swap: 旧 snapshot → retired 列表
              新 snapshot → 当前活跃
6. 旧 snapshot 的 ref_count 降到 0 后自动销毁
```

```
[旧 generation N]                [新 generation N+1]
                    reload()
 registry (frozen) ────────→  registry (frozen)   ← 也是冻结的！
 snapshot (ref=3)               snapshot (ref=1)
   ├─ run A 持有
   ├─ run B 持有
   └─ run C 持有
                                 └─ 新 run D acquire
```

- **reload 失败**时不替换 snapshot，旧 generation 继续服务
- **reload 成功**后旧 registry 进入 retired 列表，等 ref_count=0 时销毁
- 每个 registry **依然需要 freeze**——如果不冻结，run 期间的 add 调用会破坏一致性

**面试要点**：用两句话讲清楚 freeze 和 snapshot 的关系——
"freeze 保证单个 registry 是不可变的（冻结后不能 add）。snapshot 在 freeze 之上加了引用计数和版本号，让热重载时新旧 registry 同时存在——旧的继续服务已有 run，新的接管后续 run。"

### 9.2 策略二：快照模式 / Snapshot Pattern（事件总线）

**核心思想**：锁内只做浅拷贝，锁外执行可能耗时的回调——这是项目中最精妙的并发设计。

```c
// 文件: cclaw/core/src/core/cc_event_bus.c

struct cc_event_bus {
    cc_event_handler_entry_t handlers[64];  // 固定容量
    size_t count;
    cc_mutex_t mutex;
};

cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus, const char *event_type, const char *event_json)
{
    // ═══════════════════════════════════════════════════
    // 阶段 1：锁内 — 构建 handlers 快照
    // ═══════════════════════════════════════════════════
    cc_event_handler_entry_t snapshot[64];  // ← 栈上分配，O(1) 空间
    size_t snapshot_count = 0;

    cc_mutex_lock(bus->mutex);
    for (size_t i = 0; i < bus->count; i++) {
        cc_event_handler_entry_t *entry = &bus->handlers[i];
        // 匹配事件名（NULL = 通配订阅，接收所有事件）
        if (entry->event_type == NULL ||
            strcmp(entry->event_type, event_type) == 0) {
            snapshot[snapshot_count++] = *entry;  // ← 浅拷贝三个指针字段
        }
    }
    cc_mutex_unlock(bus->mutex);
    // ═══════════════════════════════════════════
    // 锁已释放！阶段 2：执行 handler 回调
    // ═══════════════════════════════════════════

    for (size_t i = 0; i < snapshot_count; i++) {
        snapshot[i].handler(event_type, event_json, snapshot[i].user_data);
        //      ^^^^^^^^^^^^^^^^ 回调可以在 handler 内安全调用 subscribe/publish
    }
    return cc_result_ok();
}
```

**快照模式解决的问题矩阵**：

| 场景 | 如果全程持锁 | 快照模式 |
|------|------------|---------|
| handler 内调用 `subscribe` | 可能长时间持锁，且修改正在遍历的集合 | ✅ 当前发布只使用旧快照 |
| handler 内调用 `publish` | 依赖递归锁才不死锁，语义复杂 | ✅ 外层锁已释放，嵌套发布清晰 |
| handler 执行时间长（写文件、网络 IO） | 其他线程阻塞 | ✅ 不阻塞其他线程 |

当前事件总线没有 `unsubscribe` 接口，订阅关系按"初始化阶段注册，运行期基本静态"来设计。

**面试追问：为什么用栈上的 `snapshot[64]` 而不用 `malloc`？**

```c
// 栈分配：snapshot[64] 约 1.5KB → 在栈上，无 malloc 开销
// 堆分配：malloc(count * sizeof(entry)) → 需要 free、可能失败
// 栈：无失败风险，缓存友好
// 缺点：递归 publish 时每次递归都消耗栈空间 → Handler 不应无限递归
```

### 9.3 策略三：全程持锁（日志系统）

**核心思想**：从级别判断到输出完成全程持锁，保证单条日志行完整不交错。

```c
// 文件: cclaw/core/src/util/cc_logger.c

struct cc_logger {
    char *name;
    cc_log_level_t level;   // ← 被 mutex 保护
    cc_mutex_t mutex;
};

void cc_logger_log(cc_logger_t *logger, cc_log_level_t level, const char *fmt, ...)
{
    if (!logger) return;

    cc_mutex_lock(logger->mutex);
    // ═══════════════════════ 锁保护区间开始 ═══════════════════════

    // 级别过滤（在格式化之前做，避免无用的 CPU 和 I/O 开销）
    if (level < logger->level) {
        cc_mutex_unlock(logger->mutex);
        return;
    }

    // 时间戳 — 必须用线程安全版本
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);  // ← _r 后缀 = reentrant（线程安全）
    //           ^ 调用者提供缓冲区，不使用 static 变量
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    // 输出前缀：[日期 时间] [级别] [模块名]
    fprintf(stderr, "[%s] [%s] [%s] ", time_buf, level_string(level), logger->name);

    // 输出消息体
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);  // 即时刷新 — 崩溃时不丢日志

    // ═══════════════════════ 锁保护区间结束 ═══════════════════════
    cc_mutex_unlock(logger->mutex);
}

void cc_logger_set_level(cc_logger_t *logger, cc_log_level_t level)
{
    cc_mutex_lock(logger->mutex);
    logger->level = level;       // 持锁写入
    cc_mutex_unlock(logger->mutex);
}
```

**全程持锁 vs 快照模式的选择条件**：

| 条件 | 快照模式 | 全程持锁 |
|------|---------|---------|
| 临界区操作时间 | 不关心（锁外执行） | 必须很短（几微秒） |
| 回调是否可能重入 | 安全 | 可能死锁 |
| 是否需要严格顺序 | 快照时点确定 | 实时确定 |
| 典型场景 | 事件分发 | 日志输出、简单读写 |

**为什么日志不适合快照模式？**

```c
// 如果日志用快照模式：
cc_mutex_lock(mutex);
char *msg_copy = strdup(formatted_msg);  // malloc
cc_mutex_unlock(mutex);
fprintf(stderr, "%s\n", msg_copy);       // 锁外输出
free(msg_copy);
// 问题：线程 A 的日志可能在线程 B 之后输出 → 时序颠倒！
```

### 9.4 策略四：标准互斥锁（存储后端）

**核心思想**：最简单也最鲁棒 — 所有读写操作都通过互斥锁保护。

```c
// 文件: cclaw/adapters/src/storage/cc_inmem_memory_store.c

typedef struct {
    cc_memory_entry_t *entries;
    size_t count;
    size_t cap;
    cc_mutex_t mutex;          // ← 保护 entries/count/cap
} cc_inmem_memory_store_t;

static cc_result_t inmem_set(void *self, const cc_memory_entry_t *entry)
{
    cc_inmem_memory_store_t *store = (cc_inmem_memory_store_t *)self;

    cc_mutex_lock(store->mutex);
    // ... 修改 entries 数组 ...
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}

static cc_result_t inmem_get(void *self, const char *key, cc_memory_entry_t *out)
{
    cc_inmem_memory_store_t *store = (cc_inmem_memory_store_t *)self;

    cc_mutex_lock(store->mutex);
    // ... 查找 entries 数组，拷贝到 out ...
    cc_mutex_unlock(store->mutex);
    return cc_result_ok();
}
```

**SQLite 后端的特殊处理**：

```c
// 文件: cclaw/adapters/src/storage/cc_sqlite_session_store.c
// SQLite 自身支持 SQLITE_OPEN_FULLMUTEX 模式 — 线程安全
// 但项目额外加了 cc_mutex_t 保护，原因：
// 1. 多语句事务需要原子性
// 2. last_insert_rowid 在多线程下需要序列化
// 3. 防御性编程：不信任 SQLite 的线程模式切换行为
```

---

## 10. 并发测试体系：27 个 CTest 覆盖关键共享状态

### 10.1 并发测试矩阵

| 测试文件 | 测试目标 | 线程数 | 压力规模 | 验证内容 |
|----------|---------|--------|---------|---------|
| `test_thread_mutex` | Mutex 基础正确性 | 8 | 80,000 次自增 | 最终计数 == 80,000 |
| `test_tool_registry_freeze` | freeze 后并发读 | 4 | 4,000 次 find+schema | 无崩溃、返回一致 |
| `test_event_bus_concurrent` | 发布/订阅 + 嵌套事件 | 4 | 800 发布 + 800 嵌套 | 事件计数完整 |
| `test_logger_concurrent` | 并发日志写入 | 4 | 800 条日志 | 无崩溃、无死锁 |
| `test_memory_store_concurrent` | 存储并发追加 | 4 | 400 条消息 | 每个 session 恰好 100 条 |
| `test_runtime_concurrent_sessions` | Runtime 多 session | 4 | 4 session 并行 | 独立 session 无干扰 |

### 10.0 扩展测试套件（当前版本新增）

除上述 6 个并发测试外，当前版本还包含了以下专项测试：

| 测试文件 | 测试目标 |
|----------|---------|
| `test_run_queue_session_serial` | run queue session 串行执行语义 |
| `test_run_queue_async_interrupt` | run queue 异步中断和 cancel token |
| `test_tool_executor_pool_lane` | tool pool lane 并发上限和排队 |
| `test_cancel_token` | cancel token 创建/取消/清理语义 |
| `test_sse_parser` | SSE 增量状态机分 chunk 解析 |
| `test_skill_catalog_prompt` | skill catalog SKILL.md 解析和 prompt 生成 |
| `test_mcp_runtime_manager_fake_transport` | MCP transport fake 注入和 tools/list |
| `test_mcp_jsonrpc_matcher` | MCP JSON-RPC 响应匹配 |
| `test_plugin_protocol_envelope` | JSON-RPC 2.0 envelope 构建/解析 |
| `test_config_runtime_sections` | config runtime section 解析 |
| `test_config_missing_defaults` | config 默认值降级 |
| `test_runtime_request_config` | runtime request 配置参数构造 |
| `test_tool_executor_policy_approval` | 工具执行策略审批流程 |
| `test_process_capture` | POSIX 进程输出捕获 |
| `test_message_envelope_serialization` | 消息信封序列化 |

### 10.2 基础测试：互斥锁能否阻止竞态

```c
// 文件: cclaw/tests/core/test_thread_mutex.c

#define N_THREADS 8
#define N_INCREMENTS 10000

static int shared_counter = 0;
static cc_mutex_t g_mutex;

static void *increment_thread(void *arg) {
    for (int i = 0; i < N_INCREMENTS; i++) {
        cc_mutex_lock(g_mutex);
        shared_counter++;               // 临界区：读-改-写
        cc_mutex_unlock(g_mutex);
    }
    return NULL;
}

void test_mutex_safety() {
    cc_mutex_create(&g_mutex);

    cc_thread_t threads[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        cc_thread_create(increment_thread, NULL, &threads[i]);

    for (int i = 0; i < N_THREADS; i++)
        cc_thread_join(threads[i]);

    assert(shared_counter == N_THREADS * N_INCREMENTS);  // 80,000
    cc_mutex_destroy(g_mutex);
}
```

**面试追问：如果不加锁会怎样？**

```
线程 A：读取 counter = 42
线程 B：读取 counter = 42  （同时读，都拿到 42）
线程 A：写入 counter = 43
线程 B：写入 counter = 43  ← 丢失了一次递增！
预期：44，实际：43
```

### 10.3 高级测试：事件总线并发 + 嵌套

```c
// 文件: cclaw/tests/core/test_event_bus_concurrent.c

#define N_PUBLISHERS 4
#define N_EVENTS     200

typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    int count;
} event_ctx_t;

static void handler(const char *type, const char *json, void *data) {
    event_ctx_t *ctx = (event_ctx_t *)data;
    cc_mutex_lock(ctx->mutex);
    ctx->count++;
    cc_mutex_unlock(ctx->mutex);

    // 模拟：handler 内部触发嵌套 publish
    // 安全的关键是 publish 已经释放 bus->mutex 后才调用 handler
    if (type && type[0] == 'r') {  // "root" 事件
        cc_event_bus_publish(ctx->bus, "nested", "{}");
    }
}

static void *publisher(void *arg) {
    event_ctx_t *ctx = (event_ctx_t *)arg;
    for (int i = 0; i < N_EVENTS; i++) {
        cc_event_bus_publish(ctx->bus, "root", "{}");
    }
    return NULL;
}

void test_concurrent_publish_subscribe() {
    event_ctx_t ctx = {0};
    cc_event_bus_create(&ctx.bus);
    cc_mutex_create(&ctx.mutex);
    cc_event_bus_subscribe(ctx.bus, NULL, handler, &ctx);  // 通配订阅

    cc_thread_t publishers[N_PUBLISHERS];
    for (int i = 0; i < N_PUBLISHERS; i++) {
        cc_thread_create(publisher, &ctx, &publishers[i]);
    }
    for (int i = 0; i < N_PUBLISHERS; i++)
        cc_thread_join(publishers[i]);

    // 每个 publisher 发 200 条 root → handler 内又发 200 条 nested
    assert(ctx.count == N_PUBLISHERS * N_EVENTS * 2);  // 1,600
}
```

---

## 11. 事件总线深度解析：从内部实现到全链路应用

> 本章完整解析事件总线的数据结构、发布/订阅逻辑、快照模式的线程安全设计、
> 以及在 Agent 运行时和 CLI Gateway 中的端到端使用流程。

### 11.1 为什么需要事件总线？

在 C-Claw 中，Agent 运行时内部有大量事情在发生：LLM 开始请求、思考内容生成、
文本增量输出、工具调用开始/结束、Agent 循环终止。CLI Gateway 需要实时展示这些
状态给用户，但它不应该与运行时代码耦合。

**问题**：如何让 Runtime 通知 Gateway，而不让 Runtime 知道 Gateway 的存在？

**答案**：事件总线（Event Bus）—— Runtime 只负责发布事件，Gateway 只负责订阅事件，
二者通过**事件类型字符串**进行松耦合通信。

```
┌─────────────────┐        事件发布           ┌──────────────────┐
│ cc_agent_runtime │ ──────────────────────→  │   cc_event_bus    │
│                  │  stream.thinking         │                  │
│  (发布方)        │  stream.text             │  (中介/总线)     │
│                  │  stream.tool.start       │                  │
│                  │  stream.tool.end         │                  │
│                  │  llm.request.started     │                  │
│                  │  agent.finished          │                  │
└─────────────────┘                          └────────┬─────────┘
                                                       │ 回调
                                                       ↓
                                              ┌──────────────────┐
                                              │  cc_cli_gateway   │
                                              │                  │
                                              │  (订阅方)        │
                                              │  stream_event_   │
                                              │  handler         │
                                              └──────────────────┘
```

### 11.2 数据结构设计

#### 11.2.1 接口层（Port）

`cc_event_bus_t` 是一个 **不透明类型（opaque type）**，头文件中只声明指针，
具体实现在 `.c` 文件中：

```c
// cc_event_bus.h — 对外接口
typedef struct cc_event_bus cc_event_bus_t;  // 不透明类型，调用方不知道内部

// 回调函数签名
typedef void (*cc_event_handler_fn)(
    const char *event_type,   // 事件的类型标识符
    const char *event_json,   // 事件的负载数据（JSON 字符串，可为 NULL）
    void *user_data           // 订阅时传入的自定义上下文
);

// 对外 API
cc_result_t cc_event_bus_create(cc_event_bus_t **out_bus);
void        cc_event_bus_destroy(cc_event_bus_t *bus);
cc_result_t cc_event_bus_subscribe(cc_event_bus_t *bus, const char *event_type,
                                    cc_event_handler_fn handler, void *user_data);
cc_result_t cc_event_bus_publish(cc_event_bus_t *bus, const char *event_type,
                                  const char *event_json);
```

#### 11.2.2 实现层（Core 实现）

```c
// cc_event_bus.c — 内部实现

#define MAX_HANDLERS 64    // 固定容量，初始化时一次性注册

// ── 单个订阅者条目 ──
typedef struct {
    char *event_type;               // 订阅的事件类型（strdup 拷贝）
    //  NULL = 通配订阅者，接收所有类型的事件
    cc_event_handler_fn handler;    // 回调函数指针
    void *user_data;                // 透传给回调的自定义上下文
} cc_event_handler_entry_t;

// ── 事件总线本身 ──
struct cc_event_bus {
    cc_event_handler_entry_t handlers[64];  // 固定数组，按注册顺序排列
    size_t count;                           // 当前已注册数量 [0, 64)
    cc_mutex_t mutex;                       // 保护 subscribe 和 publish 的并发访问
};
```

**为什么用固定数组而非动态链表？**

| 考量 | 固定数组 | 动态链表 |
|------|---------|---------|
| 内存分配 | 一次 calloc，零碎片 | 每个节点需 malloc |
| 缓存性能 | 连续内存，cache-friendly | 指针跳转，cache miss |
| 遍历速度 | O(n) 线性扫描但内存连续 | O(n) 但每次追指针 |
| 容量上限 | 64 个（对于事件订阅绰绰有余） | 无上限（但不需要） |

订阅者在程序初始化阶段一次性注册完毕（通常 6~20 个），运行时不再增减，
固定数组是更优选择。

### 11.3 生命周期：Create → Subscribe → Publish → Destroy

```
程序启动
  │
  ├── cc_event_bus_create(&bus)
  │     ├── calloc 整个总线结构体（所有槽位清零）
  │     └── cc_mutex_create 创建互斥锁
  │
  ├── CLI Gateway 订阅 6 种流式事件
  │     cc_event_bus_subscribe(bus, "stream.thinking", handler, NULL)
  │     cc_event_bus_subscribe(bus, "stream.text",      handler, NULL)
  │     cc_event_bus_subscribe(bus, "stream.tool.start", handler, NULL)
  │     cc_event_bus_subscribe(bus, "stream.tool.delta", handler, NULL)
  │     cc_event_bus_subscribe(bus, "stream.tool.end",   handler, NULL)
  │     cc_event_bus_subscribe(bus, "stream.finished",   handler, NULL)
  │
  ├── Agent Runtime 在处理过程中发布事件
  │     cc_event_bus_publish(bus, "stream.text", "你好，我是 AI 助手")
  │     cc_event_bus_publish(bus, "stream.tool.start",
  │         "{\"tool_name\":\"file_read\",\"tool_id\":\"call_1\"}")
  │     ...
  │
  └── 程序结束
        cc_event_bus_destroy(bus)
          ├── 遍历释放所有 event_type 字符串
          ├── cc_mutex_destroy 销毁互斥锁
          └── free 总线结构体
```

### 11.4 Subscribe：订阅事件（注册回调）

```c
cc_result_t cc_event_bus_subscribe(
    cc_event_bus_t *bus,
    const char *event_type,        // 可为 NULL（通配订阅）
    cc_event_handler_fn handler,   // 不可为 NULL
    void *user_data                // 透传上下文
)
{
    if (!bus || !handler) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null");

    cc_mutex_lock(bus->mutex);

    // 容量检查
    if (bus->count >= MAX_HANDLERS) {
        cc_mutex_unlock(bus->mutex);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Event bus full");
    }

    // 填充下一个空槽位
    bus->handlers[bus->count].event_type = event_type ? strdup(event_type) : NULL;
    //                                             ^^^^^^^^^^^^^^^^^^^   ^^^^
    //                                             strdup 拷贝字符串      NULL = 通配
    bus->handlers[bus->count].handler    = handler;
    bus->handlers[bus->count].user_data  = user_data;
    bus->count++;

    cc_mutex_unlock(bus->mutex);
    return cc_result_ok();
}
```

**为什么 `event_type` 要 `strdup` 拷贝？**

调用者可能在栈上构造临时字符串：
```c
char buf[64];
snprintf(buf, sizeof(buf), "tool.%s.started", tool_name);
cc_event_bus_subscribe(bus, buf, handler, NULL);
// buf 在函数返回后失效 → 如果把 buf 指针存进去，publish 时 strcmp 访问悬空指针
// strdup 拷贝到堆上 → 总线拥有独立副本，publish 时安全
```

**为什么支持 `event_type = NULL`？**

NULL 表示"通配订阅"——接收所有类型的事件。CLI Gateway 虽然注册了 6 种具体事件，
但也可以用 NULL 实现全局日志：

```c
// 全局日志订阅者 — 记录所有事件到文件
cc_event_bus_subscribe(bus, NULL, global_logger, log_file_handle);
// 不必为每种事件类型单独注册，新增事件类型自动覆盖
```

### 11.5 Publish：发布事件（核心分发逻辑）

这是整个事件总线最核心的函数，包含线程安全的**快照策略（Snapshot Pattern）**：

```c
cc_result_t cc_event_bus_publish(
    cc_event_bus_t *bus,
    const char *event_type,
    const char *event_json
)
{
    if (!bus || !event_type)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null argument");

    // ══════════════════════════════════════════════════════
    // 阶段 1：持锁 — 构建匹配的 handler 快照
    // ══════════════════════════════════════════════════════
    cc_event_handler_entry_t snapshot[MAX_HANDLERS];  // 栈上分配 ~1.5KB
    size_t snapshot_count = 0;

    cc_mutex_lock(bus->mutex);
    for (size_t i = 0; i < bus->count; i++) {
        cc_event_handler_entry_t *entry = &bus->handlers[i];

        // 匹配规则（二选一）：
        if (entry->event_type == NULL ||                         // 通配订阅
            strcmp(entry->event_type, event_type) == 0) {        // 精确匹配

            snapshot[snapshot_count++] = *entry;  // 浅拷贝三个指针字段
        }
    }
    cc_mutex_unlock(bus->mutex);
    // ══════════════════════════════════════════
    // 锁已释放！阶段 2：执行 handler
    // ══════════════════════════════════════════

    for (size_t i = 0; i < snapshot_count; i++) {
        snapshot[i].handler(event_type, event_json, snapshot[i].user_data);
        //       ^^^^^^^^^ 回调中可以安全调用 subscribe/publish
    }
    return cc_result_ok();
}
```

#### 快照策略详解（Snapshot Pattern）

**如果不用快照——全程持锁调用 handler 会怎样？**

```c
// 危险做法：
cc_mutex_lock(bus->mutex);
for (i = 0; i < bus->count; i++) {
    if (match)
        handler(event_type, event_json, user_data);
        // ⚠ handler 内部可能调用 subscribe → 修改 bus->handlers → 遍历器失效
        // ⚠ handler 内部可能调用 publish  → 重入同一把锁，语义依赖锁实现
}
cc_mutex_unlock(bus->mutex);
```

**快照策略的解决方案**：

```
┌─────────────────────────────────────────────────────────────────┐
│                        cc_event_bus_publish                     │
│                                                                 │
│  ┌─ 持锁区间 ──────────────────────────┐                       │
│  │  遍历 handlers[0..count-1]           │                       │
│  │    ├─ 匹配 event_type？              │                       │
│  │    │   ├─ 是 → 浅拷贝到 snapshot[]   │  ← 只读，不执行代码   │
│  │    │   └─ 否 → 跳过                  │                       │
│  │    └─ snapshot_count++              │                       │
│  └──────────────── unlock ─────────────┘                       │
│                                                                 │
│  ┌─ 无锁区间 ──────────────────────────┐                       │
│  │  for each handler in snapshot[]:     │                       │
│  │    handler(event_type, json, data)   │  ← 安全调用回调       │
│  │    ↑ 回调内可以 subscribe/publish    │                       │
│  └─────────────────────────────────────┘                       │
└─────────────────────────────────────────────────────────────────┘
```

**快照策略的代价与收益**：

| 代价 | 收益 |
|------|------|
| 栈上额外数组 `snapshot[64]` ~1.5KB | handler 内可安全调用 subscribe |
| 每个匹配条目浅拷贝 ~24 字节 | handler 内可安全调用 publish（嵌套发布） |
| 快照时的 handler 集合固定为发布瞬间的快照 | 减少锁持有时间 → 更好并发性能 |

### 11.6 端到端全链路：从 LLM 响应到终端渲染

这是事件总线在实际运行时中的完整使用流程。

#### 第一步：流式回调产生事件

当 LLM 以流式方式返回响应时，`cc_agent_runtime.c` 的 `stream_loop_callback` 
被每个 chunk 触发：

```c
// cc_agent_runtime.c — stream_loop_callback
static void stream_loop_callback(const cc_stream_chunk_t *chunk, void *user_data)
{
    stream_loop_ctx_t *ctx = (stream_loop_ctx_t *)user_data;
    cc_event_bus_t *bus = ctx->runtime->event_bus;

    switch (chunk->type) {

    case CC_STREAM_CHUNK_THINKING:
        // 模型的"内心独白"，发布为灰底思考内容
        cc_event_bus_publish(bus, "stream.thinking", chunk->content);
        break;

    case CC_STREAM_CHUNK_TEXT:
        // 用户可见的回复文本增量
        cc_event_bus_publish(bus, "stream.text", chunk->content);
        break;

    case CC_STREAM_CHUNK_TOOL_START:
        // LLM 决定调用工具
        // payload: {"tool_name":"file_read","tool_id":"call_abc123"}
        char payload[512];
        snprintf(payload, sizeof(payload),
            "{\"tool_name\":\"%s\",\"tool_id\":\"%s\"}",
            chunk->tool_name ? chunk->tool_name : "",
            chunk->tool_id   ? chunk->tool_id   : "");
        cc_event_bus_publish(bus, "stream.tool.start", payload);
        break;

    case CC_STREAM_CHUNK_TOOL_DELTA:
        // 工具参数的逐片增量
        cc_event_bus_publish(bus, "stream.tool.delta", chunk->content);
        break;

    case CC_STREAM_CHUNK_TOOL_END:
        // 工具执行完成 — 通知所有订阅者
        // 此时 execute_pending_tool(ctx) 已执行完毕
        break;
    }
}
```

#### 第二步：工具执行完成后发布结果

```c
// cc_agent_runtime.c — execute_pending_tool
static void execute_pending_tool(stream_loop_ctx_t *ctx)
{
    // 1. 查找工具 → 执行 → 获得 cc_tool_result_t
    // 2. 保存到会话存储
    // 3. 发布事件：
    cc_event_bus_t *bus = ctx->runtime->event_bus;
    if (bus) {
        // 构造 JSON payload
        cc_json_value_t *payload = cc_json_create_object();
        cc_json_object_set(payload, "tool_name", cc_json_create_string(tool_name));
        cc_json_object_set(payload, "tool_id",   cc_json_create_string(tool_id));
        cc_json_object_set(payload, "arguments", cc_json_create_string(args));
        cc_json_object_set(payload, "ok",        cc_json_create_bool(result.ok));
        if (result.ok)
            cc_json_object_set(payload, "result", cc_json_create_string(result.content));
        else
            cc_json_object_set(payload, "error",  cc_json_create_string(result.error));

        char *json_str = cc_json_stringify_unformatted(payload);
        cc_event_bus_publish(bus, "stream.tool.end", json_str);
        free(json_str);
        cc_json_destroy(payload);
    }
}
```

#### 第三步：CLI Gateway 订阅并渲染

```c
// cc_cli_gateway.c — 订阅注册（初始化阶段）
static void run_chat_loop(cc_agent_runtime_t *runtime)
{
    cc_event_bus_t *event_bus = cc_agent_runtime_event_bus(runtime);
    if (event_bus) {
        cc_event_bus_subscribe(event_bus, "stream.thinking",   stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, "stream.text",        stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, "stream.tool.start", stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, "stream.tool.delta", stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, "stream.tool.end",   stream_event_handler, NULL);
        cc_event_bus_subscribe(event_bus, "stream.finished",   stream_event_handler, NULL);
    }
    // ... 进入交互循环
}
```

```c
// cc_cli_gateway.c — 统一的 stream 事件回调
static void stream_event_handler(const char *event, const char *payload, void *user_data)
{
    (void)user_data;
    if (!g_stream_mode) return;  // 非流式模式不输出

    if (strcmp(event, "stream.thinking") == 0) {
        // ── 灰色输出思考过程 ──
        if (!g_stream_seen_thinking) {
            printf("\n\033[90m[Thinking]\033[0m\n");  // 灰色标题
            g_stream_seen_thinking = 1;
        }
        printf("\033[90m%s\033[0m", payload ? payload : "");  // 灰色内容

    } else if (strcmp(event, "stream.text") == 0) {
        // ── 青色输出文本回复 ──
        if (!g_stream_seen_text) {
            printf("\033[36m[Agent]\033[0m\n");  // 青色标题
            g_stream_seen_text = 1;
        }
        printf("%s", payload ? payload : "");

    } else if (strcmp(event, "stream.tool.start") == 0) {
        // ── 黄色输出工具调用 ──
        printf("\033[33m[Tool Call]\033[0m");
        if (payload) {
            // 解析 JSON 提取 tool_name
            cc_json_value_t *json = NULL;
            cc_json_parse(payload, &json);
            cc_json_value_t *name = cc_json_object_get(json, "tool_name");
            printf(" %s", cc_json_string_value(name));
            cc_json_destroy(json);
        }
        printf("\n");

    } else if (strcmp(event, "stream.tool.end") == 0) {
        // ── 绿色输出工具结果 ──
        printf("\033[32m[Tool Output]\033[0m\n");
        if (payload) {
            cc_json_value_t *json = NULL;
            cc_json_parse(payload, &json);
            cc_json_value_t *result = cc_json_object_get(json, "result");
            const char *text = cc_json_string_value(result);
            if (text && text[0]) printf("%s", text);
            cc_json_destroy(json);
        }
        printf("\n\033[32m[Tool Done]\033[0m\n");

    } else if (strcmp(event, "stream.finished") == 0) {
        printf("\n");
    }
    fflush(stdout);  // 即时刷新终端
}
```

#### 端到端时序图

```
LLM 服务端                     cc_agent_runtime               cc_event_bus           cc_cli_gateway
    │                              │                            │                      │
    │  SSE: chunk THINKING "嗯..." │                            │                      │
    │─────────────────────────────→│                            │                      │
    │                              │ publish("stream.thinking" │                      │
    │                              │──────────────────────────→│                      │
    │                              │                            │ handler("thinking")  │
    │                              │                            │─────────────────────→│
    │                              │                            │                      │ printf("\033[90m[Thinking]")
    │                              │                            │                      │ printf("嗯...")
    │                              │                            │                      │
    │  SSE: chunk TEXT "你好"      │                            │                      │
    │─────────────────────────────→│                            │                      │
    │                              │ publish("stream.text")     │                      │
    │                              │──────────────────────────→│                      │
    │                              │                            │ handler("text")      │
    │                              │                            │─────────────────────→│
    │                              │                            │                      │ printf("\033[36m[Agent]")
    │                              │                            │                      │ printf("你好")
    │                              │                            │                      │
    │  SSE: chunk TOOL_START       │                            │                      │
    │  tool="file_read"            │                            │                      │
    │─────────────────────────────→│                            │                      │
    │                              │ publish("stream.tool.start"│                      │
    │                              │──────────────────────────→│                      │
    │                              │                            │ handler("tool.start")│
    │                              │                            │─────────────────────→│
    │                              │                            │                      │ printf("\033[33m[Tool Call]")
    │                              │                            │                      │ printf(" file_read")
    │                              │                            │                      │
    │                              │ execute_pending_tool()     │                      │
    │                              │  → 读文件成功               │                      │
    │                              │                            │                      │
    │                              │ publish("stream.tool.end") │                      │
    │                              │──────────────────────────→│                      │
    │                              │                            │ handler("tool.end")  │
    │                              │                            │─────────────────────→│
    │                              │                            │                      │ printf("\033[32m[Tool Output]")
    │                              │                            │                      │ printf("文件内容...")
```

### 11.7 事件总线的事件类型完整清单

| 事件类型 | 发布位置 | payload 格式 | 流向 |
|----------|---------|-------------|------|
| `stream.thinking` | `stream_loop_callback` | 原始思考文本增量 | Runtime → CLI |
| `stream.text` | `stream_loop_callback` | 原始回复文本增量 | Runtime → CLI |
| `stream.tool.start` | `stream_loop_callback` | `{"tool_name":"...","tool_id":"..."}` | Runtime → CLI |
| `stream.tool.delta` | `stream_loop_callback` | 工具参数 JSON 增量 | Runtime → CLI |
| `stream.tool.end` | `execute_pending_tool` | `{"tool_name":"...","result":"..."}` | Runtime → CLI |
| `stream.finished` | `handle_message_stream` 退出点 | `"{}"` | Runtime → CLI |
| `tool.call.started` | `cc_tool_executor` | `{"session_id":"...","tool":"..."}` | Executor → (预留) |
| `tool.call.finished` | `cc_tool_executor` | `{"session_id":"...","tool":"...","ok":bool}` | Executor → (预留) |
| `llm.request.started` | `chat_sync` / `chat_stream` | `{"step":N}` | Runtime → (预留) |
| `llm.response.received` | `chat_sync` | `"{}"` | Runtime → (预留) |
| `agent.finished` | 所有退出点 | `{}` 或 `{"reason":"max_steps_reached"}` | Runtime → (预留) |

### 11.8 线程安全验证：并发发布 + 嵌套事件

`test_event_bus_concurrent.c` 测试了事件总线最极端的场景：

```c
#define THREADS 4
#define LOOPS 200

// ── 事件上下文：共享总线、保护计数器的锁、事件计数器 ──
typedef struct {
    cc_event_bus_t *bus;
    cc_mutex_t mutex;
    int count;
} event_ctx_t;

// ── 事件处理器 ──
static void handler(const char *event_type, const char *event_json, void *user_data)
{
    event_ctx_t *ctx = (event_ctx_t *)user_data;

    // 1. 保护计数器的递增
    cc_mutex_lock(ctx->mutex);
    ctx->count++;
    cc_mutex_unlock(ctx->mutex);

    // 2. 【关键】嵌套发布 —— handler 内部再次调用 publish
    //    安全点：事件总线已经释放自己的 mutex，handler 在锁外执行
    //    所以嵌套 publish 不会重入同一段临界区
    if (event_type && event_type[0] == 'r') {  // "root" 事件
        cc_event_bus_publish(ctx->bus, "nested", "{}");
        //  ↑ 嵌套 publish 会触发 handler 再次被调用
    }
}

// ── 发布者线程 ──
static void *publisher(void *arg) {
    event_ctx_t *ctx = (event_ctx_t *)arg;
    for (int i = 0; i < LOOPS; i++) {
        cc_event_bus_publish(ctx->bus, "root", "{}");
        //  ↑ 每个 "root" 发布 → handler 调用 → handler 内嵌套发布 "nested"
        //    → 每个 "root" 触发 2 次 handler 调用
    }
    return NULL;
}

int main(void) {
    event_ctx_t ctx = {0};
    cc_event_bus_create(&ctx.bus);
    cc_mutex_create(&ctx.mutex);
    cc_event_bus_subscribe(ctx.bus, NULL, handler, &ctx);  // 通配订阅

    // 4 个线程并发发布
    cc_thread_t threads[THREADS];
    for (int i = 0; i < THREADS; i++)
        cc_thread_create(publisher, &ctx, &threads[i]);
    for (int i = 0; i < THREADS; i++)
        cc_thread_join(threads[i]);

    // 验证：THREADS * LOOPS * 2 = 4 * 200 * 2 = 1600
    assert(ctx.count == THREADS * LOOPS * 2);
    // ✓ 并发发布正确 ✓ 嵌套发布不丢事件 ✓ 无死锁
}
```

**这个测试同时验证了三个关键属性**：
1. **并发安全**：4 个线程同时 publish，互斥锁保护 handlers 数组
2. **嵌套发布**：handler 内部 publish "nested" → 快照模式避免重入同一段锁
3. **计数正确**：1600 次 handler 调用无一丢失

### 11.9 为什么事件负载用 JSON 字符串而非结构体？

```c
// 方案 A（本项目）：JSON 字符串
cc_event_bus_publish(bus, "stream.tool.start",
    "{\"tool_name\":\"file_read\",\"tool_id\":\"call_1\"}");

// 方案 B（备选）：类型化结构体
struct tool_start_event { const char *tool_name; const char *tool_id; };
cc_event_bus_publish_typed(bus, TOOL_START, &tool_event);
```

| 考量 | JSON 字符串 | 类型化结构体 |
|------|-----------|------------|
| 新增事件类型 | 不用改事件总线代码 | 需要新增结构体定义 + switch-case |
| 订阅方扩展 | 新订阅方不依赖老事件的结构体定义 | 每个订阅方都依赖所有事件的结构体 |
| payload 为 NULL | 自然支持（语义：空负载信号） | 需要额外约定 |
| 解析开销 | 订阅方按需解析 JSON | 零开销（直接访问字段） |
| 跨语言 | 字符串天然跨语言 | 需要 FFI 绑定 |

对于事件这种"发布者不关心谁订阅"的场景，JSON 字符串的灵活性远胜结构体的性能优势。

### 11.10 设计决策总结

| 决策 | 选择 | 理由 |
|------|------|------|
| 存储结构 | 固定数组 (64) | 订阅者数量确定，无动态扩容需求 |
| 调用模式 | 同步调用 | 简化错误处理，"publish 返回 = 所有 handler 已执行" |
| 线程安全 | mutex + 快照 | 避免 handler 内 subscribe/publish 导致的死锁 |
| 事件类型 | 自由字符串 | 允许运行时动态注册，无需重新编译 |
| 负载格式 | JSON 字符串 | 灵活的结构化数据，不同事件类型格式不同 |
| 通配订阅 | event_type = NULL | 支持日志/监控等横切关注点 |
| 无 unsubscribe | — | 订阅关系在程序生命周期中是静态的 |

### 11.11 实战：如何添加自定义事件

```c
// ── 步骤 1：发布事件（在你的模块中）──
cc_event_bus_t *bus = cc_agent_runtime_event_bus(runtime);
if (bus) {
    // 构造 payload
    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"status\":\"%s\",\"elapsed_ms\":%d}", "ok", elapsed);

    // 发布自定义事件
    cc_event_bus_publish(bus, "my_module.status_changed", payload);
}

// ── 步骤 2：订阅事件（在 CLI gateway 或其他模块中）──
static void my_handler(const char *event, const char *payload, void *data) {
    // 解析 payload JSON
    cc_json_value_t *json = NULL;
    cc_json_parse(payload, &json);
    cc_json_value_t *status = cc_json_object_get(json, "status");
    printf("Status: %s\n", cc_json_string_value(status));
    cc_json_destroy(json);
}

cc_event_bus_subscribe(bus, "my_module.status_changed", my_handler, NULL);
```

---

## 12. 面试高频问答

### Q1: "C 语言中如何实现面向对象编程？"

**答**：使用 **VTable 模式**（虚函数表）。定义两个结构体：
1. **多态句柄**：包含 `void *self`（指向实例数据的指针）和 `vtable`（指向函数指针表）
2. **虚函数表**：包含所有虚函数的函数指针

```c
// 类似于 C++ 的对象模型：
// cc_tool_t  ≈ 对象引用（带有 vptr）
// cc_tool_vtable_t ≈ 类的虚函数表
```

这样上层代码只依赖 vtable 调用，不需要知道具体实现类型。以本项目为例，`cc_tool_t` 统一了
文件读写、Shell、HTTP 请求等所有工具的接口。

### Q2: "C 语言的 VTable 和 C++ 虚函数有什么区别？"

| 方面 | C VTable | C++ 虚函数 |
|------|---------|-----------|
| vptr 存储 | 显式在 struct 中 | 编译器隐式嵌入 |
| vtable 生成 | 手动定义静态常量 | 编译器自动生成 |
| self/this | 显式传参 `void *self` | 隐式 `this` 指针 |
| 内存模型 | transparent，完全可控 | ABI 依赖编译器 |
| 性能 | 完全等价（一次指针解引用） | 完全等价 |
| 优点 | 无 RTTI 开销，可跨 FFI | 语法糖，IDE 友好 |
| 缺点 | 手写代码多 | 二进制膨胀（RTTI、异常表） |

### Q3: "你在这个项目中用了哪些设计模式？"

**答**：项目核心使用了 7 种设计模式：

1. **VTable 多态**（手写虚函数表）— 所有端口层接口
2. **工厂方法** — `cc_storage_factory_create_store()` 按配置选后端
3. **构建器模式** — `cc_runtime_builder` 装配 10+ 组件
4. **注册表模式** — `cc_tool_registry` 工具的中央索引
5. **观察者模式** — `cc_event_bus` 发布-订阅流式输出
6. **策略模式** — `cc_policy_engine` 可替换的安全检查
7. **代理模式 / 编排器模式** — `cc_agent_manager` 多 Agent 调度与生命周期管理

其中构建器模式最值得展开——它解决了"创建一个依赖 10+ 个组件的复杂对象"的问题：
```c
// 单步构建，builder 内部按正确顺序创建和注入
cc_runtime_builder_create(&config, features, &builder);

// 获取 product
cc_agent_runtime_t *rt = cc_runtime_builder_runtime(builder);

// 级联销毁所有子组件
cc_runtime_builder_destroy(builder);
```

### Q4: "什么是端口-适配器架构？它的好处是什么？"

**答**：又称六边形架构（Hexagonal Architecture），核心思想是 **业务逻辑不依赖外部系统**。

在本项目中：
- **端口（Ports）** 是 `cc_llm_provider_t`、`cc_session_store_t` 等抽象接口
- **适配器（Adapters）** 是 OpenAI、SQLite、Docker 等具体实现

好处：
1. **可测试性**：注入 mock vtable 即可测试业务逻辑（无需真实 LLM/数据库）
2. **可迁移性**：同一个 core 可在 Linux/Windows/ESP32 运行（替换平台适配器即可）
3. **可裁剪性**：通过编译宏在生产/开发/嵌入式三套 profile 间切换能力
4. **开闭原则**：新增 LLM 后端只需实现 `cc_llm_protocol_vtable_t`，无需修改核心代码

### Q5: "C 语言中如何保证线程安全？"

**答**：本项目通过 **平台抽象 + 四种同步策略** 实现线程安全：

**1. 平台抽象**：用 `cc_mutex_t` / `cc_thread_t` / `cc_cond_t` 不透明类型封装
POSIX pthread、Windows CRITICAL_SECTION/CONDITION_VARIABLE 和 ESP32 FreeRTOS 信号量。
POSIX 端显式使用递归锁，Windows 的 `CRITICAL_SECTION` 也支持递归；ESP32 当前用普通
FreeRTOS mutex，因此上层设计不能依赖递归锁语义。条件变量用于 run queue、tool pool 的
阻塞等待，`cc_cond_timedwait` 配合协作式取消令牌，每隔几十毫秒醒来查询 cancel token。

**2. 四种同步策略**：

| 策略 | 数据结构 | 核心技巧 |
|------|---------|---------|
| Freeze 模式 | `cc_tool_registry` | 单向冻结后变只读，find 返回浅拷贝 |
| 快照模式 | `cc_event_bus` | 锁内浅拷贝 handler，锁外执行回调 |
| 全程持锁 | `cc_logger` | 从级别判断到 fflush 全程持锁 |
| 标准互斥 | 所有存储后端 | lock → 读写 → unlock |

**3. 并发测试**：POSIX CLI 当前 27 项 CTest 全部通过，覆盖互斥锁基础 (8线程/8万次)、freeze 并发读、事件嵌套、日志并发、存储并发、Runtime 多 session，以及 run queue、cancel token、tool pool、SSE parser、skill catalog、MCP runtime、plugin protocol 等专项单测。

```c
// freeze 模式示例
cc_tool_registry_freeze(reg);
// 之后多个线程可以安全地调用：
cc_tool_registry_find(reg, "file_read", &tool);  // 只读
cc_tool_registry_build_schema_json(reg, &json);   // 只读
// 但不能再 cc_tool_registry_add —— 结构已冻结
```

**亮点**：项目**没有使用 C11 `_Atomic`、CAS 或读写锁**，全部用 `cc_mutex_t` 实现，通过策略模式在不同场景选择最优的锁策略。事件总线的快照模式是最精妙的设计——锁内只做浅拷贝，锁外执行 handler，避免了 handler 回调中重入 publish/subscribe 导致的死锁。

### Q6: "Go 和 Rust 的错误处理对 C 有什么启发？"

**答**：本项目借鉴了 Go 的显式错误传播和 Rust 的 Result 类型：

```c
typedef struct cc_result {
    cc_error_code_t code;   // 0 = Ok, 非 0 = Err
    char *message;          // 错误描述（堆分配）
} cc_result_t;

// 使用模式类似 Go：
cc_result_t rc = some_func();
if (rc.code != CC_OK) {
    // 传播或包装错误
    return rc;
}
```

对比 C 传统的 errno：
- errno 是全局/线程局部变量 → `cc_result_t` 是值类型，纯函数式
- errno 只能传整型 → `cc_result_t` 可携带格式化字符串
- errno 容易被忽略 → `cc_result_t` 返回类型强迫调用方显式处理

### Q7: "如何管理 C 项目中的内存安全？"

**答**：通过严格的所有权规则：

1. **谁创建谁销毁** — 每个 create 函数都有对应的 destroy
2. **值语义浅拷贝** — `cc_tool_t` 等按值传递时只拷贝指针，所有权不转移
3. **builder 级联清理** — `cc_runtime_builder_destroy()` 自动销毁所有子组件
4. **文档化所有权** — 每个接口的注释明确标注所有权归属

```c
/**
 * @param out_registry  输出：指向新注册表的指针（调用者负责 cc_tool_registry_destroy）
 *                        ^^^^^^^^^^^^^^^^  所有权转移给调用者
 */
cc_result_t cc_tool_registry_create(cc_tool_registry_t **out_registry);
```

### Q8: "事件总线为什么要用快照模式（Snapshot Pattern）？"

**答**：快照模式解决的核心问题是 **handler 回调中的重入**。

如果全程持锁调用 handler：
```c
// 危险做法：
cc_mutex_lock(bus->mutex);
for (i = 0; i < count; i++) {
    handler(event_type, json, data);
    // ⚠ handler 内部可能调用 subscribe → 修改 handlers 数组 → 遍历器失效
    // ⚠ handler 内部可能调用 publish  → 重入同一把锁，行为依赖锁实现
}
cc_mutex_unlock(bus->mutex);
```

快照模式的做法：
```c
// 安全做法：两阶段
// 阶段 1：持锁浅拷贝匹配的 handler 到栈上快照数组
cc_mutex_lock(bus->mutex);
for (i = 0; i < count; i++) {
    if (match) snapshot[snapshot_count++] = handlers[i];  // 只浅拷贝，不执行
}
cc_mutex_unlock(bus->mutex);

// 阶段 2：释放锁后执行 handler
for (i = 0; i < snapshot_count; i++) {
    snapshot[i].handler(event_type, json, data);
    // ✓ handler 内可安全调用 subscribe/publish
}
```

**收益**：
- 避免死锁（handler 内 publish 无需锁）
- 避免迭代器失效（handler 内 subscribe 不修改快照）
- 减少锁持有时间（handler 执行不占锁）

**代价**：
- 栈上额外 ~1.5KB（snapshot[64]）
- 发布期间新增的 handler 不会在当前 publish 中生效（快照一致性）

### Q9: "如果要新增一个工具，你会改哪些地方？"

**答**：分三步：

1. 在合适层级实现 `cc_tool_vtable_t`。通用文件/HTTP/记忆工具放在 `cclaw/adapters/src/tools/common`，
   桌面专属工具放在 `apps/<platform>/cli/src/tools`，板级硬件工具放在对应 board 目录。
2. 写一个工厂函数，创建 `self` 私有数据并填充 `cc_tool_t { self, vtable }`。
3. 在应用或板级的 `cc_runtime_feature_set_t` 工具 descriptor 表里注册名称、别名、编译开关和工厂函数。

面试时强调：不要在 Runtime 里写 `if tool_name == ...` 的分支。Runtime 只认注册表和 vtable，
新增工具应该只影响工具实现和 feature descriptor。

### Q10: "如果要新增一个 LLM 后端，你会怎么做？"

**答**：优先复用 `cc_http_llm_provider`，只新增协议策略：

1. 实现 `cc_llm_protocol_vtable_t`：`build_request`、`parse_response`、`parse_stream_event`、`destroy`。
2. 用 `cc_http_llm_provider_create(base_url, api_key, model, protocol, out_provider)` 包成统一的 `cc_llm_provider_t`。
3. 在 `cc_posix_cli_features.c` / `cc_windows_cli_features.c` 或板级 feature 文件里加入 provider descriptor。
4. 增加配置项和最小测试，至少覆盖普通文本、工具调用、错误响应和流式结束事件。

这个回答能体现两层 vtable 的价值：HTTP 传输层不变，API 格式差异收敛在 protocol 层。

### Q11: "这个项目有什么不足？"

**答**：可以讲三个诚实但不致命的点：

1. 事件总线是同步分发，没有异步队列和 handler 隔离；handler 崩溃或阻塞会影响发布线程。
2. 工具注册表和事件总线都使用固定容量数组（MAX_TOOLS=64, MAX_HANDLERS=64），简单稳定，嵌入式场景充分，但桌面端大量工具时可能需要改为动态扩容。
3. 存储工厂里 SQLite 失败会降级到 JSON，这提高可用性，但生产环境要配合日志/告警，否则可能掩盖数据库故障。
4. 事件总线目前不支持 `unsubscribe`，订阅关系假设为初始化阶段注册后运行期静态；如果需要运行时动态订阅/退订，需要扩展接口。

更进一步可以补一句：如果继续优化，我会优先完善事件/工具的观测指标、补齐流式降级路径测试。
头文件注释与实现不一致的问题已在当前版本修正完毕（见 docs/architecture.md 和 ports/ 头文件注释）。

### Q12: "如何证明这个项目不是玩具 Demo？"

**答**：不要只说"功能很多"，要说工程证据：

- 有明确分层：`core/ports/adapters/platforms/apps`，核心逻辑不直接依赖 curl、pthread、Win32 或 ESP-IDF。
- 有可裁剪 profile：桌面、Windows、ESP32、core-minimal 通过 CMake 开关控制功能进入二进制。
- 有生命周期管理：Runtime Builder 统一创建和销毁 logger、event bus、store、llm、policy、sandbox、registry。
- 有并发测试：mutex、registry freeze、event bus nested publish、logger、memory store、runtime sessions、run queue async interrupt、cancel token、tool pool lane 都有压力测试。
- 有安全边界：文件工具限制 workspace，工具执行前经过 policy engine，shell 走 sandbox/审批。
- 有多 Agent 编排：`cc_agent_manager` 支持 submit/collect/interrupt，通过 run queue + cancel token 实现 agent 间并发调度和协作式取消。
- 有条件变量：`cc_cond_t` + `cc_cond_timedwait` 配合 cancel token，在 run queue、tool pool 等场景实现阻塞等待而非轮询，尤其对 ESP32 省电有意义。
- 有工具热重载：`cc_tool_registry_snapshot` 通过 generation + ref_count 支持工具集热更新，reload 失败时旧 registry 继续服务。

---

## 13. 简历指标取舍：优先讲工程价值，数字只作证据

这个项目不需要强行堆性能量化。C-Claw 的价值主要是 **纯 C 工程能力、跨平台裁剪、
接口抽象、并发安全和可测试性**，不是传统高 QPS 后端服务。简历正文应优先讲清楚
架构价值和工程边界；数字只作为证据，能稳稳解释再写，不能解释就放到面试备查材料里。

### 13.1 简历正文优先写什么

优先写这些工程事实，而不是性能承诺：

| 指标 | 推荐写法 | 价值 |
|------|----------|------|
| LLM 后端 | 支持 OpenAI-compatible / Ollama / Anthropic 3 类 LLM 后端 | 体现协议抽象能力 |
| 存储后端 | 支持 SQLite / JSON 文件 / Memory 3 类会话或记忆存储 | 体现端口-适配器设计 |
| 构建 Profile | 支持 POSIX CLI、Windows CLI、core-minimal、ESP32-S3 QEMU、STM32H743 Renode 5 类构建目标 | 体现跨平台和裁剪 |
| 自动化测试 | POSIX CLI 当前 27 项 CTest 全部通过 | 体现工程质量 |
| 嵌入式验收 | ESP32-S3 QEMU 和 STM32H743 Renode 有 smoke 验收路径 | 体现不是纯桌面 demo |
| 插件协议 | JSON-RPC 2.0 over stdio，插件可用任意语言实现 | 体现扩展性 |

简历里推荐组合成一句：

> 基于纯 C 实现 AI Agent Runtime，采用 Ports & Adapters 架构解耦 LLM、工具、存储和平台能力，支持 3 类 LLM 后端、3 类存储后端、JSON-RPC 插件工具系统，并通过 5 类 CMake Profile 覆盖 POSIX、Windows、ESP32-S3 QEMU、STM32H743 Renode 等构建目标；编写 27 项 CTest 覆盖并发队列、插件协议、工具执行和配置加载等核心路径。

如果简历空间很紧，可以不写任何性能数字，只保留"多后端、多 profile、测试覆盖"这些工程指标。
这些数字不是为了显得更大，而是为了证明项目不是口头设计。

### 13.2 不建议直接放进简历的性能指标

下面这些指标更适合做面试备查材料。只有当岗位明显关注嵌入式资源占用、系统性能或工具链优化，
并且你已经实测过，才考虑写进简历：

这些指标很有含金量，但必须先保留命令输出或日志，最好记录机器环境：

| 待测指标 | 为什么值得测 | 可写到简历的形式 |
|----------|--------------|------------------|
| core-minimal 二进制/静态库体积 | 证明构建裁剪有效 | 最小核心构建产物约 xx KB/MB |
| POSIX CLI 二进制体积 | 证明 C runtime 的部署成本 | 桌面 CLI 单文件产物约 xx MB |
| 插件 stdio 调用延迟 | 证明 JSON-RPC 插件机制开销可控 | 本地插件调用平均 xx ms |
| run queue 并发调度耗时 | 证明多 session 调度能力 | N 个 session 并发提交，平均调度耗时 xx ms |
| 冷启动耗时 | 证明 CLI 工具可用性 | CLI 冷启动到 help 输出约 xx ms |
| ESP32/STM32 固件大小 | 嵌入式方向很加分 | ESP32 QEMU 固件约 xx KiB，占 factory 分区 xx% |

如果没有基准版本，不要写"提升 xx%"。可以写"控制在 xx"、"平均 xx"、"覆盖 xx"，
这些是单点测量，解释成本低。

### 13.3 明确不要写的指标

- 不写"高并发"：除非做过明确压测并能说明并发模型、机器配置和瓶颈。
- 不写"性能提升 xx%"：除非有优化前后的 commit、命令和日志。
- 不把代码行数作为主指标：代码量可以口头说明规模，但简历上优先写测试、构建目标、后端数量。
- 不把 Windows CLI 说成已完整验证：当前可以说"源码和 CMake 结构已对齐，需 Windows 环境最终验收"。

### 13.4 简历版本模板

**三行版本**：

> C-Claw｜纯 C AI Agent Runtime  
> 基于 Ports & Adapters 架构开发纯 C Agent 运行时，用 `struct + vtable + void *self` 实现接口抽象、依赖注入和跨平台适配，核心支持 ReAct 工具调用循环、流式输出、上下文管理和长期记忆。  
> 接入 OpenAI-compatible / Ollama / Anthropic 3 类 LLM 后端，支持 SQLite / JSON / Memory 3 类存储，以及文件、Shell、HTTP、Memory、JSON-RPC 插件工具系统。  
> 通过 CMake Profile 支持 POSIX、Windows、ESP32-S3 QEMU、STM32H743 Renode 等多平台裁剪构建，编写 27 项 CTest 覆盖并发调度、插件协议、工具执行和配置加载等关键路径。

**偏系统开发版本**：

> 设计纯 C Agent Runtime 的端口抽象层，将 LLM Provider、Tool、Session Store、Memory Store、Policy Engine、Sandbox、Event Bus 等能力统一为 vtable 接口，并通过 Runtime Builder 进行依赖注入和生命周期托管，降低核心层对 curl、pthread、Win32、ESP-IDF 等平台能力的耦合。

**偏嵌入式/跨平台版本**：

> 通过 CMake Profile 和平台端口实现同一套 Agent Core 在 POSIX CLI、Windows CLI、ESP32-S3 QEMU、STM32H743 Renode 和 core-minimal 之间裁剪复用；设备端只保留必要文件系统、线程、HTTP、UART/FreeRTOS 适配，桌面能力不进入核心层。

---

## 14. 面试准备路线：从项目介绍到源码深挖

### 14.1 15 秒版本

> C-Claw 是我用纯 C 实现的 AI Agent Runtime。它的目标是让同一套 Agent Core 可以在桌面、服务器和嵌入式设备上复用，所以我用 Ports & Adapters 把 LLM、工具、存储、平台能力解耦，并用 C 的 vtable 模式实现多态和依赖注入。

### 14.2 60 秒版本

> 这个项目的核心链路是 ReAct Agent 循环：用户输入先写入 session store，context builder 根据历史消息、长期记忆和工具 schema 组装 LLM messages；LLM 如果返回工具调用，tool executor 会从 registry 查找工具，经过 policy engine 检查后执行，把 tool result 写回 session，再进入下一轮 LLM；如果没有工具调用，就输出最终文本。  
> 架构上，core 层只依赖 ports 里的接口，不直接依赖 OpenAI、SQLite、curl 或具体平台。OpenAI/Ollama/Anthropic、SQLite/JSON、POSIX/Windows/ESP32 都作为 adapter 注入。项目还实现了 run queue、cancel token、tool pool、event bus 和 JSON-RPC 插件系统，并用 27 项 CTest 覆盖关键并发和协议路径。

### 14.3 3 分钟深挖版本

面试官让你"详细讲讲项目"时，可以按下面顺序讲：

1. **背景**：为什么做纯 C Agent Runtime。重点讲可移植、可裁剪、资源受限设备，而不是跟 Python 框架比生态。
2. **主链路**：用户输入、session store、context builder、LLM provider、tool executor、tool result、下一轮 LLM。
3. **架构边界**：`core/ports/adapters/platforms/apps` 各自负责什么，为什么 core 不能直接引用平台实现。
4. **C 多态**：用 `struct + vtable + void *self`，值类型浅拷贝，`destroy(self)` 负责释放具体实现。
5. **并发设计**：同 session 串行、跨 lane 并发、cancel token 协作式取消、事件总线快照分发。
6. **工程证据**：5 类 profile、3 类 LLM、3 类存储、JSON-RPC 插件、27 项 CTest。
7. **不足和后续**：观测指标、benchmark、Windows 真机验收、事件总线异步化。

### 14.4 STAR 故事模板

**故事 1：从耦合实现重构到 Ports & Adapters**

| STAR | 内容 |
|------|------|
| Situation | 早期桌面能力、LLM 接入、存储和平台代码容易混在核心逻辑里 |
| Task | 让同一套 Agent Core 能在 POSIX、Windows、ESP32、STM32 profile 下复用 |
| Action | 抽象 `cc_llm_provider_t`、`cc_tool_t`、`cc_session_store_t`、`cc_http_client_t` 等端口，用 Runtime Builder 注入具体 adapter，用 CMake Profile 控制能力裁剪 |
| Result | core 层不直接依赖 curl、pthread、Win32 或 ESP-IDF；新增 LLM 后端或平台主要改 adapter/feature descriptor，不改 Agent 主循环 |

**故事 2：事件总线为什么用快照模式**

| STAR | 内容 |
|------|------|
| Situation | Runtime、工具执行、流式输出、CLI 渲染都需要发布事件，handler 内可能重入 publish/subscribe |
| Task | 保证并发发布和嵌套发布时不死锁、不长时间持锁、不破坏 handler 数组遍历 |
| Action | publish 时锁内筛选并浅拷贝 handler 快照，释放锁后逐个调用回调 |
| Result | handler 可以安全重入发布事件，锁持有时间短；代价是当前 publish 看不到发布中新增的订阅者 |

**故事 3：工具调用为什么放到 registry + policy + executor**

| STAR | 内容 |
|------|------|
| Situation | Agent 工具数量会增长，包含文件、Shell、HTTP、Memory、插件等不同风险等级能力 |
| Task | 避免 Runtime 里写大量 `if tool_name == ...`，同时给危险工具加策略边界 |
| Action | 每个工具实现 `cc_tool_vtable_t`，统一注册到 registry；执行前由 tool executor 查找工具、构造上下文、调用 policy engine，再执行工具并发布事件 |
| Result | 新增工具只影响工具实现和 feature descriptor，Runtime 主循环保持稳定；Shell/文件等能力可以被策略层统一控制 |

### 14.5 面试追问清单

面试前至少把下面问题各准备 1 分钟答案。回答时不要背概念，最好都落回本项目里的模块名、
设计取舍和代价。

**Q1：为什么用 C 而不是 Python / Rust / Go？**

**答**：这个项目的目标不是快速做一个应用层 Agent，而是探索 Agent Runtime 在桌面和
嵌入式/资源受限环境之间如何复用。C 的优势是 ABI 简单、运行时依赖少、容易做平台裁剪，
也更贴近 ESP32、STM32、FreeRTOS 这类环境。  
如果用 Python，工具生态会更快，但很难自然下沉到嵌入式 profile；如果用 Rust，内存安全更强，
但 FFI、工具链和板级适配成本更高。这里选择 C，是为了把重点放在 runtime 边界、生命周期、
接口抽象和平台移植上。

**追问补充**：不是说 C 比 Rust/Go 更适合所有场景，而是这个项目的训练目标和部署假设更偏
系统层、可裁剪和跨平台端口。

**Q2：C 里怎么实现多态？**

**答**：项目统一使用 `struct + vtable + void *self`。对外暴露的是一个值类型句柄，比如
`cc_tool_t` 或 `cc_llm_provider_t`，里面有 `self` 指向具体实现的私有数据，`vtable`
指向一组函数指针。调用方只通过 vtable 调 `chat`、`call`、`destroy` 等接口，不知道背后是
OpenAI、Ollama、SQLite、JSON store 还是插件工具。

```c
typedef struct cc_tool {
    void *self;
    const cc_tool_vtable_t *vtable;
} cc_tool_t;
```

**追问补充**：它和 C++ 虚函数类似，但 C++ 的 vtable 由编译器生成，C-Claw 是手写函数指针表；
`this` 指针也不是隐式的，而是显式传入 `void *self`。

**Q3：如何避免内存泄漏？**

**答**：核心原则是所有权明确和生命周期集中管理。项目里多数对象遵循 `create -> use -> destroy`，
谁创建谁释放；`cc_tool_t`、`cc_llm_provider_t` 这类句柄可以浅拷贝，但浅拷贝不转移 `self`
所有权；复杂组件由 `cc_runtime_builder` 统一装配，失败路径和销毁路径做级联清理。错误处理也用
`cc_result_t` 显式返回，避免错误路径漏掉释放。

**追问补充**：C 没有 RAII，所以最容易出问题的是中途失败路径。这个项目的做法是把复杂依赖创建
集中到 builder/factory，减少到处手写清理逻辑。

**Q4：工具调用失败怎么办？**

**答**：工具失败不应该直接让进程退出。Agent 的语义里，工具结果也是 observation 的一部分。
所以 `cc_tool_executor_execute()` 可以返回一个工具失败结果，例如 `ok=0` 和错误文本，
然后把这个 tool result 写回 session store。下一轮 LLM 能看到失败原因，决定修正参数、换工具，
或者向用户解释失败。

**追问补充**：只有基础 runtime 创建失败、配置 JSON 严重错误、内存分配失败这类系统级问题才应该
阻止启动；单个工具找不到或执行失败属于 Agent 循环里的可恢复错误。

**Q5：同一个 session 为什么要串行？**

**答**：同一个 session 的消息历史有严格顺序。如果两个请求同时写同一个 session，可能出现上下文
交叉、工具结果对应错 assistant tool call、最终回答基于不一致历史的问题。所以 run queue 的策略是
同 session 串行，保证消息顺序；不同 session 可以进入不同 lane 并发执行，提高吞吐。

**追问补充**：这不是简单限制并发，而是在一致性和并发之间做边界划分：session 内保证顺序，
session 间释放并发。

**Q6：cancel token 是什么？**

**答**：`cc_cancel_token_t` 是协作式取消边界。它不强杀线程，而是让 run queue、tool pool、
插件 worker、MCP transport、LLM stream 等在安全点检查“是否已取消”。等待条件变量时也会用
timed wait 周期性醒来检查 token，这样既不会忙等，也能及时响应 interrupt。

**追问补充**：强杀线程在 C 里很危险，容易留下锁、内存、pipe、socket 等资源处于不一致状态。
协作式取消更保守，但更容易保证清理路径正确。

**Q7：新增一个 LLM 后端要改哪里？**

**答**：优先复用 HTTP LLM provider，只新增协议策略。HTTP 层负责请求生命周期、headers/body
发送、状态码处理、stream 分帧；具体后端只实现 URL、headers、JSON body、普通响应解析和流式事件解析。
然后在 feature descriptor 里注册 provider，并补配置项和测试。

**追问补充**：这就是两层 vtable 的价值。新增后端不应该复制一份 HTTP transport，也不应该改
Agent Runtime 主循环。

**Q8：JSON-RPC 插件有什么风险？**

**答**：主要风险有四类：第一是进程生命周期，插件启动失败或退出不能拖垮 runtime；第二是超时和取消，
请求不能无限等；第三是响应串线，stdio 上多个请求必须靠 JSON-RPC id 匹配；第四是 schema 和错误处理，
插件返回非法 JSON 或错误响应时要转成工具错误。项目里取消读取后会复位 worker，避免旧响应污染后续请求。

**追问补充**：JSON-RPC over stdio 的优点是语言无关、实现简单、隔离性比动态库好；代价是序列化、
进程通信和生命周期管理成本更高。

**Q9：事件总线同步分发有什么问题？**

**答**：同步分发的主要问题是 handler 在发布线程里执行，如果 handler 很慢、阻塞或崩溃，会影响
发布方。当前项目用快照模式解决的是锁边界和重入问题：锁内只复制 handler 快照，锁外回调，避免
handler 内 publish/subscribe 导致死锁或遍历失效。但它没有解决 handler 隔离和异步调度问题。

**追问补充**：如果继续演进，可以加异步事件队列、worker 线程、handler 超时和背压策略。
当前同步模型更简单，适合 runtime 内部诊断和 CLI 渲染。

**Q10：如何证明这个项目不是 demo？**

**答**：我会从工程证据证明，而不是只说功能多。第一，分层清楚，`core/ports/adapters/platforms/apps`
各有边界，core 不直接依赖 curl、pthread、Win32 或 ESP-IDF；第二，有 5 类 CMake Profile，
能做桌面、core-minimal 和板级裁剪；第三，有 OpenAI-compatible、Ollama、Anthropic 3 类 LLM 后端，
SQLite、JSON、Memory 3 类存储；第四，有 JSON-RPC 插件、run queue、cancel token、tool pool、
event bus 等 runtime 组件；第五，POSIX CLI 当前 27 项 CTest 全部通过，覆盖并发、协议、工具执行和配置路径。

**追问补充**：我不会把未完整验收的环境说满，比如 Windows CLI 可以说结构已对齐，但仍需要 Windows
真机或交叉工具链做最终编译验证。

**Q11：这个项目最大的技术难点是什么？**

**答**：最大的难点不是某一个 API，而是如何在 C 里保持复杂 runtime 的边界清晰。Agent Runtime
会同时碰到 LLM、工具、存储、插件、并发、平台适配和生命周期管理，如果没有端口抽象，很快会变成
到处都是 `if provider == ...` 和平台条件编译。这个项目把变化点都收敛到 vtable、factory、
feature descriptor 和 CMake profile 里，让主循环保持稳定。

**追问补充**：面试时可以接着讲 `cc_runtime_builder`，它是依赖注入和生命周期管理的关键入口。

**Q12：如果让你继续优化，下一步做什么？**

**答**：我会优先做三件事。第一，补 benchmark 和观测指标，比如插件调用延迟、core-minimal 体积、
run queue 调度耗时，让项目有更清楚的工程数据。第二，完善 Windows 真机和更多板级 profile 的验收。
第三，把事件总线和插件 worker 的监控做得更完整，比如超时统计、错误分类和异步事件队列。

**追问补充**：这里不要说“重写成某某语言”。更好的回答是沿着现有架构补验证、补观测、补边界能力。

### 14.6 现场演示顺序

如果面试允许展示终端，推荐用 3 分钟演示：

```bash
cmake --preset posix-cli
cmake --build --preset posix-cli
ctest --test-dir build/app/posix/cli --output-on-failure
./build/app/posix/cli/bin/c-claw --help
```

演示时不要边跑边解释所有源码。建议只说三件事：

1. profile 决定构建入口和能力裁剪。
2. 测试覆盖并发队列、插件协议、工具执行、配置加载等核心路径。
3. CLI 是一个 app gateway，真正的 Agent Runtime 在 core 层。

### 14.7 工具数据交互与跨线程模型

面试官如果问"工具的数据是怎么传的"或"工具是不是跨线程执行"，不要只回答"通过函数调用"。
更完整的回答应该分成 **数据流、调用接口、并发边界、插件/MCP 特例** 四层。

**1. 工具调用的数据流**

工具调用发生在 ReAct 循环中。LLM 不直接执行工具，而是返回一个结构化的 tool call：

```text
用户输入
  ↓
Agent Runtime 构建 messages + tool schema
  ↓
LLM 返回 tool_call: { id, name, arguments_json }
  ↓
cc_tool_executor_execute()
  ↓
tool_registry 根据 name 查找 cc_tool_t
  ↓
policy_engine 做安全检查和人工审批
  ↓
tool.vtable->call(...)
  ↓
工具填充 cc_tool_result_t
  ↓
runtime 写入 session store，形成 role="tool" 的消息
  ↓
下一轮 LLM 读取 tool result，继续推理或输出最终答案
```

这个设计的关键点是：工具结果不是临时变量里用完就丢，而是写回 session store。
下一轮构建上下文时，`cc_context_builder` 会把 assistant 的 tool call 和 tool 的 result
都还原成 LLM API 需要的 messages。这样 LLM 能看到 observation，符合 ReAct 模式。

**2. 工具统一调用接口**

所有工具都实现同一个 vtable 接口：

```c
cc_result_t (*call)(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result
);
```

这四个参数分别承担不同职责：

| 参数 | 含义 | 所有权 |
|------|------|--------|
| `self` | 工具自己的私有数据，例如插件 worker、文件工具配置、HTTP client | 工具实现拥有，调用方只借用 |
| `args_json` | LLM 生成的工具参数 JSON 字符串 | 调用方借给工具，工具不能释放 |
| `ctx` | 本次调用上下文：session、workspace、runtime services、cancel token、timeout、lane | 调用方临时提供，工具只借用 |
| `out_result` | 工具输出结果 | 工具填充，调用方负责后续销毁字段 |

成功时工具通常填：

```c
out_result->ok = 1;
out_result->content = strdup("{\"content\":\"...\"}");
```

失败时工具通常填：

```c
out_result->ok = 0;
out_result->error = strdup("File not found");
```

注意：`cc_result_t` 表示"工具调用函数本身有没有系统级异常"，`cc_tool_result_t.ok`
表示"这次工具业务执行是否成功"。例如文件不存在、参数错误、policy 拒绝这类情况通常会变成
`out_result->ok = 0`，然后作为 observation 写回给 LLM，而不是直接中断整个 Agent。

**3. `cc_tool_context_t` 传了哪些运行时能力**

工具不会拿到完整 `cc_agent_runtime_t`，只拿到受限上下文：

```c
typedef struct cc_tool_context {
    const char *session_id;
    const char *workspace_dir;
    const char *user_id;
    const cc_runtime_services_t *services;
    cc_cancel_token_t *cancel_token;
    int timeout_ms;
    const char *lane_name;
    unsigned long generation;
} cc_tool_context_t;
```

这么做有两个好处：

1. **边界更安全**：工具只能访问被允许的服务，比如 event bus、logger、memory store、
   tool pool、审批回调，不能随意改 runtime 内部状态。
2. **更容易裁剪**：不同 profile 可以不给某些 service，例如嵌入式 profile 可以禁用 shell、
   plugin、SQLite 等能力，工具只需要判断对应服务是否为 NULL。

面试时可以强调：`ctx` 是一次调用的"环境包"，不是长期持有对象。工具不应该把 `ctx` 指针保存到
异步任务里长期使用；如果确实要异步执行，需要深拷贝必要字段并明确生命周期。

**4. 单次工具调用本身是同步函数调用**

在 `cc_tool_executor_execute_with_options()` 中，真正执行工具的是一行 vtable 调用：

```c
rc = tool.vtable->call(
    tool.self,
    call->arguments_json,
    &ctx,
    out_result
);
```

这意味着：**单次工具调用本身不是自动开线程的**。executor 调用工具时，是在当前执行线程里同步等待
工具返回。工具返回后，runtime 才会释放 tool pool ticket、发布 finished 事件、把结果写回 session。

所以对"是否跨线程"的准确回答是：

> 工具调用本身是同步函数调用；跨线程并发发生在外层 run queue 或具体 adapter 内部。

比如某个 Agent run 被 run queue 放到 worker 线程执行，那么这个 run 里面的工具调用就在那个 worker
线程里同步执行。工具 executor 不会为每次 tool call 额外创建一个线程。

**5. Tool Executor Pool 不是线程池，而是并发闸门**

`cc_tool_executor_pool` 很容易被误解成线程池，但它实际上只负责 lane 级准入控制和 timeout 策略。

这里的 lane 可以理解成"工具执行车道"或"工具分类队列"。**lane 级并发闸门**的意思是：
按工具类别划分不同 lane，每个 lane 设置最大并发数；工具执行前必须先拿到这个 lane 的名额，
拿不到就等待，执行完再释放名额。

执行前：

```text
build_tool_lane_name(call->name)
  ↓
cc_tool_executor_pool_acquire_with_cancel(pool, lane_name, cancel_token, &ticket)
  ↓
tool.vtable->call(...)
  ↓
cc_tool_executor_pool_release(pool, ticket)
```

lane 名称大致这样映射：

```text
普通工具              → tool.<name>
plugin.<id>.<tool>    → plugin.<id>
mcp.<server>.<tool>   → mcp.<server>
```

这样配置可以限制一类工具的并发。例如 shell 工具可以限制为 1，HTTP 工具可以允许多个并发，
插件工具可以按 plugin 维度限流。

举个例子，如果 `tool.shell_run` 的并发上限是 1：

```text
第 1 个 shell_run 进来：
  in_flight = 0 < 1
  允许执行，in_flight++

第 2 个 shell_run 进来：
  in_flight = 1 >= 1
  没有名额，进入条件变量等待

第 1 个 shell_run 执行完：
  in_flight--
  cond_broadcast 唤醒等待者

第 2 个 shell_run 获得名额：
  in_flight++
  开始执行
```

所以它像一个闸门：

```text
          tool.shell_run lane
请求 1 ──> [ 有名额 ] ──> 执行
请求 2 ──> [ 没名额 ] ──> 等待
请求 3 ──> [ 没名额 ] ──> 等待
```

它和线程池的区别：

| 概念 | 职责 |
|------|------|
| 线程池 | 创建/管理 worker 线程，把任务放到 worker 上执行 |
| lane 并发闸门 | 不执行任务，只判断某类工具当前能不能开始执行 |
 
为什么需要 lane 而不是全局一个并发数？因为不同工具的风险和资源消耗不同：

```text
shell_run       风险高，通常应该限制为 1
http_request    可以允许多个并发
plugin 工具     可能共享同一个外部进程，需要按 plugin 限流
mcp 工具        可能共享同一个 MCP server，需要按 server 限流
```

pool 内部用 `mutex + cond` 维护每条 lane 的 `in_flight`：

```text
如果 in_flight < concurrency:
    in_flight++
    允许执行
否则:
    cond_timedwait 50ms
    醒来后检查 cancel_token
```

执行结束后：

```text
in_flight--
cond_broadcast 唤醒等待者
```

所以 pool 的职责是：

- 控制同类工具最多同时多少个调用在飞行中。
- 给工具提供本次调用的 `timeout_ms`。
- 等待 lane 名额时响应 cancel token。
- 不拥有工具，不调用工具，不创建执行线程。

**6. 插件工具和 MCP 工具的数据交互**

插件和 MCP 在 runtime 看来仍然是普通工具：它们也注册成 `cc_tool_t`，也通过
`tool.vtable->call(...)` 被调用。区别在于它们的 `call` 内部会把本地函数调用转成外部协议。

插件工具常见链路：

```text
tool.vtable->call
  ↓
plugin adapter
  ↓
JSON-RPC 2.0 request
  ↓
stdin/stdout
  ↓
外部插件进程
  ↓
JSON-RPC response
  ↓
cc_tool_result_t
```

MCP 工具常见链路：

```text
tool.vtable->call
  ↓
MCP tool bridge
  ↓
MCP transport
  ↓
外部 MCP server
  ↓
tools/call result
  ↓
cc_tool_result_t
```

也就是说，runtime 层看到的是统一工具接口；跨进程、stdio、HTTP、MCP transport 等细节被封装在
具体 adapter 里。这是端口-适配器架构的价值：Agent Runtime 不需要知道工具到底是本地函数、
外部进程还是远程服务。

**7. 面试回答模板**

> 工具的数据交互是通过统一的 `cc_tool_vtable_t.call` 完成的。LLM 返回的 tool call 会被解析成
> `cc_tool_call_t`，里面有工具 id、工具名和 JSON 参数。tool executor 从 registry 查找工具，
> 经过 policy engine 检查后，把 `args_json`、`cc_tool_context_t` 和 `cc_tool_result_t`
> 传给工具。工具解析 JSON 参数，执行逻辑，把成功内容或错误写入 `out_result`。runtime 再把结果
> 作为 `role="tool"` 的消息写回 session store，下一轮 LLM 就能看到 observation。
>
> 单次工具调用本身是同步函数调用，不是每次调用都新开线程。跨线程并发发生在外层 run queue，
> 而 `cc_tool_executor_pool` 不是线程池，它只是 lane 级并发闸门，负责限制某类工具同时执行的数量、
> 提供 timeout，并在等待时响应 cancel token。插件和 MCP 工具在 runtime 看来仍是普通工具，
> 只是它们的 `call` 内部会转成 JSON-RPC、stdio、HTTP 或 MCP transport。

---

## 15. 性能与体积 Benchmark 准备清单

### 15.1 先记录环境

所有 benchmark 都要先记录环境，否则数字没有可比性：

```bash
uname -a
cc --version
cmake --version
```

建议把结果写到本地笔记，不一定提交到仓库。简历里只放稳定、可复测、能解释的数字。

### 15.2 体积指标

POSIX CLI：

```bash
cmake --preset posix-cli
cmake --build --preset posix-cli
ls -lh build/app/posix/cli/bin/c-claw
size build/app/posix/cli/bin/c-claw
```

core-minimal：

```bash
cmake --preset core-minimal
cmake --build --preset core-minimal
find build/sdk/core-minimal -maxdepth 3 -type f \( -name '*.a' -o -perm -111 \) -exec ls -lh {} \;
```

ESP32 QEMU 固件：

```bash
. "$IDF_PATH/export.sh"
./scripts/esp32_s3_qemu.sh build
ls -lh build/app/esp32/esp32_s3_qemu/*.bin
```

可写口径：

> core-minimal 构建产物约 xx，POSIX CLI 单文件约 xx，ESP32-S3 QEMU 固件约 xx KiB。

### 15.3 测试耗时指标

```bash
ctest --test-dir build/app/posix/cli --output-on-failure
ctest --test-dir build/sdk/core-minimal --output-on-failure
```

可写口径：

> POSIX CLI 27 项 CTest 在本地约 xx 秒完成，覆盖并发、插件、MCP、工具执行和配置路径。

注意：测试耗时受机器影响很大，简历上通常不必写秒数；面试中可以作为补充证据。

### 15.4 冷启动指标

如果系统有 `/usr/bin/time`：

```bash
/usr/bin/time -f 'elapsed=%e sec maxrss=%M KB' ./build/app/posix/cli/bin/c-claw --help
```

可写口径：

> CLI 冷启动到 help 输出约 xx ms，峰值 RSS 约 xx MB。

### 15.5 插件调用延迟指标

插件 benchmark 比较适合这个项目，因为 JSON-RPC over stdio 是明确的工程亮点。建议新增一个
专门 benchmark，而不是复用功能测试：

```text
目标：启动一个 echo plugin，连续调用 1000 次，记录平均/中位/p95 延迟。
关注点：JSON encode/decode、stdio flush/read、请求 id 匹配、超时处理。
注意：第一次调用可能包含进程启动成本，应单独统计 cold start 和 warm call。
```

可写口径：

> 本地 warm stdio 插件调用平均 xx ms，p95 xx ms；cold start 单独约 xx ms。

### 15.6 Run Queue 并发指标

建议设计一个 fake LLM / fake tool benchmark，避免真实网络 API 影响结果：

```text
目标：提交 N 个 session，每个 session M 个任务；同 session 必须串行，不同 session 可以跨 lane 并发。
记录：总耗时、完成任务数、取消任务数、平均排队时间。
验证：消息顺序正确，cancel token 能打断等待中的任务。
```

可写口径：

> 使用 fake backend 压测 run queue，N 个 session / M 个任务下保持同 session 顺序一致，跨 lane 并发完成，总耗时约 xx。

### 15.7 Benchmark 结果记录模板

```markdown
## Benchmark YYYY-MM-DD

环境：
- OS:
- CPU:
- Compiler:
- Build type:
- Commit:

结果：
- POSIX CLI size:
- core-minimal size:
- ctest posix-cli:
- ctest core-minimal:
- CLI cold start:
- Plugin warm call avg/p95:
- Run queue workload:

结论：
- 可以写进简历的数字：
- 不能写或需要复测的数字：
```

---

## 附录 A：完整构建流程

```bash
# Linux/macOS 桌面构建
cmake --preset posix-cli
cmake --build --preset posix-cli
ctest --test-dir build/app/posix/cli --output-on-failure

# 最小化构建（无 LLM，无 Shell）
cmake --preset core-minimal
cmake --build --preset core-minimal
ctest --test-dir build/sdk/core-minimal --output-on-failure

# ESP32 QEMU
. "$IDF_PATH/export.sh"
./scripts/esp32_s3_qemu.sh doctor
./scripts/esp32_s3_qemu.sh build
./scripts/esp32_s3_qemu.sh qemu
```

构建目录按层分开：桌面应用使用 `build/app/posix/cli`，Windows CLI 使用
`build/app/windows/cli`，SDK 最小裁剪使用 `build/sdk/core-minimal`，ESP32 QEMU
使用 `build/app/esp32/esp32_s3_qemu`。这样阅读 build 产物时能直接看出它属于
哪个层，而不会把 SDK 测试、桌面 app 和板级工程混在一个平铺目录里。

## 附录 B：核心源文件速查表

| 学习目标 | 文件 | 关键内容 |
|---------|------|---------|
| VTable 定义 | [cc_tool.h](../cclaw/ports/include/cc/ports/cc_tool.h) | tool vtable 模式 + cc_runtime_services_t |
| VTable 定义 | [cc_llm_provider.h](../cclaw/ports/include/cc/ports/cc_llm_provider.h) | provider vtable + cancel_token/think_mode |
| VTable 定义 | [cc_policy_engine.h](../cclaw/ports/include/cc/ports/cc_policy_engine.h) | 策略模式 + SAFE/MEDIUM/DANGEROUS 风险等级 |
| 线程抽象 | [cc_thread.h](../cclaw/ports/include/cc/ports/cc_thread.h) | cc_mutex_t / cc_thread_t / cc_cond_t 三平台抽象 |
| HTTP 端口 | [cc_http_client.h](../cclaw/ports/include/cc/ports/cc_http_client.h) | 函数式端口（非 vtable）+ cancel_token |
| 会话存储 | [cc_session_store.h](../cclaw/ports/include/cc/ports/cc_session_store.h) | session store vtable（含 clear_session） |
| 工厂模式 | [cc_storage_factory.c](../cclaw/adapters/src/storage/cc_storage_factory.c) | 配置驱动工厂 + SQLite→JSON 降级 |
| 构建器模式 | [cc_runtime_builder.c](../cclaw/core/src/app/cc_runtime_builder.c) | 聚合 10+ 依赖 + 级联生命周期 |
| 注册表模式 | [cc_tool_registry.h](../cclaw/ports/include/cc/ports/cc_tool_registry.h) | freeze/is_frozen/count/list_names |
| 注册表 Snapshot | [cc_tool_registry_snapshot.c](../cclaw/core/src/app/cc_tool_registry_snapshot.c) | generation + ref_count 热重载 |
| 事件总线 | [cc_event_bus.c](../cclaw/core/src/core/cc_event_bus.c) | snapshot 模式发布-订阅 |
| 多 Agent 编排 | [cc_agent_manager.h](../cclaw/core/include/cc/app/cc_agent_manager.h) | submit/collect/interrupt/reset |
| 协作式取消 | [cc_cancel_token.c](../cclaw/core/src/app/cc_cancel_token.c) | cancel token + cc_cond_timedwait |
| Run Queue | [cc_run_queue.c](../cclaw/core/src/app/cc_run_queue.c) | STEER/DROP 策略 + cancel 级联 |
| Tool Pool | [cc_tool_executor_pool.c](../cclaw/core/src/app/cc_tool_executor_pool.c) | lane 并发上限 + timeout + cancel |
| Skill 目录 | [cc_skill_catalog.c](../cclaw/core/src/app/cc_skill_catalog.c) | AgentSkills SKILL.md 解析 |
| MCP 运行时 | [cc_mcp_runtime_manager.c](../cclaw/core/src/app/cc_mcp_runtime_manager.c) | MCP transport + tools/list |
| 插件协议 | [cc_plugin_protocol.c](../cclaw/core/src/app/cc_plugin_protocol.c) | JSON-RPC 2.0 envelope |
| 依赖注入 | [cc_runtime_features.h](../cclaw/core/include/cc/app/cc_runtime_features.h) | cc_runtime_feature_set_t 描述符表 |
| Agent 循环 | [cc_agent_runtime.h](../cclaw/core/include/cc/app/cc_agent_runtime.h) | ReAct 循环 + deps/options/cancel_token |
| 错误处理 | [cc_result.h](../cclaw/core/include/cc/core/cc_result.h) | cc_result_t + cc_result_errf |
| SSE 解析器 | [cc_sse_parser.c](../cclaw/adapters/src/llm/cc_sse_parser.c) | 增量状态机分 chunk |
| 架构文档 | [architecture.md](../cclaw/docs/architecture.md) | 分层架构、profile、扩展点 |

---

> 本文档基于 C-Claw 项目的实际代码编写。部分代码块为便于讲解做了精简或改写；
> 面试前应以链接到的源码和测试为准，尤其关注所有权、错误传播和锁边界。
>
> License: Apache-2.0
