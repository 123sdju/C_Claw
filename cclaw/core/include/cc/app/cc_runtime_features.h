/**
 * 学习导读：cclaw/core/include/cc/app/cc_runtime_features.h
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#ifndef CC_RUNTIME_FEATURES_H
#define CC_RUNTIME_FEATURES_H

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_llm_provider.h"
#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_policy_engine.h"
#include "cc/ports/cc_sandbox.h"
#include "cc/ports/cc_session_store.h"
#include "cc/ports/cc_tool.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_config.h"

/**
 * cc_runtime_llm_create_fn — LLM provider 工厂函数类型。
 *
 * 工厂从配置中读取 provider 所需的 base_url、api_key、model 等字段，
 * 成功后把可按值拷贝的 cc_llm_provider_t 写入 out_provider。调用方后续
 * 通过 provider.vtable->destroy(provider.self) 释放实现私有状态。
 *
 * @param config 只读配置；工厂只借用该指针，不保存配置对象本身。
 * @param out_provider 输出参数；成功时获得 provider 值对象。
 * @return CC_OK 表示创建成功；否则返回具体错误码，out_provider 不可使用。
 */
typedef cc_result_t (*cc_runtime_llm_create_fn)(
    const cc_config_t *config,
    cc_llm_provider_t *out_provider
);

/**
 * cc_runtime_session_store_create_fn — 会话存储工厂函数类型。
 *
 * 会话存储负责保存 session 元数据、用户/助手消息以及工具调用结果。
 * 成功后 out_store 持有 store.self，runtime_builder 在销毁阶段调用 vtable
 * 的 destroy 释放该实现。
 *
 * @param config 只读配置，常用于选择 data_dir 或 SQLite/JSON 文件路径。
 * @param out_store 输出参数；成功时写入可用的会话存储端口。
 * @return CC_OK 表示创建成功；否则返回错误码和错误消息。
 */
typedef cc_result_t (*cc_runtime_session_store_create_fn)(
    const cc_config_t *config,
    cc_session_store_t *out_store
);

/**
 * cc_runtime_memory_store_create_fn — 长期记忆存储工厂函数类型。
 *
 * memory store 是可选能力，创建失败时某些 profile 会降级为无长期记忆。
 * 成功写入的 out_store 由 runtime_builder 拥有并负责销毁。
 *
 * @param config 只读配置，提供 data_dir、profile 等存储选择信息。
 * @param out_store 输出参数；成功时写入 memory store 端口值。
 * @return CC_OK 表示可用；失败返回错误码，调用方可决定是否降级。
 */
typedef cc_result_t (*cc_runtime_memory_store_create_fn)(
    const cc_config_t *config,
    cc_memory_store_t *out_store
);

/**
 * cc_runtime_policy_create_fn — 工具调用策略引擎工厂函数类型。
 *
 * policy engine 在工具执行前判断是否允许、拒绝或需要人工审批。
 * 成功后的 policy.self 由 runtime_builder 持有到销毁阶段。
 *
 * @param config 只读配置，包含安全策略和审批模式。
 * @param out_policy 输出参数；成功时写入策略端口值。
 * @return CC_OK 表示创建成功；否则 runtime_builder 会中止启动。
 */
typedef cc_result_t (*cc_runtime_policy_create_fn)(
    const cc_config_t *config,
    cc_policy_engine_t *out_policy
);

/**
 * cc_runtime_sandbox_create_fn — sandbox 工厂函数类型。
 *
 * sandbox 用于隔离 shell/file 等高风险工具。它既可作为 runtime 的默认
 * sandbox，也可由工具工厂按需创建独立实例。
 *
 * @param config 只读配置，描述 sandbox 后端与工作目录限制。
 * @param out_sandbox 输出参数；成功时写入 sandbox 端口值。
 * @return CC_OK 表示创建成功；失败返回错误码，调用方决定是否中止。
 */
typedef cc_result_t (*cc_runtime_sandbox_create_fn)(
    const cc_config_t *config,
    cc_sandbox_t *out_sandbox
);

/**
 * cc_runtime_tool_factory_ctx — 工具工厂上下文，向 tool create 回调传递配置、文件系统、沙箱和服务依赖。
 *
 * 该结构体本身不拥有任何依赖。runtime_builder 在调用工具工厂时临时构造它，
 * 工具可以复制需要长期保存的字符串或端口值，但不能保存 ctx 指针本身。
 */
typedef struct cc_runtime_tool_factory_ctx {
    /** 借用的只读配置；工具工厂可读取工具参数、工作目录和 profile 信息。 */
    const cc_config_t *config;
    /** 文件系统端口值的浅拷贝；底层 self 仍由 runtime_builder 统一销毁。 */
    cc_filesystem_t filesystem;
    /** 可选长期记忆存储；为 NULL 表示当前 profile 未启用 memory store。 */
    cc_memory_store_t *memory_store;
    /** 可选 sandbox 工厂；工具需要独立 sandbox 时调用，创建出的实例由工具负责释放。 */
    cc_runtime_sandbox_create_fn create_sandbox;
} cc_runtime_tool_factory_ctx_t;

/**
 * cc_runtime_tool_create_fn — 内建工具工厂函数类型。
 *
 * 工厂根据 ctx 创建一个 cc_tool_t 值对象。创建成功后 runtime_builder 会把该
 * tool 转交给 registry；注册失败时 builder 会调用 tool 的 destroy 清理临时对象。
 *
 * @param ctx 借用的工具工厂上下文；不能保存该指针。
 * @param out_tool 输出参数；成功时写入工具端口值。
 * @return CC_OK 表示创建成功；失败返回错误码，out_tool 不应注册。
 */
typedef cc_result_t (*cc_runtime_tool_create_fn)(
    const cc_runtime_tool_factory_ctx_t *ctx,
    cc_tool_t *out_tool
);

/**
 * cc_runtime_plugin_load_fn — profile 的插件加载入口。
 *
 * 该回调把外部插件进程暴露的工具注册到 registry。若需要保存插件管理器状态，
 * 可通过 out_state 返回不透明指针，后续由 destroy_plugins 回调释放。
 *
 * @param config 借用的只读配置，包含插件路径和启用列表。
 * @param registry 借用的工具注册表；回调可向其中添加工具。
 * @param out_state 输出插件状态；无状态实现可写入 NULL。
 * @return CC_OK 表示插件加载完成；失败会阻止 runtime 启动。
 */
typedef cc_result_t (*cc_runtime_plugin_load_fn)(
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    void **out_state
);

/**
 * cc_runtime_plugin_destroy_fn — 释放 load_plugins 返回的不透明插件状态。
 *
 * @param state load_plugins 写入的状态指针；无状态或未加载插件时可能为 NULL。
 */
typedef void (*cc_runtime_plugin_destroy_fn)(void *state);

/**
 * cc_llm_provider_descriptor — 注册描述符，把名称和工厂函数配对，供 runtime_builder 统一枚举和创建。
 *
 * 描述符数组通常是应用层的静态常量表，runtime_builder 只借用其中的字符串和函数指针。
 */
typedef struct cc_llm_provider_descriptor {
    /** 配置文件中的 provider 名称，例如 "openai"、"anthropic"、"ollama"。 */
    const char *name;
    /** 编译期开关结果；0 表示该 provider 在当前 profile 中不可用。 */
    int compiled;
    /** 创建该 provider 的工厂函数；compiled 为 1 时应非 NULL。 */
    cc_runtime_llm_create_fn create;
} cc_llm_provider_descriptor_t;

/**
 * cc_tool_descriptor — 注册描述符，把名称和工厂函数配对，供 runtime_builder 统一枚举和创建。
 *
 * name 和 alias 都是静态借用字符串，用于匹配 config.enabled_tools。
 */
typedef struct cc_tool_descriptor {
    /** 工具正式名称，通常等于 registry 中暴露给 LLM 的工具名。 */
    const char *name;
    /** 兼容配置用的短别名；可为 NULL。 */
    const char *alias;
    /** 编译期开关结果；0 表示当前 profile 不注册该工具。 */
    int compiled;
    /** 工具工厂；成功创建的工具由 registry 接管所有权。 */
    cc_runtime_tool_create_fn create;
} cc_tool_descriptor_t;

/**
 * cc_runtime_feature_set — 应用 profile 的能力描述表，runtime_builder 通过它创建 LLM、store、sandbox 和 tools。
 *
 * 这是应用层和核心 runtime_builder 之间的组合边界：核心层不直接知道
 * POSIX/Windows/ESP32 编译了哪些 provider、tool 或插件能力，只读取这张表。
 */
typedef struct cc_runtime_feature_set {
    /** 静态 LLM provider 描述符数组；长度由 llm_provider_count 给出。 */
    const cc_llm_provider_descriptor_t *llm_providers;
    /** llm_providers 的元素数量。 */
    size_t llm_provider_count;

    /** 静态工具描述符数组；runtime_builder 会过滤 compiled 和 enabled_tools。 */
    const cc_tool_descriptor_t *tools;
    /** tools 的元素数量。 */
    size_t tool_count;

    /** 必需：创建会话存储；缺失时 runtime_builder_create 直接失败。 */
    cc_runtime_session_store_create_fn create_session_store;
    /** 可选：创建长期记忆存储；失败或缺失时可降级为无记忆能力。 */
    cc_runtime_memory_store_create_fn create_memory_store;
    /** 必需：创建工具策略引擎；控制工具调用的允许/拒绝/审批。 */
    cc_runtime_policy_create_fn create_policy_engine;
    /** 可选：创建 sandbox；没有 sandbox 的 profile 应确保工具自身安全。 */
    cc_runtime_sandbox_create_fn create_sandbox;

    /** 可选：加载插件工具；通常由 CLI 应用层实现。 */
    cc_runtime_plugin_load_fn load_plugins;
    /** 可选：释放 load_plugins 返回的 plugin_state。 */
    cc_runtime_plugin_destroy_fn destroy_plugins;
} cc_runtime_feature_set_t;

#endif
