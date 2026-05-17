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

struct cc_runtime_builder {
    const cc_runtime_feature_set_t *features;
    cc_logger_t *logger;
    cc_event_bus_t *event_bus;
    cc_filesystem_t fs;
    cc_session_store_t store;
    cc_llm_provider_t llm;
    cc_policy_engine_t policy;
    cc_memory_store_t memory_store;
    cc_sandbox_t sandbox;
    cc_tool_registry_t *tool_registry;
    cc_agent_runtime_t *runtime;
    char *system_prompt;
    void *plugin_state;
};

/* 学习注释：config_tool_enabled 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：destroy_tool_if_unowned 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void destroy_tool_if_unowned(cc_tool_t *tool)
{
    if (tool && tool->vtable && tool->vtable->destroy && tool->self) {
        tool->vtable->destroy(tool->self);
    }
    if (tool) memset(tool, 0, sizeof(*tool));
}

/* 学习注释：register_created_tool 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：destroy_sandbox_if_owned 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void destroy_sandbox_if_owned(cc_sandbox_t *sandbox)
{
    if (sandbox && sandbox->vtable && sandbox->vtable->destroy && sandbox->self) {
        sandbox->vtable->destroy(sandbox->self);
    }
    if (sandbox) memset(sandbox, 0, sizeof(*sandbox));
}

/* 学习注释：create_llm 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：build_tools 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
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

/* 学习注释：cc_runtime_builder_create 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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

/* 学习注释：cc_runtime_builder_runtime 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
cc_agent_runtime_t *cc_runtime_builder_runtime(cc_runtime_builder_t *builder)
{
    return builder ? builder->runtime : NULL;
}

cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder)
{
    return builder ? builder->logger : NULL;
}

/* 学习注释：cc_runtime_builder_destroy 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
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
