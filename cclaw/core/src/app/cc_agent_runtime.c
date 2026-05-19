/**
 * 学习导读：cclaw/core/src/app/cc_agent_runtime.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * ===========================================================================
 * cc_agent_runtime.c — Agent 运行时引擎（核心主循环）
 * ===========================================================================
 *
 * 模块在整体架构中的角色：
 * ─────────────────────────────
 * 本模块是整个 c-claw Agent 系统的"大脑"——它将 LLM、工具注册表、会话存储、
 * 策略引擎、沙箱、事件总线等多个子系统编排为一个完整的 ReAct（Reasoning +
 * Acting）对话-工具调用流水线。它是 cc_agent_runtime.h 的唯一实现。
 *
 * 上游调用方：
 *   - POSIX/Windows CLI gateway —— 通过 cc_agent_runtime_handle_message 或
 *     cc_agent_runtime_handle_message_stream 发送用户输入
 *   - ESP32 QEMU 示例入口 —— 在设备 profile 中复用同一个 runtime 编排逻辑
 *   - 测试代码 —— 注入 mock LLM/store/tool 来验证主循环行为
 *
 * 下游依赖模块：
 *   - cc_context_builder  —— 从 storage 加载历史 → 构建 messages JSON 数组
 *   - cc_tool_registry     —— 提供可用工具列表及其 JSON Schema
 *   - cc_tool_executor     —— 查找工具 → 安全策略检查 → 执行 → 发布事件
 *   - cc_session_store     —— 消息/工具调用的持久化（虚接口，支持多种后端）
 *   - cc_event_bus         —— 发布生命周期事件（llm.request.started, agent.finished 等）
 *   - cc_policy_engine     —— （由 tool_executor 内部调用）工具调用安全审查
 *   - cc_sandbox           —— 沙箱隔离环境（预留接口）
 *   - cc_logger            —— 日志输出
 *
 * 关键设计模式：
 *   - 依赖注入（Dependency Injection）：所有外部依赖通过 cc_agent_runtime_create
 *     的参数注入，runtime 不创建任何具体实现，保证可测试性和可替换性。
 *   - 模板方法（Template Method）：主循环定义固定步骤，但每步的具体行为由注入
 *     的组件决定。
 *   - 虚函数表（VTable）：LLM Provider、Storage、Policy 等通过虚函数表实现
 *     多态，支持不同的后端实现（如不同 LLM API、不同数据库）。
 *
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                    Agent 主循环数据流                            │
 * │                                                                 │
 * │  用户输入                                                       │
 * │    │                                                            │
 * │    ▼                                                            │
 * │  ┌──────────────────┐                                           │
 * │  │ runtime helpers   │ ← 确保会话存在 + 追加用户消息到 storage  │
 * │  └────────┬─────────┘                                           │
 * │           │                                                     │
 * │           ▼                                                     │
 * │  ┌──────────────────────────────────────┐                       │
 * │  │         step-loop (0..max_steps)     │                       │
 * │  │                                      │                       │
 * │  │  ① cc_context_builder               │ ← 从 storage 构建     │
 * │  │     加载历史消息 + system_prompt      │    消息列表 JSON      │
 * │  │           │                          │                       │
 * │  │  ② cc_tool_registry                 │ ← 构建 tools JSON     │
 * │  │     构建可用工具 schema               │                      │
 * │  │           │                          │                       │
 * │  │  ③ LLM.chat                          │ ← 发送 messages +     │
 * │  │     调用大语言模型                     │    tools 到 LLM API  │
 * │  │           │                          │                       │
 * │  │           ├── has_tool_call ──────────┤                       │
 * │  │           │  ④ cc_tool_executor      │ ← 查找/策略/执行工具  │
 * │  │           │  ⑤ storage 追加          │    结果写回 storage   │
 * │  │           │  ⑥ tool 消息追加到 storage│                       │
 * │  │           │  ⑦ continue ──→ 回到①   │ ← LLM 看到工具结果后  │
 * │  │           │                          │    也许再调工具或回答  │
 * │  │           │                          │                       │
 * │  │           └── has_text ───────────────┤                       │
 * │  │              ⑧ 存储 assistant 消息   │ ← 最终回答            │
 * │  │              ⑨ 发布 agent.finished   │                       │
 * │  │              ⑩ return 响应文本       │                       │
 * │  │                                      │                       │
 * │  │  循环结束（step >= max_steps）        │                       │
 * │  │      → 返回 "max steps reached"      │                       │
 * │  └──────────────────────────────────────┘                       │
 * └─────────────────────────────────────────────────────────────────┘
 *
 * ReAct 模式核心原理：
 * ────────────────────
 * ReAct（Reasoning + Acting）是当前 Agent 系统的标准范式。其核心思想是：
 *   - 每轮迭代中，LLM 先"思考"（Reasoning），决定是否需要调用工具
 *   - 如果需要，LLM 输出 tool_call（Acting），由系统执行工具并返回结果
 *   - LLM 看到工具结果后继续"思考"，可能再次调用工具或给出最终回答
 *   - 这个过程持续直到 LLM 认为任务完成（返回纯文本）或达到步数上限
 *
 * 与传统一问一答的区别：
 *   - 传统：用户提问 → LLM 直接回答（一次性）
 *   - ReAct：用户提问 → LLM 思考 → 调工具A → LLM 思考 → 调工具B → ... → LLM 回答
 *   这种多步推理让 LLM 能够完成需要"获取信息→分析→再获取→再分析"的复杂任务。
 *
 * reasoning_content（思维链）的设计决策：
 * ──────────────────────────────────────
 * 部分 LLM（如 DeepSeek-R1、o1）支持返回 reasoning_content 字段，
 * 即模型在给出最终回答前的内部"思考过程"。与 tool_call 的思考不同，
 * reasoning_content 是 LLM 自己的内省推理。
 *
 * 存储策略：
 *   - tool_call 场景：reasoning_content 与 tool_calls 一起打包存入
 *     {"tool_calls":[...], "reasoning_content":"..."}
 *     这样 context_builder 在后续迭代中能将其还原为 assistant 消息的
 *     reasoning_content 字段，LLM 可以继续之前的推理链。
 *   - 纯文本场景：reasoning_content 与 text 一起打包存入
 *     {"text":"...", "reasoning_content":"..."}
 *     此时 reasoning_content 代表 LLM 在输出最终文本前的思考过程。
 *
 * 为什么用 JSON 包装而非两个独立字段：
 *   当 reasoning_content 存在时，将其包装为 JSON 统一存储，因为
 *   cc_message_t 只有一个 content 字段。context_builder 检测到 content
 *   以 '{' 开头时，会按 JSON 格式解析以提取 text 和 reasoning_content。
 */

#include "cc_agent_runtime_internal.h"
#include "cc/app/cc_context_builder.h"
#include "cc/app/cc_tool_executor.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_json.h"
#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_thread.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static cc_mutex_t g_id_mutex = NULL;
static unsigned long g_id_counter = 0;

/**
 * ensure_id_mutex — 按需初始化全局消息 ID 互斥锁，保护进程内递增计数。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
static void ensure_id_mutex(void)
{
    if (!g_id_mutex) {
        cc_mutex_create(&g_id_mutex);
    }
}

/**
 * generate_id — 生成唯一消息标识符
 *
 * 功能：
 *   为每条新创建的消息生成全局唯一的 ID。
 *
 * 格式：
 *   msg_<Unix秒级时间戳>_<进程内递增计数>
 *   示例：msg_1715000000_42
 *
 * @return 堆上分配的 ID 字符串（调用方负责 free）
 *
 * 设计决策——为什么用时间戳+进程内计数而非外部 UUID 库：
 *   - 避免引入额外 UUID 依赖，减少跨平台编译和链接复杂度
 *   - 递增计数由互斥锁保护，在同一进程内不会因为并发消息创建而重复
 *   - ID 格式紧凑，便于日志阅读和测试断言
 *   - 时间戳部分使得 ID 天然具有可排序性（按创建时间排序 ↔ 按 ID 排序）
 *   - 前缀 "msg_" 表明这是消息 ID（区别于 "ses_" 前缀的会话 ID）
 *
 * 线程安全性说明：
 *   g_id_counter 的递增被 g_id_mutex 保护。这里的唯一性边界是"当前进程"；
 *   如果未来多个进程共享同一个 session store，应改为数据库序列、UUID
 *   或由存储后端生成 ID。
 */
static char *generate_id(void)
{
    char buf[64];
    ensure_id_mutex();
    cc_mutex_lock(g_id_mutex);
    unsigned long next = ++g_id_counter;
    cc_mutex_unlock(g_id_mutex);
    snprintf(buf, sizeof(buf), "msg_%ld_%lu", (long)time(NULL), next);
    return strdup(buf);
}

/**
 * cc_agent_runtime_create — 创建 Agent 运行时实例（工厂函数）
 *
 * 功能：
 *   分配并初始化一个完整的 Agent 运行时实例，将所有顶层组件组装在一起。
 *   采用"依赖注入"模式，所有外部依赖都通过参数传入。
 *
 * @param deps          运行时依赖集合，包含 LLM、工具注册表、存储、策略、沙箱、
 *                      事件总线、日志器、记忆存储和工具审批回调。
 * @param options       运行时行为选项，包含可复制的 config 和 thinking_mode 初始值。
 * @param out_runtime   输出参数，成功时指向新创建的运行时实例
 *
 * @return CC_OK 成功；CC_ERR_OUT_OF_MEMORY 内存分配失败
 *
 * 实现要点：
 *   - 使用 calloc 分配结构体，保证所有字段初始化为零/NULL
 *   - 对配置中的字符串字段做 strdup 深拷贝，防止调用方释放 config 后 runtime 内部
 *     持有悬空指针。这是重要的防御性编程——config 可能是在调用者的栈上分配的临时变量。
 *   - 不会增加注入组件的引用计数（C 语言无 GC），调用方负责管理这些组件的生命周期。
 *
 * 典型调用方式：
 *   cc_agent_runtime_t *runtime = NULL;
 *   cc_agent_runtime_deps_t deps = { .llm = llm, .store = store, ... };
 *   cc_agent_runtime_options_t options = {
 *       .config = config,
 *       .thinking_mode = config.thinking_mode
 *   };
 *   cc_agent_runtime_create(&deps, &options, &runtime);
 */
cc_result_t cc_agent_runtime_create(
    const cc_agent_runtime_deps_t *deps,
    const cc_agent_runtime_options_t *options,
    cc_agent_runtime_t **out_runtime
)
{
    if (!deps || !options || !out_runtime) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime create argument");
    }
    cc_agent_runtime_config_t config = options->config;
    cc_agent_runtime_t *runtime = calloc(1, sizeof(cc_agent_runtime_t));
    if (!runtime) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create agent runtime");

    cc_result_t mutex_rc = cc_mutex_create(&runtime->mutex);
    if (mutex_rc.code != CC_OK) {
        free(runtime);
        return mutex_rc;
    }

    /* 深拷贝配置中的字符串字段（config 可能是临时栈变量） */
    runtime->config = config;
    if (runtime->config.system_prompt)
        runtime->config.system_prompt = strdup(config.system_prompt);
    if (runtime->config.workspace_dir)
        runtime->config.workspace_dir = strdup(config.workspace_dir);
    if (runtime->config.model)
        runtime->config.model = strdup(config.model);

    /* 组装所有依赖组件 — 依赖注入模式 */
    runtime->llm = deps->llm;
    runtime->tool_registry = deps->tool_registry;
    runtime->store = deps->store;
    runtime->policy = deps->policy;
    runtime->sandbox = deps->sandbox;
    runtime->event_bus = deps->event_bus;
    runtime->logger = deps->logger;
    runtime->memory_store = deps->memory_store;
    runtime->thinking_mode = options->thinking_mode;
    runtime->services.event_bus = deps->event_bus;
    runtime->services.logger = deps->logger;
    runtime->services.memory_store = deps->memory_store;
    runtime->services.approve_tool_call = deps->approve_tool_call;
    runtime->services.approval_user_data = deps->approval_user_data;

    *out_runtime = runtime;
    return cc_result_ok();
}

/**
 * build_tool_calls_json — 把单个工具调用编码成 LLM 兼容的 tool_calls JSON 数组字符串。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param call 借用的指针参数；若需要长期保存内容，函数会复制。
 * @return 新分配字符串；返回 NULL 表示分配或输入校验失败，调用方负责 free。
 */
static char *build_tool_calls_json(const cc_tool_call_t *call)
{
    cc_json_value_t *tcs = cc_json_create_array();
    cc_json_value_t *tc = cc_json_create_object();
    cc_json_object_set(tc, "id", cc_json_create_string(call && call->id ? call->id : ""));
    cc_json_object_set(tc, "type", cc_json_create_string("function"));
    cc_json_value_t *func = cc_json_create_object();
    cc_json_object_set(func, "name", cc_json_create_string(call && call->name ? call->name : ""));
    cc_json_object_set(func, "arguments",
        cc_json_create_string(call && call->arguments_json ? call->arguments_json : "{}"));
    cc_json_object_set(tc, "function", func);
    cc_json_array_append(tcs, tc);
    char *json = cc_json_stringify(tcs);
    cc_json_destroy(tcs);
    return json;
}

/**
 * cc_agent_runtime_store_assistant_text — 把最终 assistant 文本和可选 reasoning_content 包装成消息并追加到 session store。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 * @param session_id 借用的只读字符串；函数不会释放该指针。
 * @param text 借用的只读字符串；函数不会释放该指针。
 * @param reasoning_content 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_agent_runtime_store_assistant_text(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *text,
    const char *reasoning_content
)
{
    if (!runtime || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null assistant text argument");
    }
    cc_message_t *assistant_msg = NULL;
    char *aid = generate_id();
    cc_result_t rc = cc_message_create(
        aid, session_id, CC_ROLE_ASSISTANT, text ? text : "", NULL, &assistant_msg);
    free(aid);
    if (rc.code != CC_OK) return rc;
    if (reasoning_content && reasoning_content[0]) {
        rc = cc_message_set_reasoning_content(assistant_msg, reasoning_content);
        if (rc.code != CC_OK) {
            cc_message_destroy(assistant_msg);
            return rc;
        }
    }
    if (runtime->store.vtable && runtime->store.vtable->append_message) {
        rc = runtime->store.vtable->append_message(runtime->store.self, assistant_msg);
    }
    cc_message_destroy(assistant_msg);
    return rc;
}

/**
 * cc_agent_runtime_execute_tool_step — 参与工具注册、工具调用或工具结果写回流程。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 * @param session_id 借用的只读字符串；函数不会释放该指针。
 * @param call 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param reasoning_content 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_agent_runtime_execute_tool_step(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const cc_tool_call_t *call,
    const char *reasoning_content
)
{
    if (!runtime || !session_id || !call) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool step argument");
    }

    char *tool_calls_json = build_tool_calls_json(call);
    if (!tool_calls_json) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize tool calls");
    }

    cc_message_t *asst_msg = NULL;
    char *aid = generate_id();
    cc_result_t rc = cc_message_create(aid, session_id, CC_ROLE_ASSISTANT,
        NULL, call->id, &asst_msg);
    free(aid);
    if (rc.code == CC_OK) {
        rc = cc_message_set_tool_calls_json(asst_msg, tool_calls_json);
    }
    if (rc.code == CC_OK && reasoning_content && reasoning_content[0]) {
        rc = cc_message_set_reasoning_content(asst_msg, reasoning_content);
    }
    free(tool_calls_json);
    if (rc.code != CC_OK) {
        cc_message_destroy(asst_msg);
        return rc;
    }
    if (runtime->store.vtable && runtime->store.vtable->append_message) {
        runtime->store.vtable->append_message(runtime->store.self, asst_msg);
    }
    cc_message_destroy(asst_msg);

    cc_tool_result_t tool_result;
    memset(&tool_result, 0, sizeof(tool_result));
    cc_tool_executor_execute(runtime, session_id, call, &tool_result);

    if (runtime->store.vtable && runtime->store.vtable->append_tool_call) {
        runtime->store.vtable->append_tool_call(runtime->store.self, session_id, call);
    }
    if (runtime->store.vtable && runtime->store.vtable->append_tool_result) {
        runtime->store.vtable->append_tool_result(
            runtime->store.self, session_id, call->id, &tool_result);
    }

    cc_message_t *tool_msg = NULL;
    char *tid = generate_id();
    rc = cc_message_create(tid, session_id, CC_ROLE_TOOL,
        tool_result.ok ? tool_result.content : tool_result.error,
        call->id, &tool_msg);
    free(tid);
    if (rc.code == CC_OK && runtime->store.vtable && runtime->store.vtable->append_message) {
        runtime->store.vtable->append_message(runtime->store.self, tool_msg);
    }
    cc_message_destroy(tool_msg);
    free(tool_result.content);
    free(tool_result.error);
    free(tool_result.metadata_json);
    return rc.code == CC_OK ? cc_result_ok() : rc;
}

/*
 * ═══════════════════════════════════════════════════════════════════════
 * 流式 Agent 循环 — 上下文和回调结构体定义
 * ═══════════════════════════════════════════════════════════════════════
 *
 * 以下为 cc_agent_runtime_handle_message_stream 的实现。
 * 与 handle_message 的同步循环不同，流式版本通过 chat_stream 回调
 * 逐步接收 LLM 输出，同时通过事件总线实时广播增量数据。
 */

/*
 * stream_loop_ctx_t — 流式循环上下文
 *
 * 在 chat_stream 回调中持久化状态，记录当前流式迭代中的累积状态。
 * 因为在 chat_stream 回调中没有传统的 for 循环，所有状态（文本、
 * 思考内容、工具调用参数、步数等）必须保存在此结构体中。
 *
 * 字段说明：
 *   runtime          — Agent 运行时指针
 *   session_id       — 会话 ID（用于存储和日志）
 *   step             — 当前 ReAct 步数（每次 LLM 调用 +1）
 *   text_builder     — 累积用户可见的文本回复
 *   thinking_builder — 累积模型思考推理过程
 *   args_builder     — 累积工具调用参数 JSON
 *   cur_tool_name    — 当前工具名称（TOOL_START 时记录）
 *   cur_tool_id      — 当前工具调用 ID（LLM 分配）
 *   has_tool_call    — 本轮流是否有工具调用
 *   finished         — 流是否已结束
 *   response_text    — 最终响应的完整文本（返回给调用方）
 */
typedef struct {
    cc_agent_runtime_t *runtime;
    const char *session_id;
    int step;
    int chunk_count;
    cc_string_builder_t text_builder;
    cc_string_builder_t thinking_builder;
    cc_string_builder_t args_builder;
    char *cur_tool_name;
    char *cur_tool_id;
    int has_tool_call;
    int finished;
    char *response_text;
} stream_loop_ctx_t;

/*
 * stream_chunk_callback — chat_stream 的 chunk 回调函数
 *
 * 功能：
 *   每收到一个 SSE chunk 时被调用。根据 chunk 类型执行对应的累积、
 *   事件发布、工具调用存储和执行操作。
 *
 * 参数：
 *   chunk     — 当前的增量数据块
 *   user_data — 指向 stream_loop_ctx_t 的指针
 *
 * Chunk 处理流程：
 *
 *   CC_STREAM_CHUNK_THINKING：
 *     累积到 thinking_builder，通过事件总线发布 stream.thinking
 *
 *   CC_STREAM_CHUNK_TEXT：
 *     累积到 text_builder，通过事件总线发布 stream.text
 *
 *   CC_STREAM_CHUNK_TOOL_START：
 *     记录工具名称和 ID，通过事件总线发布 stream.tool.start
 *
 *   CC_STREAM_CHUNK_TOOL_DELTA：
 *     累积参数 JSON 片段，通过事件总线发布 stream.tool.delta
 *
 *   CC_STREAM_CHUNK_TOOL_END：
 *     工具调用参数收集完毕。存储 tool_call 消息、执行工具、
 *     存储 tool 结果消息、发布 stream.tool.end
 *
 *   CC_STREAM_CHUNK_FINISHED：
 *     标记流结束，不在此回调中做业务处理（由主函数检查 finished 标志）
 *
 * 关键设计决策：
 *   - 回调不做重入保护：chat_stream 保证顺序调用
 *   - 事件总线的 JSON 手动拼接（避免 cc_json 的开销）
 *   - 工具调用参数在 DELTA 阶段累积，END 阶段提交执行
 */

/*
 * execute_pending_tool — 执行当前挂起的工具调用
 *
 * 从 ctx 中取出 cur_tool_name、cur_tool_id 和 args_builder，
 * 创建 tool_call 消息、执行工具、存储 tool 结果消息。
 * 执行完毕后清除 cur_tool_name/cur_tool_id 并清空 args_builder。
 *
 * 多工具并行调用处理：
 *   当 LLM 在同一轮中调用多个工具时，TOOL_START 回调会先
 *   执行前一个挂起的工具，再开始追踪新工具。
 *   TOOL_END 则执行最后一个挂起的工具。
 */
static void execute_pending_tool(stream_loop_ctx_t *ctx)
{
    if (!ctx->cur_tool_name) return;

    const char *arguments = cc_string_builder_cstr(&ctx->args_builder);
    if (!arguments || strlen(arguments) == 0) {
        arguments = "{}";
    }

    /* 创建并存储 tool_call 消息 */
    cc_message_t *tc_msg = NULL;
    char *tid = generate_id();
    cc_message_create(tid, ctx->session_id, CC_ROLE_ASSISTANT,
        NULL, ctx->cur_tool_id ? ctx->cur_tool_id : "", &tc_msg);
    free(tid);

    /* 以 JSON 包装存储 tool_calls */
    cc_json_value_t *tc_json = cc_json_create_object();
    cc_json_value_t *tcs_arr = cc_json_create_array();
    cc_json_value_t *tc_item = cc_json_create_object();
    cc_json_object_set(tc_item, "id",
        cc_json_create_string(ctx->cur_tool_id ? ctx->cur_tool_id : ""));
    cc_json_object_set(tc_item, "type", cc_json_create_string("function"));
    cc_json_value_t *func_obj = cc_json_create_object();
    cc_json_object_set(func_obj, "name",
        cc_json_create_string(ctx->cur_tool_name));
    cc_json_object_set(func_obj, "arguments", cc_json_create_string(arguments));
    cc_json_object_set(tc_item, "function", func_obj);
    cc_json_array_append(tcs_arr, tc_item);
    char *tcs_json = cc_json_stringify(tcs_arr);
    cc_json_object_set(tc_json, "tool_calls", tcs_arr);
    const char *thinking = cc_string_builder_cstr(&ctx->thinking_builder);
    if (thinking && strlen(thinking) > 0) {
        cc_json_object_set(tc_json, "reasoning_content",
            cc_json_create_string(thinking));
    }
    tc_msg->content = cc_json_stringify(tc_json);
    cc_message_set_tool_calls_json(tc_msg, tcs_json);
    cc_message_set_reasoning_content(tc_msg, thinking);
    free(tcs_json);
    free(tc_msg->content);
    tc_msg->content = NULL;
    cc_json_destroy(tc_json);

    if (ctx->runtime->store.vtable && ctx->runtime->store.vtable->append_message) {
        ctx->runtime->store.vtable->append_message(ctx->runtime->store.self, tc_msg);
    }
    cc_message_destroy(tc_msg);

    /*
     * 策略审批 + 工具执行
     */
    cc_tool_call_t call;
    memset(&call, 0, sizeof(call));
    call.name = ctx->cur_tool_name;
    call.arguments_json = (char *)arguments;
    if (ctx->cur_tool_id) {
        call.id = ctx->cur_tool_id;
    }

    cc_tool_result_t tres = {0};
    cc_tool_executor_execute(ctx->runtime, ctx->session_id, &call, &tres);

    char *tool_result_content = NULL;
    if (tres.ok) {
        cc_json_value_t *res_json = cc_json_create_object();
        cc_json_object_set(res_json, "success", cc_json_create_bool(1));
        cc_json_object_set(res_json, "result",
            cc_json_create_string(tres.content ? tres.content : ""));
        tool_result_content = cc_json_stringify_unformatted(res_json);
        cc_json_destroy(res_json);
    } else {
        cc_json_value_t *res_json = cc_json_create_object();
        cc_json_object_set(res_json, "success", cc_json_create_bool(0));
        cc_json_object_set(res_json, "error",
            cc_json_create_string(tres.error ? tres.error : "unknown error"));
        tool_result_content = cc_json_stringify_unformatted(res_json);
        cc_json_destroy(res_json);
    }
    /* 创建并存储 tool 结果消息 */
    cc_message_t *tool_msg = NULL;
    char *tcid = ctx->cur_tool_id ? strdup(ctx->cur_tool_id) : NULL;
    tid = generate_id();
    cc_message_create(tid, ctx->session_id, CC_ROLE_TOOL,
        tool_result_content ? tool_result_content : "{}", tcid, &tool_msg);
    free(tid);
    free(tcid);

    if (ctx->runtime->store.vtable && ctx->runtime->store.vtable->append_message) {
        ctx->runtime->store.vtable->append_message(ctx->runtime->store.self, tool_msg);
    }
    cc_message_destroy(tool_msg);

    /* 发布 stream.tool.end 事件 */
    cc_event_bus_t *bus = ctx->runtime->event_bus;
    if (bus) {
        cc_json_value_t *payload_json = cc_json_create_object();
        cc_json_object_set(payload_json, "tool_name",
            cc_json_create_string(ctx->cur_tool_name));
        cc_json_object_set(payload_json, "tool_id",
            cc_json_create_string(ctx->cur_tool_id ? ctx->cur_tool_id : ""));
        cc_json_object_set(payload_json, "arguments",
            cc_json_create_string(arguments));
        cc_json_object_set(payload_json, "ok", cc_json_create_bool(tres.ok));
        if (tres.ok) {
            cc_json_object_set(payload_json, "result",
                cc_json_create_string(tres.content ? tres.content : ""));
        } else {
            cc_json_object_set(payload_json, "error",
                cc_json_create_string(tres.error ? tres.error : "unknown error"));
        }
        char *payload = cc_json_stringify_unformatted(payload_json);
        cc_json_destroy(payload_json);
        if (payload) {
            cc_event_bus_publish(bus, CC_EVENT_STREAM_TOOL_END, payload);
            free(payload);
        }
    }
    free(tool_result_content);
    free(tres.content);
    free(tres.error);
    free(tres.metadata_json);

    /* 清除挂起工具状态 */
    free(ctx->cur_tool_name);
    ctx->cur_tool_name = NULL;
    free(ctx->cur_tool_id);
    ctx->cur_tool_id = NULL;
    cc_string_builder_clear(&ctx->args_builder);
}

/**
 * stream_loop_callback — 维护流式输出缓冲和事件分发状态。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param chunk 借用的指针参数；若需要长期保存内容，函数会复制。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
static void stream_loop_callback(const cc_stream_chunk_t *chunk, void *user_data)
{
    stream_loop_ctx_t *ctx = (stream_loop_ctx_t *)user_data;
    if (!ctx || !ctx->runtime) return;

    ctx->chunk_count++;
    cc_event_bus_t *bus = ctx->runtime->event_bus;

    if (ctx->step >= 2 && getenv("CCLAW_DEBUG")) {
        static const char *cnames[] = {
            "TEXT","THINKING","TOOL_START","TOOL_DELTA","TOOL_END","FINISHED"
        };
        fprintf(stderr, "[CB] step=%d chunk=%d/%s\n",
            ctx->step, chunk->type,
            (chunk->type >= 0 && chunk->type < 6) ? cnames[chunk->type] : "?");
    }

    switch (chunk->type) {

    case CC_STREAM_CHUNK_THINKING:
        /*
         * 思考内容增量 — 模型的"内心独白"
         * 累积到 thinking_builder，通过事件总线广播
         */
        if (chunk->content && strlen(chunk->content) > 0) {
            cc_string_builder_append(&ctx->thinking_builder, chunk->content);
            if (bus) {
                /* payload: {"content":"思考内容增量..."} */
                cc_event_bus_publish(bus, CC_EVENT_STREAM_THINKING, chunk->content);
            }
        }
        break;

    case CC_STREAM_CHUNK_TEXT:
        /*
         * 文本增量 — 用户可见的回复片段
         * 累积到 text_builder，通过事件总线广播
         */
        if (chunk->content && strlen(chunk->content) > 0) {
            cc_string_builder_append(&ctx->text_builder, chunk->content);
            if (bus) {
                cc_event_bus_publish(bus, CC_EVENT_STREAM_TEXT, chunk->content);
            }
        }
        break;

    case CC_STREAM_CHUNK_TOOL_START:
        /*
         * 工具调用开始 — LLM 决定使用工具
         *
         * 多工具并行调用处理：
         *   如果已有挂起的工具（cur_tool_name != NULL），先执行它。
         *   当 LLM 在同一轮中调用多个工具时，新工具的 TOOL_START
         *   意味着前一个工具的参数已收集完毕，可以提交执行。
         */
        if (ctx->cur_tool_name != NULL) {
            execute_pending_tool(ctx);
        }
        ctx->has_tool_call = 1;
        ctx->cur_tool_name = chunk->tool_name ? strdup(chunk->tool_name) : NULL;
        ctx->cur_tool_id = chunk->tool_id ? strdup(chunk->tool_id) : NULL;
        cc_string_builder_clear(&ctx->args_builder);

        if (bus) {
            char payload[512];
            snprintf(payload, sizeof(payload),
                "{\"tool_name\":\"%s\",\"tool_id\":\"%s\"}",
                ctx->cur_tool_name ? ctx->cur_tool_name : "",
                ctx->cur_tool_id ? ctx->cur_tool_id : "");
            cc_event_bus_publish(bus, CC_EVENT_STREAM_TOOL_START, payload);
        }
        break;

    case CC_STREAM_CHUNK_TOOL_DELTA:
        /*
         * 工具调用参数增量 — arguments JSON 片段
         * 累积到 args_builder，通过事件总线广播增量
         */
        if (chunk->content && strlen(chunk->content) > 0) {
            cc_string_builder_append(&ctx->args_builder, chunk->content);
            if (bus) {
                cc_event_bus_publish(bus, CC_EVENT_STREAM_TOOL_DELTA, chunk->content);
            }
        }
        break;

    case CC_STREAM_CHUNK_TOOL_END:
        /*
         * 工具调用参数收集完毕 — 执行最后一个挂起的工具
         *
         * 多工具并行调用时，之前的工具已在各自 TOOL_START 时
         * 被执行，此处仅需执行最后一个挂起的工具。
         */
        execute_pending_tool(ctx);
        break;

    case CC_STREAM_CHUNK_FINISHED:
        ctx->finished = 1;
        break;
    }
}

/**
 * cc_agent_runtime_handle_message_stream — 流式处理用户消息
 *
 * 功能：
 *   与 cc_agent_runtime_handle_message 等效，但使用 LLM 流式接口。
 *   在响应生成过程中通过事件总线实时发布增量数据。
 *
 * 参数：
 *   runtime       — 运行时实例
 *   session_id    — 目标会话 ID
 *   user_input    — 用户输入文本
 *   out_response  — 输出：Agent 的最终文本回复
 *
 * 返回值：cc_result_t
 *
 * ─── 流式循环流程 ───────────────────────────────────────────────────
 *
 *   与同步 handle_message 的 for 循环类似，但 LLM 调用使用 chat_stream：
 *
 *   1. 构造 messages JSON 和 tools JSON
 *   2. 检查 chat_stream vtable 是否可用，不可用则降级为同步 chat
 *   3. 调用 chat_stream，在 stream_loop_callback 中实时处理 chunk
 *   4. 每次 TOOL_END 后，准备下一轮 LLM 调用（回到步骤 1）
 *   5. 流结束且无工具调用 → 返回累积的文本回复
 *
 * ─── 降级策略 ─────────────────────────────────────────────────────────
 *
 *   当 LLM Provider 不支持 chat_stream 时（vtable->chat_stream == NULL），
 *   自动降级为同步 chat 模式：
 *   - 调用 handle_message 完成完整 Agent 循环
 *   - 逐字分割最终回复文本，通过事件总线发送 stream.text 增量
 *   - 最后发送 stream.finished 事件
 *
 *   这样 CLI Gateway 无需感知降级是否发生，始终从事件总线获取相同的事件序列。
 */
cc_result_t cc_agent_runtime_handle_message_stream(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
)
{
    *out_response = NULL;

    if (!runtime || !session_id || !user_input) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "NULL argument");
    }

    /*
     * 调用前准备: 将用户消息持久化到 storage
     *
     * 与同步 handle_message 相同，在 ReAct 循环开始前必须先将用户输入
     * 写入存储，否则 cc_context_builder_build_messages 无法从存储中
     * 加载到本次用户消息，LLM 将看到空的或不完整的上下文。
     */
    cc_message_t *user_msg = NULL;
    char *msg_id = generate_id();
    cc_result_t rc = cc_message_create(msg_id, session_id, CC_ROLE_USER, user_input, NULL, &user_msg);
    free(msg_id);

    if (rc.code != CC_OK) return rc;

    if (runtime->store.vtable && runtime->store.vtable->append_message) {
        rc = runtime->store.vtable->append_message(runtime->store.self, user_msg);
        if (rc.code != CC_OK) {
            cc_message_destroy(user_msg);
            return rc;
        }
    }
    cc_message_destroy(user_msg);

    /*
     * 降级检查：如果 LLM Provider 不支持 chat_stream，
     * 回退到同步 chat 模式并通过事件总线模拟流式事件。
     */
    if (!runtime->llm.vtable || !runtime->llm.vtable->chat_stream) {
        char *sync_response = NULL;
        cc_result_t rc = cc_agent_runtime_handle_message(
            runtime, session_id, user_input, &sync_response);

        /*
         * 将同步回复逐字分割为流式事件，保持 CLI Gateway 等
         * 订阅方无需感知降级是否发生。
         */
        if (sync_response && runtime->event_bus) {
            /*
             * 为简单起见，按词而非按字符分割：
             * 在空格处切分，每个词作为一次 stream.text 事件。
             * 对于中文等无空格语言，回退为逐字符发送。
             */
            const char *p = sync_response;
            const char *start = p;
            while (*p) {
                if (*p == ' ' || *p == '\n') {
                    if (p > start) {
                        char word[256] = {0};
                        size_t wlen = p - start;
                        if (wlen > 255) wlen = 255;
                        strncpy(word, start, wlen + 1);
                        word[wlen] = ' ';
                        word[wlen + 1] = '\0';
                        cc_event_bus_publish(runtime->event_bus, CC_EVENT_STREAM_TEXT, word);
                    }
                    start = p + 1;
                }
                p++;
            }
            if (p > start) {
                char word[256] = {0};
                size_t wlen = p - start;
                if (wlen > 255) wlen = 255;
                strncpy(word, start, wlen);
                word[wlen] = '\0';
                cc_event_bus_publish(runtime->event_bus, CC_EVENT_STREAM_TEXT, word);
            }
        }

        if (sync_response) {
            *out_response = sync_response;
        }

        if (runtime->event_bus) {
            cc_event_bus_publish(runtime->event_bus, CC_EVENT_STREAM_FINISHED, "{}");
        }
        return rc;
    }

    /*
     * 流式 Agent 循环
     *
     * 使用 stream_loop_ctx_t 在每次 chat_stream 调用之间保持状态。
     * 每次工具调用后重新调用 chat_stream，让 LLM 看到工具执行结果。
     */
    stream_loop_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.runtime = runtime;
    ctx.session_id = session_id;
    cc_string_builder_init(&ctx.text_builder);
    cc_string_builder_init(&ctx.thinking_builder);
    cc_string_builder_init(&ctx.args_builder);

    for (int step = 0; step < runtime->config.max_steps; step++) {
        ctx.step = step + 1;
        ctx.finished = 0;
        ctx.has_tool_call = 0;
        ctx.chunk_count = 0;
        cc_string_builder_clear(&ctx.text_builder);
        cc_string_builder_clear(&ctx.thinking_builder);
        cc_string_builder_clear(&ctx.args_builder);
        free(ctx.cur_tool_name);
        ctx.cur_tool_name = NULL;
        free(ctx.cur_tool_id);
        ctx.cur_tool_id = NULL;

        /*
         * Step ①: 构建上下文（messages JSON）
         *
         * 与同步 handle_message 使用相同的 cc_context_builder_build_messages。
         * 共享上下文压缩逻辑（token 预算 + 摘要压缩）。
         */
        char *messages_json = NULL;
        cc_context_builder_build_messages(runtime, session_id,
            runtime->config.system_prompt, &messages_json);

        /*
         * Step ②: 构建工具 Schema JSON（为空时返回 "[]"）
         */
        char *tools_json = NULL;
        cc_tool_registry_build_schema_json(runtime->tool_registry, &tools_json);

        /*
         * Step ③: 发布 LLM 请求开始事件
         */
        if (runtime->event_bus) {
            char evt_payload[64];
            snprintf(evt_payload, sizeof(evt_payload), "{\"step\":%d}", step);
            cc_event_bus_publish(runtime->event_bus, "llm.request.started", evt_payload);
        }

        /*
         * Step ④: 构造请求并调用 LLM 流式接口
         */
        cc_llm_chat_request_t req;
        memset(&req, 0, sizeof(req));
        req.model = runtime->config.model;
        req.messages_json = messages_json;
        req.max_tokens = 4096;
        req.temperature = 0.7;
        req.stream = 1;
        req.thinking_mode = cc_agent_runtime_get_thinking_mode(runtime);

        if (tools_json && strlen(tools_json) > 2) {
            req.tools_json = tools_json;
        }

        if (getenv("CCLAW_DEBUG")) {
        fprintf(stderr, "[DEBUG] step=%d calling chat_stream, msg_json_len=%zu tools_json_len=%zu\n",
            step,
            messages_json ? strlen(messages_json) : 0,
            tools_json ? strlen(tools_json) : 0);
        }

        cc_result_t rc = runtime->llm.vtable->chat_stream(
            runtime->llm.self, &req, stream_loop_callback, &ctx);

        free(messages_json);
        free(tools_json);

        if (getenv("CCLAW_DEBUG")) {
        fprintf(stderr, "[DEBUG] step=%d chunks=%d has_tool_call=%d finished=%d "
            "text_len=%zu rc=%d\n",
            step, ctx.chunk_count, ctx.has_tool_call, ctx.finished,
            strlen(cc_string_builder_cstr(&ctx.text_builder)), rc.code);
        }

        if (rc.code != CC_OK) {
            free(ctx.cur_tool_name);
            free(ctx.cur_tool_id);
            cc_string_builder_deinit(&ctx.text_builder);
            cc_string_builder_deinit(&ctx.thinking_builder);
            cc_string_builder_deinit(&ctx.args_builder);
            *out_response = strdup("Streaming error");
            return rc;
        }

        /*
         * Step ⑤: 检查本轮结果
         *
         *   如果有工具调用 → 继续循环（下一轮 LLM 会看到工具结果）
         *   如果无工具调用且有文本 → 这是最终回复，保存并返回
         */
        if (!ctx.has_tool_call) {
            const char *final_text = cc_string_builder_cstr(&ctx.text_builder);
            const char *thinking = cc_string_builder_cstr(&ctx.thinking_builder);

            cc_agent_runtime_store_assistant_text(
                runtime, session_id, final_text, thinking);

            *out_response = final_text ? strdup(final_text) : strdup("");
            free(ctx.cur_tool_name);
            free(ctx.cur_tool_id);
            cc_string_builder_deinit(&ctx.text_builder);
            cc_string_builder_deinit(&ctx.thinking_builder);
            cc_string_builder_deinit(&ctx.args_builder);

            if (runtime->event_bus) {
                cc_event_bus_publish(runtime->event_bus, CC_EVENT_STREAM_FINISHED, "{}");
                cc_event_bus_publish(runtime->event_bus, "agent.finished", "{}");
            }
            return cc_result_ok();
        }

        /*
         * 有工具调用 → 下一轮循环
         *
         * LLM 会在下一轮看到本轮回调中已存储的 tool_call 和 tool 结果消息，
         * 继续 ReAct 推理。
         */
    }

    /*
     * max_steps 超限
     */
    free(ctx.cur_tool_name);
    free(ctx.cur_tool_id);
    cc_string_builder_deinit(&ctx.text_builder);
    cc_string_builder_deinit(&ctx.thinking_builder);
    cc_string_builder_deinit(&ctx.args_builder);

    *out_response = strdup("Agent stopped: max steps reached.");
    if (runtime->event_bus) {
        cc_event_bus_publish(runtime->event_bus, CC_EVENT_STREAM_FINISHED, "{}");
        cc_event_bus_publish(runtime->event_bus, "agent.finished",
            "{\"reason\":\"max_steps_reached\"}");
    }
    return cc_result_ok();
}

/**
 * cc_agent_runtime_destroy — 销毁 Agent 运行时实例
 *
 * 功能：
 *   释放 runtime 持有的所有堆上资源，包括深拷贝的配置字符串和结构体本身。
 *
 * @param runtime 待销毁的 Agent 运行时实例（可为 NULL，不做任何操作）
 *
 * 重要说明——内存所有权边界：
 *   本函数只释放 runtime 自身分配的资源（由 cc_agent_runtime_create 中 malloc/strdup
 *   产生的内存）。不会释放注入的外部组件（llm、tool_registry、store、policy、sandbox、
 *   event_bus、logger），这些组件的生命周期由外部调用方管理。
 *
 *   这是依赖注入模式的典型约束：谁创建（或拥有）谁负责销毁。runtime 只是"借用"
 *   这些组件，不拥有它们的所有权。
 */
void cc_agent_runtime_destroy(cc_agent_runtime_t *runtime)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    free(runtime->config.system_prompt);
    free(runtime->config.workspace_dir);
    free(runtime->config.model);
    cc_mutex_unlock(runtime->mutex);
    cc_mutex_destroy(runtime->mutex);
    free(runtime);
}

/**
 * cc_agent_runtime_set_thinking_mode — 设置思考模式开关
 *
 * 功能：
 *   在运行时动态启用或禁用 LLM 的"思考模式"（thinking mode）。
 *   思考模式是部分 LLM（如 DeepSeek-R1）支持的功能，启用后
 *   LLM 会在响应中包含 reasoning_content（内部推理链）。
 *
 * @param runtime Agent 运行时实例
 * @param enabled 非零启用思考模式，零禁用
 *
 * 线程安全性说明：
 * ──────────────────
 *   本函数通过 runtime->mutex 互斥锁保护对 thinking_mode 字段的读写。
 *   调用场景：可能在 CLI 交互线程（用户通过 /thinking 命令切换）与
 *   Agent 主循环线程（读取 thinking_mode 构造 LLM 请求参数）之间并发
 *   访问。互斥锁保证：
 *     - 写入操作（set）与读取操作（get）不会产生数据竞争
 *     - thinking_mode 的修改对下一次 LLM 请求可见（happens-before 语义）
 *
 *   WHY 用互斥锁而非原子变量：
 *     thinking_mode 本身只是一个 int，理论上可用 atomic_int。
 *     但 runtime 中其他字段（如 config 字符串）也可能在运行时被修改，
 *     统一使用 mutex 模式更一致且更容易扩展。对于低频写入场景
 *     （用户手动切换思考模式），mutex 的性能开销可忽略不计。
 *
 * 数据流影响：
 *   修改 thinking_mode 后，下一次 cc_agent_runtime_handle_message 调用
 *   中的 Step ③（组装 LLM 请求）会读取最新值并传递给 LLM provider。
 *   LLM provider 根据此标志决定是否在 API 请求中启用思考模式参数。
 */
void cc_agent_runtime_set_thinking_mode(cc_agent_runtime_t *runtime, int enabled)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    runtime->thinking_mode = enabled ? 1 : 0;
    cc_mutex_unlock(runtime->mutex);
}

/**
 * cc_agent_runtime_set_tool_approval — 参与工具注册、工具调用或工具结果写回流程。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 * @param approve_tool_call 按值传入，用于控制本次操作。
 * @param user_data 回调上下文；函数只透传或临时读取，不取得所有权。
 * 无返回值；副作用体现在对象状态、输出缓冲区或资源释放上。
 */
void cc_agent_runtime_set_tool_approval(
    cc_agent_runtime_t *runtime,
    cc_tool_approval_fn approve_tool_call,
    void *user_data
)
{
    if (!runtime) return;
    cc_mutex_lock(runtime->mutex);
    runtime->services.approve_tool_call = approve_tool_call;
    runtime->services.approval_user_data = user_data;
    cc_mutex_unlock(runtime->mutex);
}

/**
 * cc_agent_runtime_get_thinking_mode — 查询当前思考模式状态
 *
 * 功能：
 *   读取 runtime 当前的思考模式开关状态。
 *
 * @param runtime Agent 运行时实例
 * @return 非零表示思考模式已启用，零表示已禁用
 *         若 runtime 为 NULL，返回 0（安全 fallback）
 *
 * 线程安全性说明：
 * ──────────────────
 *   与 cc_agent_runtime_set_thinking_mode 使用同一把互斥锁（runtime->mutex），
 *   保证读写之间的 happens-before 关系。即使在 thinking_mode 被并发写入的
 *   过程中调用本函数，也能读到一致的（写入前或写入后的完整）值，
 *   不会出现"读到半个 int"的情况（虽然 int 在绝大多数平台上天然原子对齐）。
 *
 *   读取后解锁互斥锁，读到的值在函数返回后可能立即被另一个线程修改。
 *   调用方不应假设返回值在后续操作中保持不变——这就是"快照读取"的语义。
 *
 * 调用位置：
 *   在 cc_agent_runtime_handle_message 的 Step ③（组装 LLM 请求）中调用，
 *   将结果填入 request.thinking_mode 字段，传递给 LLM provider。
 */
int cc_agent_runtime_get_thinking_mode(cc_agent_runtime_t *runtime)
{
    if (!runtime) return 0;
    cc_mutex_lock(runtime->mutex);
    int enabled = runtime->thinking_mode;
    cc_mutex_unlock(runtime->mutex);
    return enabled;
}

/**
 * cc_agent_runtime_event_bus — 返回 runtime 注入的事件总线借用指针，供 gateway 订阅流式事件。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 * @return 返回借用对象指针；NULL 表示未注入、未找到或当前对象无效。
 */
cc_event_bus_t *cc_agent_runtime_event_bus(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->event_bus : NULL;
}

/**
 * cc_agent_runtime_tool_registry — 返回 runtime 注入的工具注册表借用指针。
 *
 * @param runtime 借用 runtime；可为 NULL。
 * @return 工具注册表借用指针；runtime 为 NULL 时返回 NULL。
 */
cc_tool_registry_t *cc_agent_runtime_tool_registry(cc_agent_runtime_t *runtime)
{
    return runtime ? runtime->tool_registry : NULL;
}

/**
 * cc_agent_runtime_session_store — 返回 runtime 内部会话存储端口地址，供测试或诊断复用。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 * @return 返回借用对象指针；NULL 表示未注入、未找到或当前对象无效。
 */
cc_session_store_t *cc_agent_runtime_session_store(cc_agent_runtime_t *runtime)
{
    return runtime ? &runtime->store : NULL;
}

/**
 * cc_agent_runtime_supports_stream — 查询当前 LLM provider 是否实现流式 chat 回调。
 *
 * @param runtime 借用 runtime；可为 NULL。
 * @return 非 0 表示支持流式输出，0 表示不支持或 runtime 无效。
 */
int cc_agent_runtime_supports_stream(cc_agent_runtime_t *runtime)
{
    return runtime && runtime->llm.vtable && runtime->llm.vtable->chat_stream;
}

/**
 * cc_agent_runtime_create_session — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：Agent runtime 应用层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param runtime 借用的对象；函数不释放该对象本身。
 * @param session_id 借用的只读字符串；函数不会释放该指针。
 * @param workspace_dir 借用的只读字符串；函数不会释放该指针。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
cc_result_t cc_agent_runtime_create_session(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *workspace_dir
)
{
    if (!runtime || !session_id) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null session create argument");
    }
    if (!runtime->store.vtable || !runtime->store.vtable->create_session) {
        return cc_result_ok();
    }
    return runtime->store.vtable->create_session(
        runtime->store.self,
        session_id,
        workspace_dir ? workspace_dir : runtime->config.workspace_dir
    );
}

/**
 * cc_agent_runtime_handle_message — Agent 核心主循环入口
 *
 * 功能：
 *   这是整个 Agent 系统的核心函数，一次调用代表一次完整的"用户发送消息 →
 *   Agent 多轮思考+工具调用 → 最终回复"的完整流程。实现了标准的 ReAct/Tool-use
 *   模式主循环。
 *
 * @param runtime       Agent 运行时实例，包含所有编排组件和配置
 * @param session_id    目标会话 ID（由外部生成，如 "ses_1715000000_42"）
 * @param user_input    用户输入的原始文本（UTF-8 编码）
 * @param out_response  输出参数，Agent 的最终回复文本（调用方负责 free）
 *
 * @return CC_OK 成功（通过 out_response 返回回复文本）
 *         其他错误码 底层组件（LLM/storage）返回的错误
 *
 * 主循环详细流程（ReAct 模式的标准实现）：
 *
 *   ┌─ 调用前准备阶段 ────────────────────────────────────────────┐
 *   │ 1. 生成消息 ID（msg_<ts>_<random>）                           │
 *   │ 2. 创建 CC_ROLE_USER 消息对象                                   │
 *   │ 3. 将用户消息通过 storage->append_message 持久化              │
 *   │ 4. 释放用户消息对象（数据已持久化，后续由 cc_context_builder    │
 *   │    从 storage 重新加载以保持一致性）                           │
 *   │                                                                │
 *   │ 设计意图：先持久化用户消息，确保即使后续 LLM 调用失败，        │
 *   │ 用户的输入也不会丢失。这是"写前日志"（WAL）思想的体现。       │
 *   └───────────────────────────────────────────────────────────────┘
 *                            │
 *                            ▼
 *   ┌─ for step = 0; step < max_steps; step++ ─────────────────────┐
 *   │                                                                │
 *   │  ① 构建消息上下文                                              │
 *   │      调用 cc_context_builder_build_messages                   │
 *   │      从 storage 加载历史消息 + 最前面插入 system_prompt        │
 *   │      得到完整的 messages JSON 数组                             │
 *   │      WHY 每轮都重新构建：因为上一轮可能新增了 tool_call 和     │
 *   │      tool_result 消息，LLM 必须看到最新的完整上下文。          │
 *   │      JSON 格式示例：                                          │
 *   │      [{"role":"system","content":"你是助手"},                  │
 *   │       {"role":"user","content":"帮我查天气"},                  │
 *   │       {"role":"assistant","content":null,                     │
 *   │        "tool_calls":[{"id":"call_1","type":"function",        │
 *   │        "function":{"name":"get_weather",                      │
 *   │        "arguments":"{\"city\":\"北京\"}"}}]},                 │
 *   │       {"role":"tool","tool_call_id":"call_1",                 │
 *   │        "content":"{\"temp\":25}"}]                            │
 *   │                                                                │
 *   │  ② 构建可用工具列表                                            │
 *   │      调用 cc_tool_registry_build_schema_json                  │
 *   │      得到符合 OpenAI function-calling 格式的 tools JSON       │
 *   │      如果没有注册任何工具，返回 "[]"（空数组）                 │
 *   │      LLM 看到空 tools 后不会发起任何 tool call                │
 *   │                                                                │
 *   │  ③ 组装 LLM 请求参数                                          │
 *   │      model:        取自 runtime->config.model                  │
 *   │      messages_json: Step ① 构建的完整消息列表                  │
 *   │      tools_json:    Step ② 构建的工具 schema 列表              │
 *   │      stream = 0:   使用非流式（一次性返回完整响应）            │
 *   │                     WHY: 简化循环逻辑，避免处理增量 tool_call  │
 *   │      max_tokens=4096: 单次 LLM 回复的最大 token 数             │
 *   │      temperature=0.7: 输出创造性控制（0=确定, 1=高创造）      │
 *   │      thinking_mode: 由 runtime->thinking_mode 控制             │
 *   │                     WHY 独立字段: 部分 LLM 的思考模式是        │
 *   │                     独立于 temperature 的参数（如 deepseek）   │
 *   │      ──→ 通过 event_bus 发布 "llm.request.started" 事件       │
 *   │                                                                │
 *   │  ④ 调用 LLM API                                                │
 *   │      runtime->llm.vtable->chat(llm.self, &request, &response)  │
 *   │      阻塞等待 API 返回完整响应                                  │
 *   │      释放临时的 messages_json 和 tools_json                    │
 *   │      ──→ 通过 event_bus 发布 "llm.response.received" 事件      │
 *   │      API 返回的 cc_llm_response_t 有两种可能结果：             │
 *   │                                                                │
 *   │      ╔═══════════════════════════════════════════════════════╗ │
 *   │      ║ 分支 A: response.has_tool_call == true                ║ │
 *   │      ║ ═════════════════════════════════════════════════════ ║ │
 *   │      ║ 表示 LLM 认为需要借助外部工具才能回答用户问题。       ║ │
 *   │      ║                                                       ║ │
 *   │      ║ Step ⑤a: 持久化 assistant 消息（含 tool_calls）      ║ │
 *   │      ║   WHY 需要包装成 JSON: cc_message_t 只有一个          ║ │
 *   │      ║   content 字段，而 tool_calls 是一个数组。            ║ │
 *   │      ║   存储格式：                                          ║ │
 *   │      ║   {                                                   ║ │
 *   │      ║     "tool_calls": [                                   ║ │
 *   │      ║       {"id":"...", "type":"function",                 ║ │
 *   │      ║        "function": {"name":"...","arguments":"..."}}  ║ │
 *   │      ║     ],                                                ║ │
 *   │      ║     "reasoning_content": "..."  // 如果有思考链       ║ │
 *   │      ║   }                                                   ║ │
 *   │      ║   tool_call_id 字段存储 LLM 返回的 call id，用于      ║ │
 *   │      ║   后续将 tool 结果与此调用关联。                     ║ │
 *   │      ║                                                       ║ │
 *   │      ║ Step ⑤b: 调用 cc_tool_executor_execute 执行工具      ║ │
 *   │      ║   内部自动完成：查找→策略检查→事件→执行               ║ │
 *   │      ║                                                       ║ │
 *   │      ║ Step ⑥: 通过 storage 持久化 tool_call 记录           ║ │
 *   │      ║ Step ⑦: 通过 storage 持久化 tool_result 记录          ║ │
 *   │      ║   这两步为审计/调试提供了完整的工具调用历史           ║ │
 *   │      ║                                                       ║ │
 *   │      ║ Step ⑧: 创建 CC_ROLE_TOOL 消息（role="tool"）        ║ │
 *   │      ║   content = 工具成功时使用 tool_result.content        ║ │
 *   │      ║            工具失败时使用 tool_result.error           ║ │
 *   │      ║   tool_call_id = 与 LLM 返回的 id 相同                ║ │
 *   │      ║   持久化到 storage                                    ║ │
 *   │      ║   WHY 同时存成功和失败的 content: LLM 可以从错误      ║ │
 *   │      ║   信息中学习并调整下一次尝试，如修正参数格式。        ║ │
 *   │      ║                                                       ║ │
 *   │      ║ Step ⑨: 释放本轮所有资源 → continue 回到 Step ①     ║ │
 *   │      ║   下一轮 LLM 将看到完整的 tool_call + tool_result    ║ │
 *   │      ║   对话历史，可以继续调用其他工具或给出最终回答       ║ │
 *   │      ╚═══════════════════════════════════════════════════════╝ │
 *   │                                                                │
 *   │      ╔═══════════════════════════════════════════════════════╗ │
 *   │      ║ 分支 B: response.has_text == true                     ║ │
 *   │      ║ ═════════════════════════════════════════════════════ ║ │
 *   │      ║ LLM 给出了最终的自然语言回答。                        ║ │
 *   │      ║                                                       ║ │
 *   │      ║ reasoning_content 存储处理：                          ║ │
 *   │      ║   如果 response.reasoning_content 非空，将 text 和    ║ │
 *   │      ║   reasoning_content 包装为 JSON:                      ║ │
 *   │      ║     {"text":"...", "reasoning_content":"..."}         ║ │
 *   │      ║   否则直接存储纯文本。                                ║ │
 *   │      ║   WHY 包装: cc_message_t.content 只有一个字段，      ║ │
 *   │      ║   需要兼容方案才能同时存储两个信息。                  ║ │
 *   │      ║   context_builder 在加载时会检测 content 以 '{' 开头 ║ │
 *   │      ║   来决定是否需要按 JSON 格式解析还原。               ║ │
 *   │      ║                                                       ║ │
 *   │      ║ 处理步骤：                                            ║ │
 *   │      ║ ⑧ 持久化 assistant 消息到 storage                    ║ │
 *   │      ║ ⑨ 将 response.text 复制到 *out_response              ║ │
 *   │      ║ ⑩ 通过 event_bus 发布 "agent.finished" 事件          ║ │
 *   │      ║ ⑪ return CC_OK → 主循环结束，响应返回给调用方       ║ │
 *   │      ╚═══════════════════════════════════════════════════════╝ │
 *   │                                                                │
 *   │      ╔═══════════════════════════════════════════════════════╗ │
 *   │      ║ 特殊情况: 既无 text 也无 tool_call                    ║ │
 *   │      ║ ═════════════════════════════════════════════════════ ║ │
 *   │      ║ LLM 返回了空响应（可能 API 错误或模型未返回内容）    ║ │
 *   │      ║ 释放 response → break 退出 for 循环                  ║ │
 *   │      ║ 最终落入超限处理逻辑，返回 "max steps reached"       ║ │
 *   │      ╚═══════════════════════════════════════════════════════╝ │
 *   │                                                                │
 *   └─ 循环体结束 ─────────────────────────────────────────────────┘
 *                            │
 *                            ▼ （仅在 for 循环耗尽仍未返回文本时到达）
 *   ┌─ 超限处理 ───────────────────────────────────────────────────┐
 *   │ 到达此处表明 Agent 在 max_steps 轮迭代内始终没有给出文本回复， │
 *   │ 可能原因：                                                    │
 *   │   - 工具调用链过于复杂，需要更多步骤（应增大 max_steps）      │
 *   │   - LLM 陷入了循环调用同一个工具（应检查工具描述或策略）      │
 *   │   - LLM 始终返回 tool_call 而没有总结（模型能力限制）         │
 *   │   - 某次 LLM 调用返回了空响应而提前 break                     │
 *   │                                                                │
 *   │ 处理方式：                                                    │
 *   │   - 返回固定提示 "Agent stopped: max steps reached."          │
 *   │   - 发布 "agent.finished" 事件（附带 reason="max_steps_reached"）│
 *   │     这里的 reason 字段用于区分正常完成和超限终止               │
 *   │   - 返回 CC_OK（不是错误），因为从系统角度看这是正常的终止    │
 *   └───────────────────────────────────────────────────────────────┘
 *
 * 关键设计决策总结：
 * ──────────────────
 *   - max_steps 作为安全阀：防止 Agent 陷入无限工具调用循环。
 *     每多一轮就意味着多一次 LLM API 调用（费用+延迟），所以需要合理设置。
 *     建议值：简单任务 3-5，复杂任务 10-20。
 *   - 每轮重新构建 messages：确保新增的工具消息对下一轮 LLM 可见，
 *     实现逐步推理链。虽然增加了 storage 读取开销，但相比 LLM API 调用
 *     的延迟和费用，这点开销可忽略不计。
 *   - 非流式模式（stream=0）：简化了循环逻辑处理。流式模式下需要增量
 *     解析 tool_call（可能跨多个 chunk），增加实现复杂度。如果未来需要
 *     更好的交互体验，可以考虑支持流式输出，同时在循环检测到 tool_call
 *     开始标记时积累完整参数再执行。
 *   - 宽松错误策略：失败的 tool call 结果仍然写入对话上下文（作为
 *     tool 消息的 content=error），让 LLM 有可能从错误中学习并调整。
 *     这比直接中断循环更符合 ReAct 的设计哲学。
 *   - 消息持久化顺序：先存 assistant 消息（tool_calls），再执行工具，
 *     最后存 tool 消息。这个顺序保证了即使工具执行过程中崩溃，tool_calls
 *     记录也不会丢失。
 */

/**
 * cc_agent_runtime_handle_message — 非流式 Agent 主入口，保存用户消息并执行 ReAct 循环。
 *
 * @param runtime 借用 runtime；不可为 NULL。
 * @param session_id 借用会话 ID。
 * @param user_input 借用用户输入文本。
 * @param out_response 输出最终回答字符串；调用方负责 free。
 * @return CC_OK 表示主循环完成；失败返回存储、LLM、工具或内存错误。
 */
cc_result_t cc_agent_runtime_handle_message(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
)
{
    cc_message_t *user_msg = NULL;
    char *msg_id = generate_id();
    cc_result_t rc = cc_message_create(msg_id, session_id, CC_ROLE_USER, user_input, NULL, &user_msg);
    free(msg_id);

    if (rc.code != CC_OK) return rc;

    /* 调用前准备: 将用户消息持久化到 storage */
    if (runtime->store.vtable && runtime->store.vtable->append_message) {
        rc = runtime->store.vtable->append_message(runtime->store.self, user_msg);
        if (rc.code != CC_OK) {
            cc_message_destroy(user_msg);
            return rc;
        }
    }
    cc_message_destroy(user_msg);

    /*
     * ═══════════════════════════════════════════════════════════════
     * Agent 主循环（ReAct / Tool-use Loop）
     * ═══════════════════════════════════════════════════════════════
     *
     * 循环不变量：
     *   - 每轮迭代开始时，storage 中包含完整的会话历史（含之前的工具调用）
     *   - messages_json 和 tools_json 在每轮都会重新构建
     *   - 如果上一轮执行了工具调用，本轮 LLM 将看到工具结果
     *   - 循环变量 step 从 0 递增到 max_steps-1
     *
     * 可能的退出路径：
     *   1. LLM 返回 has_text=true → 正常完成，返回文本响应
     *   2. step 达到 max_steps → 超限终止，返回 "max steps reached"
     *   3. LLM API 返回错误 → 传播错误码退出
     *   4. LLM 返回空响应（无 text 也无 tool_call） → break 后走超限路径
     */
    for (int step = 0; step < runtime->config.max_steps; ++step) {
        char *messages_json = NULL;
        char *tools_json = NULL;

        /*
         * Step ①: 构建消息上下文
         *
         * 从 storage 加载该会话的最近 100 条历史消息，
         * 每轮迭代都重新构建——因为上一轮可能新增了 tool_call 和 tool_result 消息。
         *
         * 构建逻辑由 cc_context_builder_build_messages 完成：
         *   1. 从 storage 加载最近 100 条消息
         *   2. 在最前面插入 system_prompt（role="system"）
         *   3. 还原 assistant 消息中的 tool_calls JSON（持久化时包装的）
         *   4. 还原 reasoning_content 字段（JSON 包装格式）
         *   5. 为 tool 消息添加 tool_call_id 字段
         *   6. 序列化为完整 JSON 字符串
         *
         * 为什么加载 100 条而不是全部：
         *   大部分 LLM 的上下文窗口有限（如 8K/32K/128K tokens），
         *   加载过多历史消息会超出窗口限制。100 条对于大多数对话
         *   来说已经足够覆盖完整的上下文。
         */
        rc = cc_context_builder_build_messages(
            runtime, session_id,
            runtime->config.system_prompt,
            &messages_json
        );
        if (rc.code != CC_OK) return rc;

        /*
         * Step ②: 构建工具列表 JSON Schema
         *
         * 从 tool_registry 获取所有已注册的工具，将这些工具的定义
         * （名称、描述、参数 schema）序列化为 OpenAI function-calling
         * 兼容的 tools JSON 数组。
         *
         * 输出格式示例：
         *   [{"type":"function","function":{"name":"get_weather",
         *     "description":"查询指定城市的天气",
         *     "parameters":{"type":"object","properties":{"city":
         *     {"type":"string","description":"城市名称"}},
         *     "required":["city"]}}}]
         *
         * 如果 tool_registry 中没有注册任何工具，返回 "[]"。
         * LLM 收到空 tools 列表后不会发起任何 tool call。
         */
        rc = cc_tool_registry_build_schema_json(
            runtime->tool_registry,
            &tools_json
        );
        if (rc.code != CC_OK) {
            free(messages_json);
            return rc;
        }

        /*
         * Step ③: 组装 LLM 请求
         *
         * 将 messages_json 和 tools_json 填入 cc_llm_chat_request_t 结构体。
         * 各参数的含义和取值原因：
         *
         *   - stream=0（非流式）：
         *     简化循环处理逻辑。流式模式下 tool_call 参数可能跨多个 chunk，
         *     需要实现增量 JSON 解析器来拼接完整的函数调用参数。
         *     代价：用户需要等待 LLM 完整回复后才看到结果，交互体验略差。
         *
         *   - max_tokens=4096：
         *     单次 LLM 回复的最大 token 数。4K 对于多数工具调用+简短回答足够。
         *     如果工具的 JSON 输出很大，LLM 可能被截断，此时需要增大此值。
         *
         *   - temperature=0.7：
         *     LLM 输出的"创造性"参数。0.7 是均衡值：既有一定的多样性不至于
         *     死板，又不会产生过于随机的 tool_call 参数导致工具执行失败。
         *     对于纯工具调用场景，可以降低到 0.0-0.3 以提高稳定性。
         *
         *   - thinking_mode：
         *     从 runtime 透传，由 LLM provider 决定如何使用。
         *     如 DeepSeek-R1 用它控制是否返回 reasoning_content。
         */
        cc_llm_chat_request_t request;
        memset(&request, 0, sizeof(request));
        request.messages_json = messages_json;
        request.tools_json = tools_json;
        request.model = runtime->config.model;
        request.stream = 0;
        request.max_tokens = 4096;
        request.temperature = 0.7;
        request.thinking_mode = cc_agent_runtime_get_thinking_mode(runtime);

        /* 发布 LLM 请求开始事件（供 UI 显示"思考中..."等状态） */
        if (runtime->event_bus) {
            cc_event_bus_publish(runtime->event_bus, "llm.request.started", "{}");
        }

        /*
         * Step ④: 调用大语言模型
         *
         * 通过 LLM provider 的虚函数表调用 chat 方法。
         * 这是一个阻塞调用——系统等待 LLM API 返回完整响应后才继续。
         * 响应内容分为两类：
         *   - has_text=true  → 普通文本回答（直接返回给用户）
         *   - has_tool_call=true → 工具调用请求（进入分支 A 处理）
         *
         * 注意：messages_json 和 tools_json 在 LLM 调用返回后立即释放。
         * 它们是本轮迭代的临时数据，下一轮迭代会重新从 storage 构建。
         * 这样做的好处是：LLM 响应中的 tool call 内容不需要手动解析后
         * 拼接到 messages_json 中，而是通过 storage 持久化后由下一轮的
         * context_builder 自动加载。
         */
        cc_llm_response_t response;
        memset(&response, 0, sizeof(response));
        rc = runtime->llm.vtable->chat(
            runtime->llm.self,
            &request,
            &response
        );

        free(messages_json);
        free(tools_json);

        /* 发布 LLM 响应接收事件 */
        if (runtime->event_bus) {
            cc_event_bus_publish(runtime->event_bus, "llm.response.received", "{}");
        }

        /* LLM 调用失败：释放响应资源，向上传播错误 */
        if (rc.code != CC_OK) {
            cc_llm_response_free(&response);
            return rc;
        }

        /*
         * ╔═════════════════════════════════════════════════════════╗
         * ║ 分支 A: LLM 请求调用工具 (has_tool_call == true)       ║
         * ╚═════════════════════════════════════════════════════════╝
         *
         * 当 LLM 认为需要借助外部工具才能回答用户问题时，
         * 会返回一个 tool_call，包含工具名称和 JSON 格式的参数。
         *
         * 处理流程概述：
         *   ⑤a 将 LLM 的 tool_calls + reasoning_content 包装为 assistant 消息
         *      存入 storage，供后续轮次还原上下文
         *   ⑤b 调用 cc_tool_executor_execute 执行工具
         *   ⑥  持久化 tool_call 记录（审计日志）
         *   ⑦  持久化 tool_result 记录（审计日志）
         *   ⑧  创建 CC_ROLE_TOOL 消息并持久化（工具返回值进入对话上下文）
         *   ⑨  释放本轮资源 → continue 回到 Step ①
         *
         * 为什么先存 assistant 消息再执行工具：
         *   这是"先记录意图，再执行动作"的原则。即使工具执行过程中崩溃，
         *   storage 中已经记录了 LLM 的意图（tool_calls），便于调试和恢复。
         */
        if (response.has_tool_call) {
            rc = cc_agent_runtime_execute_tool_step(
                runtime, session_id, &response.tool_call, response.reasoning_content);
            cc_llm_response_free(&response);
            if (rc.code != CC_OK) return rc;
            continue;

            cc_tool_result_t tool_result;
            memset(&tool_result, 0, sizeof(tool_result));

            /*
             * Step ⑤a: 先持久化 assistant 消息（含 tool_calls JSON）
             *
             * 这条消息的 content 存储格式：
             *   {
             *     "tool_calls": [
             *       {
             *         "id": "call_xxxx",        // LLM 返回的调用 ID
             *         "type": "function",
             *         "function": {
             *           "name": "get_weather",   // 工具名称
             *           "arguments": "{\"city\":\"北京\"}"  // JSON 字符串参数
             *         }
             *       }
             *     ],
             *     "reasoning_content": "..."     // 可选：LLM 的思维链
             *   }
             *
             * 为什么将 tool_calls 包装为 JSON 存入 content：
             *   cc_message_t 只有一个 content 字段（char*），而 tool_calls
             *   是一个数组结构。用 JSON 序列化后可以完整保留结构信息。
             *   context_builder 在加载时会检测 content 是否以 '{' 开头且
             *   消息角色为 assistant + tool_call_id 非空，从而按 tool_calls
             *   格式还原为 API 标准的 "content":null + "tool_calls":[...]。
             *
             * reasoning_content 的关联存储：
             *   如果 LLM 同时返回了 reasoning_content，它与 tool_calls 一起
             *   打包存入同一 JSON 对象中。这样 context_builder 在还原时能够
             *   同时还原 reasoning_content 字段给 LLM，保持推理的连续性。
             */
            {
                cc_json_value_t *tcs = cc_json_create_array();
                cc_json_value_t *tc = cc_json_create_object();
                cc_json_object_set(tc, "id", cc_json_create_string(
                    response.tool_call.id ? response.tool_call.id : ""));
                cc_json_object_set(tc, "type", cc_json_create_string("function"));
                cc_json_value_t *func = cc_json_create_object();
                cc_json_object_set(func, "name", cc_json_create_string(
                    response.tool_call.name ? response.tool_call.name : ""));
                cc_json_object_set(func, "arguments", cc_json_create_string(
                    response.tool_call.arguments_json ? response.tool_call.arguments_json : "{}"));
                cc_json_object_set(tc, "function", func);
                cc_json_array_append(tcs, tc);

                cc_json_value_t *wrapper = cc_json_create_object();
                cc_json_object_set(wrapper, "tool_calls", tcs);
                if (response.reasoning_content) {
                    cc_json_object_set(wrapper, "reasoning_content",
                        cc_json_create_string(response.reasoning_content));
                }

                char *tcs_json = cc_json_stringify(wrapper);
                cc_json_destroy(wrapper);

                cc_message_t *asst_msg = NULL;
                char *aid = generate_id();
                cc_message_create(aid, session_id, CC_ROLE_ASSISTANT,
                    tcs_json, response.tool_call.id, &asst_msg);
                free(aid);
                free(tcs_json);
                if (runtime->store.vtable && runtime->store.vtable->append_message) {
                    runtime->store.vtable->append_message(runtime->store.self, asst_msg);
                }
                cc_message_destroy(asst_msg);
            }

            /* Step ⑤b: 执行工具（内部完成查找+策略+事件发布） */
            cc_tool_executor_execute(runtime, session_id, &response.tool_call, &tool_result);

            /* Step ⑥: 持久化 tool_call 记录（审计用途） */
            if (runtime->store.vtable && runtime->store.vtable->append_tool_call) {
                runtime->store.vtable->append_tool_call(
                    runtime->store.self, session_id, &response.tool_call);
            }

            /* Step ⑦: 持久化 tool_result 记录（审计用途） */
            if (runtime->store.vtable && runtime->store.vtable->append_tool_result) {
                runtime->store.vtable->append_tool_result(
                    runtime->store.self, session_id,
                    response.tool_call.id, &tool_result);
            }

            /*
             * Step ⑧: 将工具执行结果作为 CC_ROLE_TOOL 消息持久化
             *
             * 这条消息的关键字段：
             *   - role: "tool"
             *   - tool_call_id: 与 LLM 原始 tool_call.id 相同，用于关联
             *   - content: 工具执行成功时 = tool_result.content（JSON 字符串）
             *              工具执行失败时 = tool_result.error（错误描述）
             *
             * 为什么失败时的 error 也填到 content 中：
             *   LLM 的标准 tool 消息格式中只有一个 content 字段，没有 error 字段。
             *   将错误描述放入 content，LLM 可以看到错误信息并尝试：
             *     - 调整参数后再次调用同一工具
             *     - 换用其他工具
             *     - 直接告知用户失败原因
             *
             * 下一轮循环中 cc_context_builder 加载消息时会包含这条 tool 消息，
             * LLM 看到工具结果后可以选择：
             *   - 再次发起 tool_call（多步骤推理链）
             *   - 基于工具结果给出最终自然语言回答
             */
            cc_message_t *tool_msg = NULL;
            char *tci = response.tool_call.id ? strdup(response.tool_call.id) : NULL;
            char *tid = generate_id();
            cc_message_create(tid, session_id, CC_ROLE_TOOL,
                tool_result.ok ? tool_result.content : tool_result.error, tci, &tool_msg);
            free(tid);
            free(tci);

            if (runtime->store.vtable && runtime->store.vtable->append_message) {
                runtime->store.vtable->append_message(runtime->store.self, tool_msg);
            }

            /* Step ⑨: 释放本轮资源
             *   - tool_msg: 消息对象（已持久化到 storage）
             *   - tool_result.content/error/metadata_json: 工具返回的字符串
             *   - response: LLM 响应（含 tool_call 字段）
             */
            cc_message_destroy(tool_msg);
            free(tool_result.content);
            free(tool_result.error);
            free(tool_result.metadata_json);
            cc_llm_response_free(&response);
            continue; /* ← 回到 for 循环顶部，LLM 在下一轮会看到工具结果 */
        }

        /*
         * ╔═════════════════════════════════════════════════════════╗
         * ║ 分支 B: LLM 返回纯文本回答 (has_text == true)          ║
         * ╚═════════════════════════════════════════════════════════╝
         *
         * 这是 Agent 的最终回复。LLM 综合了对话上下文和（可能的）多轮
         * 工具调用结果后，给出了面向用户的自然语言回答。
         *
         * 处理流程：
         *   1. 如有 reasoning_content，将其与 text 包装为 JSON 存入 storage
         *   2. 创建 CC_ROLE_ASSISTANT 消息，持久化到 storage
         *   3. 将纯文本（不含 reasoning_content）写入 *out_response
         *   4. 发布 "agent.finished" 事件
         *   5. return CC_OK → 主循环结束
         *
         * 为什么 out_response 只写入 text 而不含 reasoning_content：
         *   reasoning_content 是模型的"内心独白"，通常不需要展示给最终用户。
         *   如果用户需要查看推理过程（如调试场景），可以从 storage 中查询完整记录。
         */
        if (response.has_text) {
            rc = cc_agent_runtime_store_assistant_text(
                runtime, session_id, response.text, response.reasoning_content);
            if (rc.code != CC_OK) {
                cc_llm_response_free(&response);
                return rc;
            }
            *out_response = strdup(response.text ? response.text : "");
            cc_llm_response_free(&response);
            if (runtime->event_bus) {
                cc_event_bus_publish(runtime->event_bus, "agent.finished", "{}");
            }
            return cc_result_ok();

            cc_message_t *assistant_msg = NULL;
            char *aid = generate_id();

            /*
             * reasoning_content 存在时，用 JSON 包装存储以便 context_builder
             * 在后续请求中还原 reasoning_content 字段。
             *
             * 存储格式：{"text":"...", "reasoning_content":"..."}
             *
             * 为什么存入 content 字段而非创建独立字段：
             *   cc_message_t 只有一个 content 字段（char*），且消息会序列化
             *   为 JSON 数组。在 JSON 中将 text 和 reasoning_content 合并
             *   成一个 JSON 对象存入 content，context_builder 加载时可以
             *   通过 { 开头检测到这是包装格式并正确拆解。
             *
             * 如果 reasoning_content 为空：
             *   直接存储纯文本，减少一次 JSON 序列化/反序列化开销。
             */
            char *stored_content = NULL;
            if (response.reasoning_content) {
                cc_json_value_t *wrap = cc_json_create_object();
                cc_json_object_set(wrap, "text",
                    cc_json_create_string(response.text ? response.text : ""));
                cc_json_object_set(wrap, "reasoning_content",
                    cc_json_create_string(response.reasoning_content));
                stored_content = cc_json_stringify(wrap);
                cc_json_destroy(wrap);
            }

            cc_message_create(aid, session_id, CC_ROLE_ASSISTANT,
                stored_content ? stored_content : response.text, NULL, &assistant_msg);
            free(aid);
            free(stored_content);

            /* Step ⑧: 持久化 assistant 消息到 storage */
            if (runtime->store.vtable && runtime->store.vtable->append_message) {
                runtime->store.vtable->append_message(runtime->store.self, assistant_msg);
            }
            cc_message_destroy(assistant_msg);

            /* Step ⑨: 将最终回复写入输出参数 */
            *out_response = strdup(response.text);
            cc_llm_response_free(&response);

            /* Step ⑩: 发布 Agent 完成事件（正常的文本回复结束） */
            if (runtime->event_bus) {
                cc_event_bus_publish(runtime->event_bus, "agent.finished", "{}");
            }

            /* Step ⑪: 正常退出主循环，返回响应 */
            return cc_result_ok();
        }

        /*
         * 既没有 text 也没有 tool_call —— LLM 返回了空响应
         *
         * 可能原因：
         *   - LLM API 可以访问但返回了空内容（如模型限制触发了空回复）
         *   - 网络层正常但模型服务内部异常导致空响应
         *   - 某些模型的 finish_reason 为 "stop" 但无实际内容
         *
         * 处理方式：释放响应资源，跳出 for 循环。
         * 后续将落入 max_steps_reached 处理逻辑，返回预设提示文本。
         */
        cc_llm_response_free(&response);
        break;
    }

    /*
     * ═══════════════════════════════════════════════════════════════
     * for 循环耗尽（step >= max_steps）— 超限处理
     * ═══════════════════════════════════════════════════════════════
     *
     * 到达此处意味着 Agent 在 max_steps 轮迭代内始终没有给出最终文本回复，
     * 可能原因：
     *   - 工具调用链过于复杂，需要更多步骤（增大 max_steps）
     *   - LLM 陷入了循环调用同一个工具（检查工具描述和策略）
     *   - max_steps 配得太小，不适合当前任务复杂度
     *   - 某次 LLM 调用返回了空响应（无 text 也无 tool_call）
     *
     * 此时返回一个固定提示文本，防止客户端无限等待。
     * 同时发布 agent.finished 事件携带 reason="max_steps_reached"，
     * 让监听方（UI/日志）能够区分正常完成和超限终止。
     */
    *out_response = strdup("Agent stopped: max steps reached.");
    if (runtime->event_bus) {
        cc_event_bus_publish(runtime->event_bus, "agent.finished",
            "{\"reason\":\"max_steps_reached\"}");
    }
    return cc_result_ok();
}
