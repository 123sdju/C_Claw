/**
 * 学习导读：cclaw/core/src/app/cc_runtime_builder.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里定义 Agent 运行时的数据模型、主循环和通用工具，阅读时重点看所有权、错误返回和 ReAct 数据流。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/app/cc_runtime_builder.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_tool_registry.h"

#include <stdlib.h>
#include <string.h>

/**
 * cc_runtime_builder — runtime 组合根，持有构建过程中创建且需要统一销毁的资源。
 *
 * builder 是 main.c 与核心运行时之间的资源容器。除了 features 是借用的静态表，
 * 其他字段都是 builder 在创建流程中获得并在 destroy 中释放或转交释放的资源。
 */
struct cc_runtime_builder {
    /** 借用的应用能力表；生命周期由具体应用的静态存储保证。 */
    const cc_runtime_feature_set_t *features;
    /** builder 拥有的日志器，用于启动/关闭和 runtime 诊断。 */
    cc_logger_t *logger;
    /** builder 拥有的事件总线，runtime 和 gateway 通过它发布/订阅流式事件。 */
    cc_event_bus_t *event_bus;
    /** 默认文件系统端口；底层 self 由 builder destroy 调用 vtable->destroy。 */
    cc_filesystem_t fs;
    /** 会话存储端口；由 feature 工厂创建，builder 负责销毁底层 self。 */
    cc_session_store_t store;
    /** LLM provider 端口；由 provider 工厂创建，builder 负责销毁底层 self。 */
    cc_llm_provider_t llm;
    /** 工具策略引擎端口；由 feature 工厂创建，builder 负责销毁底层 self。 */
    cc_policy_engine_t policy;
    /** 可选长期记忆存储；self 为空表示当前 profile 未启用或创建失败后降级。 */
    cc_memory_store_t memory_store;
    /** 可选 sandbox；由 feature 工厂创建，builder 负责销毁底层 self。 */
    cc_sandbox_t sandbox;
    /** builder 拥有的工具注册表；工具成功注册后由 registry 接管。 */
    cc_tool_registry_t *tool_registry;
    /** builder 拥有的 Agent runtime；销毁时先释放 runtime，再释放其借用依赖。 */
    cc_agent_runtime_t *runtime;
    /** 为 runtime 构造出的系统提示词副本；runtime 创建时会再深拷贝。 */
    char *system_prompt;
    /** 插件加载器返回的不透明状态；由 features->destroy_plugins 释放。 */
    void *plugin_state;
};

/**
 * config_tool_enabled — 判断某个内建工具是否被配置允许注册。
 *
 * enabled_tools 为空时表示“注册当前 profile 中所有编译进来的工具”；否则工具
 * name 或 alias 任一命中即可注册。函数只读取配置，不保存字符串。
 *
 * @param config 借用的只读配置；可为 NULL，此时按全部启用处理。
 * @param name 工具正式名借用字符串；可为 NULL。
 * @param alias 工具别名借用字符串；可为 NULL。
 * @return 1 表示允许注册，0 表示被 enabled_tools 过滤掉。
 */
static int config_tool_enabled(const cc_config_t *config, const char *name, const char *alias)
{
    if (!config || !config->enabled_tools || config->enabled_tools_count == 0) return 1;
    for (size_t i = 0; i < config->enabled_tools_count; i++) {
        const char *enabled = config->enabled_tools[i];
        if (!enabled) continue;
        if (name && strcmp(enabled, name) == 0) return 1;
        if (alias && strcmp(enabled, alias) == 0) return 1;
    }
    return 0;
}

/**
 * destroy_tool_if_unowned — 清理尚未转交给 registry 的临时工具对象。
 *
 * 工具工厂创建的 cc_tool_t 在 registry_add 成功前仍由 builder 负责。注册失败
 * 或中途错误时调用该函数，避免 tool.self 泄漏；清理后把结构体清零，防止重复释放。
 *
 * @param tool 临时工具对象；NULL 时函数直接返回。
 */
static void destroy_tool_if_unowned(cc_tool_t *tool)
{
    if (tool && tool->vtable && tool->vtable->destroy && tool->self) {
        tool->vtable->destroy(tool->self);
    }
    if (tool) memset(tool, 0, sizeof(*tool));
}

/**
 * register_created_tool — 将刚创建的工具交给工具注册表接管。
 *
 * cc_tool_registry_add 按值接收 tool；成功后 registry 拥有底层 self，临时 tool
 * 必须清零以免 builder 的错误路径再次销毁。失败时仍由本函数销毁临时工具。
 *
 * @param registry 借用的工具注册表；必须已创建且尚未 freeze。
 * @param tool 工具工厂输出的临时工具对象；成功或失败后都会被清零。
 * @return CC_OK 表示注册成功；失败返回 registry_add 的错误码。
 */
static cc_result_t register_created_tool(cc_tool_registry_t *registry, cc_tool_t *tool)
{
    cc_result_t rc = cc_tool_registry_add(registry, *tool);
    if (rc.code != CC_OK) {
        destroy_tool_if_unowned(tool);
        return rc;
    }
    memset(tool, 0, sizeof(*tool));
    return cc_result_ok();
}

/**
 * destroy_sandbox_if_owned — 释放 builder 持有的 sandbox 端口实现。
 *
 * sandbox 是值类型端口，是否真的拥有资源由 vtable/self 决定。销毁后清零，
 * 这样失败路径和正常 destroy 共享代码时不会重复释放。
 *
 * @param sandbox builder 字段地址；NULL 时函数直接返回。
 */
static void destroy_sandbox_if_owned(cc_sandbox_t *sandbox)
{
    if (sandbox && sandbox->vtable && sandbox->vtable->destroy && sandbox->self) {
        sandbox->vtable->destroy(sandbox->self);
    }
    if (sandbox) memset(sandbox, 0, sizeof(*sandbox));
}

/**
 * create_llm — 按 config.provider 从 feature set 中选择并创建 LLM provider。
 *
 * features 中的 provider 描述符可能因为编译期开关被标记为未启用。只有名称匹配、
 * compiled 为真且 create 回调存在时才会调用工厂；找不到时返回 INVALID_ARGUMENT。
 *
 * @param config 借用的只读配置；读取 provider 选择和 provider 私有参数。
 * @param features 借用的静态能力表；必须包含 provider 描述符数组。
 * @param out_llm 输出参数；成功时获得 provider 端口值，失败时保持清零。
 * @return CC_OK 表示 provider 创建成功；否则返回 unknown/disabled provider 错误。
 */
static cc_result_t create_llm(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_llm_provider_t *out_llm
)
{
    memset(out_llm, 0, sizeof(*out_llm));
    const char *provider = config->provider ? config->provider : "";
    for (size_t i = 0; i < features->llm_provider_count; i++) {
        const cc_llm_provider_descriptor_t *desc = &features->llm_providers[i];
        if (!desc->compiled || !desc->name || !desc->create) continue;
        if (strcmp(provider, desc->name) == 0) {
            return desc->create(config, out_llm);
        }
    }
    return cc_result_errf(CC_ERR_INVALID_ARGUMENT, "Unknown or disabled LLM provider: %s",
        provider[0] ? provider : "(none)");
}

/**
 * build_tools — 创建工具注册表并注册当前 profile 启用的内建工具和插件工具。
 *
 * 先创建 registry，再用 cc_runtime_tool_factory_ctx_t 把配置、文件系统、memory store
 * 和 sandbox 工厂传给工具工厂。内建工具注册完成后加载插件工具，最后 freeze registry，
 * 防止 runtime 运行期再修改工具集合。
 *
 * @param builder 正在组装的 builder；函数会写入 tool_registry 和 plugin_state。
 * @param config 借用的只读配置，用于工具过滤和插件加载。
 * @return CC_OK 表示工具集合已冻结；失败时调用方走 builder 的统一失败清理路径。
 */
static cc_result_t build_tools(cc_runtime_builder_t *builder, const cc_config_t *config)
{
    cc_result_t rc = cc_tool_registry_create(&builder->tool_registry);
    if (rc.code != CC_OK) return rc;

    cc_runtime_tool_factory_ctx_t ctx = {
        .config = config,
        .filesystem = builder->fs,
        .memory_store = builder->memory_store.self ? &builder->memory_store : NULL,
        .create_sandbox = builder->features->create_sandbox
    };

    for (size_t i = 0; i < builder->features->tool_count; i++) {
        const cc_tool_descriptor_t *desc = &builder->features->tools[i];
        if (!desc->compiled || !desc->create) continue;
        if (!config_tool_enabled(config, desc->name, desc->alias)) continue;

        cc_tool_t tool = {0};
        rc = desc->create(&ctx, &tool);
        if (rc.code != CC_OK) return rc;
        if (!tool.vtable) continue;
        rc = register_created_tool(builder->tool_registry, &tool);
        if (rc.code != CC_OK) return rc;
    }

    if (builder->features->load_plugins) {
        rc = builder->features->load_plugins(
            config, builder->tool_registry, &builder->plugin_state);
        if (rc.code != CC_OK) return rc;
    }

    return cc_tool_registry_freeze(builder->tool_registry);
}

/**
 * cc_runtime_builder_create — 根据配置和 feature set 组装 logger、store、tools、LLM provider 和 Agent runtime。
 *
 * 该函数是 C-Claw 启动期的组合根：它校验 feature set，创建平台端口和存储，
 * 选择 LLM provider，注册工具，生成 system prompt，并把这些依赖注入
 * cc_agent_runtime_create。任一步失败都会跳到 fail，复用 destroy 做部分资源清理。
 *
 * @param config 借用的只读配置；builder 只复制 system prompt 等需要长期保存的字符串。
 * @param features 借用的静态能力表；生命周期必须覆盖 builder。
 * @param out_builder 输出参数；成功时获得新 builder，失败时写回 NULL。
 * @return CC_OK 表示 runtime 可用；失败返回具体初始化错误。
 */
cc_result_t cc_runtime_builder_create(
    const cc_config_t *config,
    const cc_runtime_feature_set_t *features,
    cc_runtime_builder_t **out_builder
)
{
    if (!config || !features || !out_builder) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null runtime builder argument");
    }
    if (!features->create_session_store || !features->create_policy_engine) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Runtime feature set is incomplete");
    }

    *out_builder = NULL;
    cc_runtime_builder_t *builder = calloc(1, sizeof(*builder));
    if (!builder) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create runtime builder");
    builder->features = features;

    cc_result_t rc = cc_logger_create("c-claw", CC_LOG_INFO, &builder->logger);
    if (rc.code != CC_OK) goto fail;
    cc_logger_log(builder->logger, CC_LOG_INFO, "c-claw starting...");

    rc = cc_event_bus_create(&builder->event_bus);
    if (rc.code != CC_OK) goto fail;
    rc = cc_filesystem_get_default(&builder->fs);
    if (rc.code != CC_OK) goto fail;
    if (config->data_dir && config->data_dir[0]) {
        rc = builder->fs.vtable->make_dir(builder->fs.self, config->data_dir);
        if (rc.code != CC_OK) goto fail;
    }
    if (config->workspace_path && config->workspace_path[0]) {
        rc = builder->fs.vtable->make_dir(builder->fs.self, config->workspace_path);
        if (rc.code != CC_OK) goto fail;
    }

    rc = features->create_session_store(config, &builder->store);
    if (rc.code != CC_OK) goto fail;
    rc = create_llm(config, features, &builder->llm);
    if (rc.code != CC_OK) goto fail;
    rc = features->create_policy_engine(config, &builder->policy);
    if (rc.code != CC_OK) goto fail;
    if (features->create_sandbox) {
        rc = features->create_sandbox(config, &builder->sandbox);
        if (rc.code != CC_OK) goto fail;
    }
    if (features->create_memory_store) {
        rc = features->create_memory_store(config, &builder->memory_store);
        if (rc.code != CC_OK) {
            cc_result_free(&rc);
            memset(&builder->memory_store, 0, sizeof(builder->memory_store));
        }
    }

    rc = build_tools(builder, config);
    if (rc.code != CC_OK) goto fail;
    rc = cc_config_build_system_prompt(config, &builder->system_prompt);
    if (rc.code != CC_OK || !builder->system_prompt) {
        cc_result_free(&rc);
        builder->system_prompt = strdup("You are a helpful AI assistant. Use tools to help the user.");
    }

    cc_agent_runtime_config_t runtime_config = {0};
    runtime_config.max_steps = config->max_steps ? config->max_steps : 10;
    runtime_config.context_window_tokens = config->context_window_tokens;
    runtime_config.context_compress_threshold = config->context_compress_threshold;
    runtime_config.context_keep_recent = config->context_keep_recent;
    runtime_config.system_prompt = builder->system_prompt;
    runtime_config.workspace_dir = config->workspace_path;
    runtime_config.model = config->model;

    cc_agent_runtime_deps_t deps = {0};
    deps.llm = builder->llm;
    deps.tool_registry = builder->tool_registry;
    deps.store = builder->store;
    deps.policy = builder->policy;
    deps.sandbox = builder->sandbox;
    deps.event_bus = builder->event_bus;
    deps.logger = builder->logger;
    deps.memory_store = builder->memory_store.self ? &builder->memory_store : NULL;

    cc_agent_runtime_options_t options = {0};
    options.config = runtime_config;
    options.thinking_mode = config->thinking_mode;
    rc = cc_agent_runtime_create(&deps, &options, &builder->runtime);
    if (rc.code != CC_OK) goto fail;

    *out_builder = builder;
    return cc_result_ok();

fail:
    cc_runtime_builder_destroy(builder);
    return rc;
}

/**
 * cc_runtime_builder_runtime — 返回 builder 持有的 runtime 借用指针，调用方不能释放该指针。
 *
 * @param builder 借用的 builder；可为 NULL。
 * @return builder 内部 runtime 的借用指针；builder 为 NULL 时返回 NULL。
 */
cc_agent_runtime_t *cc_runtime_builder_runtime(cc_runtime_builder_t *builder)
{
    return builder ? builder->runtime : NULL;
}

/**
 * cc_runtime_builder_logger — 返回 builder 持有的 logger 借用指针。
 *
 * gateway 可用该 logger 做额外诊断，但不能销毁它；logger 生命周期由 builder 控制。
 *
 * @param builder 借用的 builder；可为 NULL。
 * @return builder 内部 logger 的借用指针；builder 为 NULL 时返回 NULL。
 */
cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder)
{
    return builder ? builder->logger : NULL;
}

/**
 * cc_runtime_builder_destroy — 按所有权顺序销毁 builder 创建的 runtime、工具、store、provider 和日志资源。
 *
 * 销毁顺序与依赖关系相反：先释放 runtime 和工具注册表，再释放插件状态、
 * stores、provider、policy、sandbox、平台端口和日志/event bus。传入 NULL 安全。
 *
 * @param builder 要释放的 builder；函数取得并销毁该对象所有权。
 */
void cc_runtime_builder_destroy(cc_runtime_builder_t *builder)
{
    if (!builder) return;
    if (builder->logger) cc_logger_log(builder->logger, CC_LOG_INFO, "Shutting down...");
    cc_agent_runtime_destroy(builder->runtime);
    cc_tool_registry_destroy(builder->tool_registry);
    if (builder->features && builder->features->destroy_plugins) {
        builder->features->destroy_plugins(builder->plugin_state);
    }
    if (builder->memory_store.self) cc_memory_store_destroy(&builder->memory_store);
    if (builder->store.vtable && builder->store.vtable->destroy) {
        builder->store.vtable->destroy(builder->store.self);
    }
    if (builder->llm.vtable && builder->llm.vtable->destroy) {
        builder->llm.vtable->destroy(builder->llm.self);
    }
    if (builder->policy.vtable && builder->policy.vtable->destroy) {
        builder->policy.vtable->destroy(builder->policy.self);
    }
    destroy_sandbox_if_owned(&builder->sandbox);
    if (builder->fs.vtable && builder->fs.vtable->destroy) {
        builder->fs.vtable->destroy(builder->fs.self);
    }
    cc_event_bus_destroy(builder->event_bus);
    cc_logger_destroy(builder->logger);
    free(builder->system_prompt);
    free(builder);
}
