/**
 * 学习导读：cclaw/core/include/cc/app/cc_agent_runtime.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

/**
 * cc_agent_runtime.h — Agent 运行时核心模块
 *
 * @file    cc/app/cc_agent_runtime.h
 * @brief   定义 Agent 运行时的配置、结构体和核心接口。
 *
 * cc_agent_runtime_t 是整个应用的核心编排器。它将 LLM Provider、
 * 工具注册表、会话存储、策略引擎和沙箱整合在一起，实现
 * "感知-决策-执行" 的 Agent 循环（也称为 ReAct 循环）。
 *
 * ─── 接口契约 ─────────────────────────────────────────────────────────
 *
 *   - 通过 cc_agent_runtime_create() 创建（依赖注入方式）
 *   - cc_agent_runtime_handle_message() 是主入口，每收到一条用户消息
 *     就启动完整的 Agent 循环
 *   - 运行时只拥有自己复制的配置字符串，不拥有注入组件的底层资源
 *   - event_bus、logger、memory_store 和审批回调通过 deps 统一注入
 *   - thinking_mode 通过 options 初始化，后续可用 setter/getter 线程安全访问
 *
 * ─── Agent 循环流程 ──────────────────────────────────────────────────
 *
 *   1. 从 Store 加载历史消息
 *   2. 拼接 system_prompt + 历史消息 + 当前用户输入 → LLM 请求
 *   3. 调用 LLM.chat() 获取回复
 *   4. 如果回复是文本且 finished → 返回文本给用户
 *   5. 如果回复是工具调用 → 从 ToolRegistry 查找工具 → 执行
 *      → 将结果附加到消息历史 → 回到步骤 2
 *   6. 超过 max_steps → 强制终止并返回当前结果
 *
 * ─── 依赖 ─────────────────────────────────────────────────────────────
 *
 *   依赖 cc/ports/ 下的多个端口接口和 cc/core/cc_result.h。
 *   event_bus、logger 和 memory_store 都是可选依赖，可传 NULL 表示禁用对应能力。
 */

#ifndef CC_AGENT_RUNTIME_H
#define CC_AGENT_RUNTIME_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_event_bus.h"
#include "cc/ports/cc_logger.h"
#include "cc/ports/cc_thread.h"
#include "cc/core/cc_stream_chunk.h"

/**
 * cc_agent_runtime_config_t — Agent 运行时行为配置
 *
 * 定义 Agent 的行为参数。与构造函数参数分开的原因是：
 * 这些是"配置"而非"依赖"——它们的值由 config.json 决定，
 * 可以随时调整而无需改变依赖注入关系。
 */
typedef struct cc_agent_runtime_config {
    int max_steps;       /**< 最大推理步数。每步 = 一次 LLM 调用 + 一次工具执行。
                          *   防止 Agent 陷入无限推理循环。
                          *   建议值：10（简单任务）- 30（复杂任务） */
    char *system_prompt;  /**< 系统提示词。定义 Agent 的角色、行为约束和工具使用策略。
                          *   会在每次 LLM 调用时作为首条消息注入。
                          *   典型例子："You are a helpful AI assistant..." */
    char *workspace_dir;  /**< 工作区目录。所有文件相关工具操作限定在此目录下。
                          *   由外部保证该目录存在且有读写权限 */
    char *model;          /**< 模型名称。用于日志和元数据，不直接参与 LLM 调用 */
    int max_tokens;       /**< 主对话单次回复最大 token 数。默认 4096。 */
    double temperature;   /**< 主对话生成温度。默认 0.7，0.0 表示最确定输出。 */
    int context_window_tokens; /**< LLM 上下文窗口 token 预算。
                          *   用于动态截断历史消息。0 表示不限制。
                          *   默认值：8192 */
    double context_compress_threshold; /**< 压缩触发阈值（0.0-1.0）。
                          *   0 表示禁用压缩，仅做硬截断。
                          *   默认值：0.8 */
    int context_keep_recent; /**< 压缩时保留最近 N 条原始消息。
                          *   默认值：20 */
    int summary_max_tokens; /**< 上下文摘要压缩请求最大 token 数。
                          *   默认值：1024 */
    double summary_temperature; /**< 上下文摘要压缩请求温度。
                          *   默认值：0.3 */
} cc_agent_runtime_config_t;

/**
 * cc_agent_runtime — Agent 主循环的不透明运行时对象。
 *
 * 对外只通过本头文件的函数访问。对象内部保存注入端口的浅拷贝、
 * 配置字符串副本、线程锁和当前会话执行状态；调用方用
 * cc_agent_runtime_destroy 释放。
 */
typedef struct cc_agent_runtime cc_agent_runtime_t;

/**
 * cc_agent_runtime_deps — 创建 runtime 时注入的外部依赖集合。
 *
 * 端口值字段（llm/store/policy/sandbox）会被 runtime 浅拷贝，底层 self
 * 的最终所有权仍由 runtime_builder 或调用方管理。指针字段（registry、
 * event_bus、logger、memory_store）均为借用指针，生命周期必须覆盖 runtime。
 */
typedef struct cc_agent_runtime_deps {
    /** 借用底层 self 的 LLM provider 端口值；runtime 不负责 destroy。 */
    cc_llm_provider_t llm;
    /** 借用的工具注册表；必须在 runtime 生命周期内保持有效且通常已 freeze。 */
    cc_tool_registry_t *tool_registry;
    /** 借用底层 self 的会话存储端口值；runtime 只调用其 vtable。 */
    cc_session_store_t store;
    /** 借用底层 self 的策略引擎端口值；用于工具执行前审批。 */
    cc_policy_engine_t policy;
    /** 借用底层 self 的 sandbox 端口值；可为空 vtable 表示未启用。 */
    cc_sandbox_t sandbox;
    /** 可选事件总线；为 NULL 时 runtime 只返回最终响应，不发布流事件。 */
    cc_event_bus_t *event_bus;
    /** 可选 logger；为 NULL 时跳过日志输出。 */
    cc_logger_t *logger;
    /** 可选长期记忆存储；为 NULL 时上下文构建不注入记忆。 */
    cc_memory_store_t *memory_store;
    /** 可选人工审批回调；policy 返回需审批时由 tool executor 调用。 */
    cc_tool_approval_fn approve_tool_call;
    /** 传给 approve_tool_call 的调用方上下文，runtime 只保存借用值。 */
    void *approval_user_data;
} cc_agent_runtime_deps_t;

/**
 * cc_agent_runtime_options — 创建 runtime 时传入的行为选项。
 *
 * options 与 deps 分离：deps 描述外部端口，options 描述纯行为参数。
 * create 会深拷贝 config 中需要长期持有的字符串，因此调用方可在创建后释放
 * 自己的配置对象。
 */
typedef struct cc_agent_runtime_options {
    /** 运行时配置；system_prompt/workspace_dir/model 会被深拷贝。 */
    cc_agent_runtime_config_t config;
    /** 初始思考流展示开关；运行后可通过 setter 线程安全修改。 */
    int thinking_mode;
} cc_agent_runtime_options_t;

/**
 * cc_agent_runtime_create — 创建 Agent 运行时实例
 *
 * 聚合所有注入的依赖，初始化 Agent 运行时。
 * 传入的 llm、store、policy、sandbox 为值拷贝（浅拷贝 vtable），
 * 调用方保有原实例的所有权。
 *
 * @param deps           依赖集合；值类型接口浅拷贝，指针类型所有权不转移
 * @param options        行为选项；config 中的字符串会被 runtime 深拷贝
 * @param out_runtime    输出：指向新运行时的指针（调用者负责 cc_agent_runtime_destroy）
 * @return               CC_OK 表示成功
 */
cc_result_t cc_agent_runtime_create(
    const cc_agent_runtime_deps_t *deps,
    const cc_agent_runtime_options_t *options,
    cc_agent_runtime_t **out_runtime
);

/**
 * cc_agent_runtime_destroy — 销毁 Agent 运行时实例
 *
 * 释放运行时自身分配的资源。
 * 注意：不释放 tool_registry 指针指向的对象（所有权在调用方），
 * 也不释放 event_bus、logger、memory_store、tool_registry 或各端口实现的 self。
 *
 * @param runtime  要销毁的运行时实例
 */
void cc_agent_runtime_destroy(cc_agent_runtime_t *runtime);

/**
 * cc_agent_runtime_set_thinking_mode — 线程安全地更新是否发布 thinking 流事件。
 *
 * @param runtime 借用的 runtime；NULL 时函数直接返回。
 * @param enabled 非 0 表示启用 thinking 事件，0 表示隐藏 thinking 增量。
 */
void cc_agent_runtime_set_thinking_mode(cc_agent_runtime_t *runtime, int enabled);

/**
 * cc_agent_runtime_get_thinking_mode — 线程安全地读取 thinking 流事件开关。
 *
 * @param runtime 借用的 runtime；NULL 时返回 0。
 * @return 非 0 表示启用 thinking 展示，0 表示关闭或 runtime 无效。
 */
int cc_agent_runtime_get_thinking_mode(cc_agent_runtime_t *runtime);

/**
 * cc_agent_runtime_set_tool_approval — 更新人工工具审批回调。
 *
 * runtime 只保存函数指针和 user_data，不取得 user_data 的所有权。该接口用于
 * CLI gateway 在运行时注入交互式审批逻辑。
 *
 * @param runtime 借用的 runtime；NULL 时函数直接返回。
 * @param approve_tool_call 可选审批回调；NULL 表示禁用人工审批。
 * @param user_data 原样传给审批回调的借用上下文。
 */
void cc_agent_runtime_set_tool_approval(
    cc_agent_runtime_t *runtime,
    cc_tool_approval_fn approve_tool_call,
    void *user_data
);

/**
 * cc_agent_runtime_event_bus — 返回 runtime 注入的事件总线借用指针，用于 gateway 订阅流式事件。
 *
 * @param runtime 借用的 runtime；可为 NULL。
 * @return 注入的 event_bus 借用指针；未注入或 runtime 为 NULL 时返回 NULL。
 */
cc_event_bus_t *cc_agent_runtime_event_bus(cc_agent_runtime_t *runtime);

/**
 * cc_agent_runtime_tool_registry — 返回 runtime 内部使用的工具注册表借用指针，主要用于测试和诊断。
 *
 * @param runtime 借用的 runtime；可为 NULL。
 * @return 工具注册表借用指针；runtime 为 NULL 时返回 NULL。
 */
cc_tool_registry_t *cc_agent_runtime_tool_registry(cc_agent_runtime_t *runtime);

/**
 * cc_agent_runtime_session_store — 返回 runtime 注入的会话存储接口副本，调用方只借用底层 self。
 *
 * @param runtime 借用的 runtime；可为 NULL。
 * @return runtime 内部 store 字段的地址；runtime 为 NULL 时返回 NULL。
 */
cc_session_store_t *cc_agent_runtime_session_store(cc_agent_runtime_t *runtime);

/**
 * cc_agent_runtime_supports_stream — 查询当前 LLM provider 是否实现 chat_stream，用于选择流式或同步路径。
 *
 * @param runtime 借用的 runtime；可为 NULL。
 * @return 非 0 表示 provider 支持流式输出，0 表示不支持或 runtime 无效。
 */
int cc_agent_runtime_supports_stream(cc_agent_runtime_t *runtime);

/**
 * cc_agent_runtime_create_session — 通过 runtime 的 session store 创建会话元数据，不触发 LLM 调用。
 *
 * @param runtime 借用的 runtime；不可为 NULL。
 * @param session_id 借用的会话 ID 字符串；store 会按自身约定复制或持久化。
 * @param workspace_dir 借用的工作目录字符串；用于写入会话元数据。
 * @return CC_OK 表示会话创建成功；失败返回 store 或参数校验错误。
 */
cc_result_t cc_agent_runtime_create_session(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *workspace_dir
);

/**
 * cc_agent_runtime_handle_message — 处理用户消息（Agent 循环主入口）
 *
 * 这是 Agent 运行时的核心函数。接收用户输入，启动完整的 Agent 循环：
 * 加载上下文 → 调用 LLM → 执行工具 → 循环迭代 → 返回最终回答。
 *
 * 内部逻辑：
 *   1. 从 store 加载会话历史
 *   2. 构建完整的 LLM 上下文（system_prompt + 历史 + 新消息）
 *   3. 调用 LLM.chat() 获取回复
 *   4. 如果回复是文本且 finished=true：将回复保存到 store，返回文本
 *   5. 如果回复是工具调用：通过 policy 评估 → 执行工具 → 保存结果 → goto 3
 *   6. 如果超过 max_steps：强制返回当前文本 + 步数超限提示
 *
 * @param runtime      运行时实例（不可为 NULL）
 * @param session_id   目标会话 ID（不可为 NULL）
 * @param user_input   用户输入文本（不可为 NULL）
 * @param out_response 输出：Agent 的最终文本回复（调用者负责 free）
 * @return             CC_OK 表示成功
 */
cc_result_t cc_agent_runtime_handle_message(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
);

/**
 * cc_agent_runtime_handle_message_stream — 流式处理用户消息
 *
 * 与 cc_agent_runtime_handle_message 功能等效，但使用 LLM 流式接口
 * （chat_stream）逐步获取响应。在响应生成过程中，通过事件总线实时发布：
 *
 *   - stream.text → 用户可见的文本增量
 *   - stream.thinking → 模型思考推理过程
 *   - stream.tool.start → 工具调用开始
 *   - stream.tool.delta → 工具调用参数增量
 *   - stream.tool.end → 工具调用执行完毕
 *   - stream.finished → 流结束
 *
 * CLI / Web Gateway 通过订阅这些事件实现实时展示效果。
 *
 * 如果 LLM Provider 不支持 chat_stream：
 *   自动降级为同步 chat 模式，事件总线发布最终的完整文本。
 *
 * @param runtime      运行时实例（不可为 NULL）
 * @param session_id   目标会话 ID（不可为 NULL）
 * @param user_input   用户输入文本（不可为 NULL）
 * @param out_response 输出：Agent 的最终文本回复（调用者负责 free）
 * @return             CC_OK 表示成功
 */
cc_result_t cc_agent_runtime_handle_message_stream(
    cc_agent_runtime_t *runtime,
    const char *session_id,
    const char *user_input,
    char **out_response
);

#endif
