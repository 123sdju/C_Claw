

#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_skill_catalog.h"
#include "cc/app/cc_tool_executor_pool.h"
#include "cc_agent_runtime_internal.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_tool_registry.h"
#include "cc/util/cc_string_builder.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Runtime builder 是应用层装配器。
 *
 * 它拥有 logger/event_bus/provider/store/tool registry 等长期对象，真正的
 * cc_agent_runtime_t 只借用这些端口视图。这个分层是 C 语言里的依赖注入模式：
 * core runtime 不知道具体实现，只依赖 vtable；builder 负责选择 feature set 中的
 * adapter/factory，并在销毁时按反向顺序释放资源。
 *
 * retired_* 数组用于热重载：新 registry 生效后，旧 generation 仍可能被正在执行的 run
 * 引用，因此先放入退休列表，到 builder destroy 时统一释放，避免悬空指针。
 */
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

    cc_tool_executor_pool_t *tool_pool;

    cc_run_queue_t *run_queue;

    cc_agent_manager_t *agent_manager;

    cc_skill_catalog_t *skill_catalog;

    cc_agent_runtime_t *runtime;

    char *system_prompt;

    void *plugin_state;

    void *mcp_state;

    cc_runtime_diagnostics_t diagnostics;
    cc_tool_registry_t **retired_registries;
    void **retired_plugin_states;
    void **retired_mcp_states;
    cc_tool_executor_pool_t **retired_tool_pools;
    char **retired_runtime_prompts;
    char **retired_runtime_active_categories;
    size_t retired_count;
    unsigned long reload_generation;
};

/* 初始化 reload report，generation 默认保持不变，后续成功或失败路径再填具体状态。 */
static void reload_report_init(
    cc_runtime_reload_report_t *report,
    unsigned long generation
)
{
    if (!report) return;
    memset(report, 0, sizeof(*report));
    report->old_generation = generation;
    report->new_generation = generation;
    cc_runtime_diagnostics_reset(&report->diagnostics);
}

/*
 * 记录 reload 失败的组件名和错误信息。
 *
 * 这里不接管 rc 的所有权，只复制可读 message 到固定缓冲，方便上层在 rc 释放后仍能
 * 查看失败阶段。固定缓冲也避免嵌入式环境里 reload report 额外分配内存。
 */
static void reload_report_fail(
    cc_runtime_reload_report_t *report,
    const char *component,
    const cc_result_t *rc
)
{
    if (!report) return;
    report->rolled_back = 1;
    snprintf(report->failed_component, sizeof(report->failed_component),
        "%s", component ? component : "unknown");
    snprintf(report->message, sizeof(report->message),
        "%s", (rc && rc->message) ? rc->message : "Runtime reload failed");
}

/*
 * 判断配置是否启用某个内置工具。
 *
 * enabled_tools 为空表示“默认全开”；同时支持工具正式 name 和兼容 alias，便于配置文件
 * 使用更短的命名。这个 helper 不拥有任何字符串，只做只读匹配。
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

/*
 * 销毁尚未交给 registry 持有的 tool。
 *
 * 工具创建后如果注册失败，registry 不会接管 self；此时必须调用 tool vtable destroy。
 * 注册成功后 register_created_tool 会清空结构，避免重复销毁。
 */
static void destroy_tool_if_unowned(cc_tool_t *tool)
{
    if (tool && tool->vtable && tool->vtable->destroy && tool->self) {
        tool->vtable->destroy(tool->self);
    }
    if (tool) memset(tool, 0, sizeof(*tool));
}

/*
 * 将 factory 创建出的工具加入 registry。
 *
 * 成功后 registry 深拷贝/接管 tool 端口语义，当前栈上 tool 被清零；失败时销毁未转交的
 * tool self，保证错误路径不泄漏 adapter 私有状态。
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

/*
 * 销毁 builder 持有的 sandbox 端口。
 *
 * sandbox 是可选能力，只有 self/vtable/destroy 同时存在才调用；随后清零，避免 destroy
 * 阶段因为多个失败路径重复释放。
 */
static void destroy_sandbox_if_owned(cc_sandbox_t *sandbox)
{
    if (sandbox && sandbox->vtable && sandbox->vtable->destroy && sandbox->self) {
        sandbox->vtable->destroy(sandbox->self);
    }
    if (sandbox) memset(sandbox, 0, sizeof(*sandbox));
}

/*
 * 根据配置 provider 名称从 feature set 创建 LLM provider。
 *
 * 这是典型工厂模式：核心只认识 cc_llm_provider_t 接口，具体 OpenAI/Ollama/Anthropic
 * 等实现由 feature descriptor 决定。未知或编译关闭的 provider 直接返回配置错误。
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

/*
 * 为当前配置构建一个冻结的工具注册表。
 *
 * 注册顺序是内置工具 -> plugin 工具 -> MCP 工具。plugin_state/mcp_state 由对应 loader
 * 分配，成功后交给 builder 持有；任一阶段失败都销毁已创建资源。冻结 registry 后，
 * 运行中的 agent 只能查询，不能再修改，这让多线程读工具 schema 更容易推理。
 */
static cc_result_t build_tool_registry_for_config(
    cc_runtime_builder_t *builder,
    const cc_config_t *config,
    cc_tool_registry_t **out_registry,
    void **out_plugin_state,
    void **out_mcp_state,
    cc_runtime_diagnostics_t *diagnostics
)
{
    if (out_registry) *out_registry = NULL;
    if (out_plugin_state) *out_plugin_state = NULL;
    if (out_mcp_state) *out_mcp_state = NULL;

    cc_tool_registry_t *registry = NULL;
    void *plugin_state = NULL;
    void *mcp_state = NULL;
    cc_result_t rc = cc_tool_registry_create(&registry);
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
        rc = register_created_tool(registry, &tool);
        if (rc.code != CC_OK) {
            cc_tool_registry_destroy(registry);
            return rc;
        }
    }

    if (builder->features->load_plugins) {
        rc = builder->features->load_plugins(
            config, registry, &plugin_state, diagnostics);
        if (rc.code != CC_OK) {
            cc_tool_registry_destroy(registry);
            if (builder->features->destroy_plugins) builder->features->destroy_plugins(plugin_state);
            return rc;
        }
    }

    if (builder->features->load_mcp) {
        rc = builder->features->load_mcp(
            config, registry, &mcp_state, diagnostics);
        if (rc.code != CC_OK) {
            cc_tool_registry_destroy(registry);
            if (builder->features->destroy_plugins) builder->features->destroy_plugins(plugin_state);
            if (builder->features->destroy_mcp) builder->features->destroy_mcp(mcp_state);
            return rc;
        }
    }

    rc = cc_tool_registry_freeze(registry);
    if (rc.code != CC_OK) {
        cc_tool_registry_destroy(registry);
        if (builder->features->destroy_plugins) builder->features->destroy_plugins(plugin_state);
        if (builder->features->destroy_mcp) builder->features->destroy_mcp(mcp_state);
        return rc;
    }

    *out_registry = registry;
    if (out_plugin_state) *out_plugin_state = plugin_state;
    if (out_mcp_state) *out_mcp_state = mcp_state;
    return cc_result_ok();
}

/* 初次启动时构建 builder 当前 generation 的工具 registry 和扩展状态。 */
static cc_result_t build_tools(cc_runtime_builder_t *builder, const cc_config_t *config)
{
    return build_tool_registry_for_config(
        builder,
        config,
        &builder->tool_registry,
        &builder->plugin_state,
        &builder->mcp_state,
        &builder->diagnostics
    );
}

/*
 * 按配置创建工具执行池。
 *
 * tool pool 把不同来源的工具映射到 lane：普通工具使用 tools.policies，plugin/MCP 根据
 * entry/server 生成专属 lane。这样高延迟外部工具不会占满核心执行通道，是嵌入式/边缘
 * 设备里控制并发和超时的关键设计点。
 */
static cc_result_t create_tool_pool_from_config(
    const cc_config_t *config,
    cc_tool_executor_pool_t **out_pool
)
{
    if (!out_pool) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null tool pool output");
    *out_pool = NULL;
#if CC_ENABLE_TOOL_POOL
    cc_tool_executor_pool_config_t pool_config = cc_tool_executor_pool_default_config();
    pool_config.default_timeout_ms = config->tools.default_timeout_ms > 0 ?
        config->tools.default_timeout_ms : pool_config.default_timeout_ms;

    size_t policy_count = config->tools.policy_count +
        config->plugins.entry_count + config->mcp.server_count;
    cc_tool_executor_pool_policy_t *policies = NULL;
    if (policy_count > 0) {
        policies = calloc(policy_count, sizeof(*policies));
        if (!policies) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool pool policies");
    }

    size_t written = 0;
    for (size_t i = 0; i < config->tools.policy_count; i++) {
        policies[written].name = config->tools.policies[i].name;
        policies[written].concurrency = config->tools.policies[i].concurrency;
        policies[written].timeout_ms = config->tools.policies[i].timeout_ms;
        written++;
    }
    for (size_t i = 0; i < config->plugins.entry_count; i++) {
        const cc_config_plugin_entry_t *entry = &config->plugins.entries[i];
        if (!entry->id || !entry->enabled) continue;
        char lane[256];
        snprintf(lane, sizeof(lane), "plugin.%s", entry->id);
        policies[written].name = strdup(lane);
        policies[written].concurrency = entry->max_in_flight > 0 ?
            entry->max_in_flight : (entry->workers > 0 ? entry->workers : config->queue.plugin_concurrency);
        policies[written].timeout_ms = entry->timeout_ms;
        written++;
    }
    for (size_t i = 0; i < config->mcp.server_count; i++) {
        const cc_config_mcp_server_t *server = &config->mcp.servers[i];
        if (!server->name) continue;
        char lane[256];
        snprintf(lane, sizeof(lane), "mcp.%s", server->name);
        policies[written].name = strdup(lane);
        policies[written].concurrency = config->queue.mcp_concurrency;
        policies[written].timeout_ms = server->connection_timeout_ms;
        written++;
    }
    pool_config.policies = policies;
    pool_config.policy_count = written;

    cc_result_t rc = cc_tool_executor_pool_create(&pool_config, out_pool);
    for (size_t i = config->tools.policy_count; i < written; i++) {
        free((char *)policies[i].name);
    }
    free(policies);
    return rc;
#else
    (void)config;
    (void)out_pool;
    return cc_result_ok();
#endif
}

/* 初次启动时创建 builder 持有的工具执行池；编译关闭 tool pool 时为 no-op。 */
static cc_result_t build_tool_pool(cc_runtime_builder_t *builder, const cc_config_t *config)
{
    return create_tool_pool_from_config(config, &builder->tool_pool);
}

/*
 * 创建 run queue。
 *
 * run queue 只在 multi-agent 和 run-queue profile 同时启用时存在；小型 MCU profile
 * 可以完全裁剪掉这层，runtime 仍保持同步执行能力。
 */
static cc_result_t build_run_queue(cc_runtime_builder_t *builder, const cc_config_t *config)
{
#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
    cc_run_queue_config_t queue_config = cc_run_queue_default_config();
    queue_config.main_concurrency = config->queue.main_concurrency;
    queue_config.subagent_concurrency = config->queue.subagent_concurrency;
    queue_config.plugin_concurrency = config->queue.plugin_concurrency;
    queue_config.mcp_concurrency = config->queue.mcp_concurrency;
    queue_config.per_session_concurrency = config->queue.per_session_concurrency;
    queue_config.max_pending_per_session = config->queue.max_pending_per_session;
    return cc_run_queue_create(&queue_config, &builder->run_queue);
#else
    (void)builder;
    (void)config;
    return cc_result_ok();
#endif
}

#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
/* 将配置字符串映射成 run queue 行为枚举，未知值按 steer 处理保证默认可用。 */
static cc_run_queue_action_t queue_action_from_config(const char *mode)
{
    if (!mode || strcmp(mode, "steer") == 0) return CC_RUN_QUEUE_ACTION_STEER;
    if (strcmp(mode, "followup") == 0) return CC_RUN_QUEUE_ACTION_FOLLOWUP;
    if (strcmp(mode, "collect") == 0) return CC_RUN_QUEUE_ACTION_COLLECT;
    if (strcmp(mode, "interrupt") == 0) return CC_RUN_QUEUE_ACTION_INTERRUPT;
    return CC_RUN_QUEUE_ACTION_STEER;
}
#endif

/*
 * 构建 system prompt 和 skill catalog 快照。
 *
 * system_prompt 是传给 runtime 的配置快照，builder 和 runtime 各自持有自己的指针；
 * skills 启用时会把 allowlist 内技能拼接进 prompt。失败路径释放 catalog/prompt，
 * 避免 reload 中构建失败污染旧 generation。
 */
static cc_result_t build_system_prompt_snapshot(
    cc_runtime_builder_t *builder,
    const cc_config_t *config,
    char **out_system_prompt,
    cc_skill_catalog_t **out_skill_catalog
)
{
    if (!out_system_prompt || !out_skill_catalog) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid system prompt snapshot output");
    }
    *out_system_prompt = NULL;
    *out_skill_catalog = NULL;

    char *system_prompt = NULL;
    cc_result_t rc = cc_config_build_system_prompt(config, &system_prompt);
    if (rc.code != CC_OK || !system_prompt) {
        cc_result_free(&rc);
        system_prompt = strdup("You are a helpful AI assistant. Use tools to help the user.");
        if (!system_prompt) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to create default system prompt");
        }
    }

#if CC_ENABLE_SKILLS
    cc_skill_catalog_t *catalog = NULL;
    rc = cc_skill_catalog_create(&catalog);
    if (rc.code != CC_OK) {
        free(system_prompt);
        return rc;
    }
    rc = cc_skill_catalog_load_from_config(catalog, &builder->fs, config);
    if (rc.code != CC_OK) {
        free(system_prompt);
        cc_skill_catalog_destroy(catalog);
        return rc;
    }

    const cc_config_string_list_t *allowlist = &config->agents.defaults.skills;
    char *skill_prompt = NULL;
    rc = cc_skill_catalog_build_prompt(catalog, allowlist, &skill_prompt);
    if (rc.code != CC_OK) {
        free(system_prompt);
        cc_skill_catalog_destroy(catalog);
        return rc;
    }
    if (skill_prompt && skill_prompt[0]) {
        cc_string_builder_t sb;
        cc_string_builder_init(&sb);
        cc_string_builder_append(&sb, system_prompt);
        cc_string_builder_append(&sb, skill_prompt);
        char *joined = cc_string_builder_take(&sb);
        if (!joined) {
            free(skill_prompt);
            free(system_prompt);
            cc_skill_catalog_destroy(catalog);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to append skills to system prompt");
        }
        free(system_prompt);
        system_prompt = joined;
    }
    free(skill_prompt);
    *out_skill_catalog = catalog;
#endif

    *out_system_prompt = system_prompt;
    return cc_result_ok();
}

/*
 * 把旧 generation 的资源放入退休列表。
 *
 * reload 成功切换指针前，旧 registry/tool pool/prompt 仍可能被尚未结束的 run 使用。
 * 这里不立刻销毁，而是转移到 retired_* 数组；builder 最终销毁时统一释放。这是 C 中
 * 简化版的“generation based lifetime”策略。
 */
static cc_result_t retire_generation(
    cc_runtime_builder_t *builder,
    cc_tool_registry_t *registry,
    void *plugin_state,
    void *mcp_state,
    cc_tool_executor_pool_t *tool_pool,
    char *runtime_prompt,
    char *runtime_active_category
)
{
    size_t next_count = builder->retired_count + 1;
    cc_tool_registry_t **registries = calloc(next_count, sizeof(*registries));
    void **plugin_states = calloc(next_count, sizeof(*plugin_states));
    void **mcp_states = calloc(next_count, sizeof(*mcp_states));
    cc_tool_executor_pool_t **tool_pools = calloc(next_count, sizeof(*tool_pools));
    char **runtime_prompts = calloc(next_count, sizeof(*runtime_prompts));
    char **runtime_active_categories = calloc(next_count, sizeof(*runtime_active_categories));
    if (!registries || !plugin_states || !mcp_states || !tool_pools ||
        !runtime_prompts || !runtime_active_categories) {
        free(registries);
        free(plugin_states);
        free(mcp_states);
        free(tool_pools);
        free(runtime_prompts);
        free(runtime_active_categories);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to retain old runtime generation");
    }
    for (size_t i = 0; i < builder->retired_count; i++) {
        registries[i] = builder->retired_registries[i];
        plugin_states[i] = builder->retired_plugin_states[i];
        mcp_states[i] = builder->retired_mcp_states[i];
        tool_pools[i] = builder->retired_tool_pools[i];
        runtime_prompts[i] = builder->retired_runtime_prompts[i];
        runtime_active_categories[i] = builder->retired_runtime_active_categories[i];
    }
    free(builder->retired_registries);
    free(builder->retired_plugin_states);
    free(builder->retired_mcp_states);
    free(builder->retired_tool_pools);
    free(builder->retired_runtime_prompts);
    free(builder->retired_runtime_active_categories);
    builder->retired_registries = registries;
    builder->retired_plugin_states = plugin_states;
    builder->retired_mcp_states = mcp_states;
    builder->retired_tool_pools = tool_pools;
    builder->retired_runtime_prompts = runtime_prompts;
    builder->retired_runtime_active_categories = runtime_active_categories;
    size_t index = builder->retired_count++;
    builder->retired_registries[index] = registry;
    builder->retired_plugin_states[index] = plugin_state;
    builder->retired_mcp_states[index] = mcp_state;
    builder->retired_tool_pools[index] = tool_pool;
    builder->retired_runtime_prompts[index] = runtime_prompt;
    builder->retired_runtime_active_categories[index] = runtime_active_category;
    return cc_result_ok();
}

/*
 * 销毁所有退休 generation。
 *
 * 该函数只在 builder destroy 阶段调用，说明此时上层已经不再提交 run。释放顺序和持有
 * 顺序对应：先 tool pool/registry，再 plugin/MCP 状态，最后 prompt 字符串和数组。
 */
static void destroy_retired_generations(cc_runtime_builder_t *builder)
{
    if (!builder) return;
    for (size_t i = 0; i < builder->retired_count; i++) {
#if CC_ENABLE_TOOL_POOL
        cc_tool_executor_pool_destroy(builder->retired_tool_pools[i]);
#endif
        cc_tool_registry_destroy(builder->retired_registries[i]);
        if (builder->features && builder->features->destroy_plugins) {
            builder->features->destroy_plugins(builder->retired_plugin_states[i]);
        }
        if (builder->features && builder->features->destroy_mcp) {
            builder->features->destroy_mcp(builder->retired_mcp_states[i]);
        }
        free(builder->retired_runtime_prompts[i]);
        free(builder->retired_runtime_active_categories[i]);
    }
    free(builder->retired_registries);
    free(builder->retired_plugin_states);
    free(builder->retired_mcp_states);
    free(builder->retired_tool_pools);
    free(builder->retired_runtime_prompts);
    free(builder->retired_runtime_active_categories);
    builder->retired_registries = NULL;
    builder->retired_plugin_states = NULL;
    builder->retired_mcp_states = NULL;
    builder->retired_tool_pools = NULL;
    builder->retired_runtime_prompts = NULL;
    builder->retired_runtime_active_categories = NULL;
    builder->retired_count = 0;
}

/*
 * 创建完整 runtime builder。
 *
 * 该入口把配置、feature set 和端口工厂装配成一个可运行 SDK 实例。成功后 out_builder
 * 拥有返回对象；失败路径跳到统一 destroy，释放已创建组件。面试里可以把它解释为
 * “组合根”：所有依赖都在这里创建，业务核心通过接口使用它们。
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
    builder->reload_generation = 1;
    cc_runtime_diagnostics_reset(&builder->diagnostics);

    cc_result_t rc = cc_logger_create("c-claw", CC_LOG_INFO, &builder->logger);
    if (rc.code != CC_OK) goto fail;
    cc_logger_log(builder->logger, CC_LOG_INFO, "c-claw starting...");

    cc_event_bus_config_t event_bus_config = cc_event_bus_default_config();
    event_bus_config.mode = CC_EVENT_BUS_MODE_ASYNC;
    rc = cc_event_bus_create_with_config(&event_bus_config, &builder->event_bus);
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
    rc = build_tool_pool(builder, config);
    if (rc.code != CC_OK) goto fail;
    rc = build_system_prompt_snapshot(
        builder, config, &builder->system_prompt, &builder->skill_catalog);
    if (rc.code != CC_OK) goto fail;

    cc_agent_runtime_config_t runtime_config = {0};
    runtime_config.max_steps = config->max_steps ? config->max_steps : 10;
    runtime_config.context_window_tokens = config->context_window_tokens;
    runtime_config.context_compress_threshold = config->context_compress_threshold;
    runtime_config.context_keep_recent = config->context_keep_recent;
    runtime_config.max_tokens = config->max_tokens;
    runtime_config.temperature = config->temperature;
    runtime_config.summary_max_tokens = config->summary_max_tokens;
    runtime_config.summary_temperature = config->summary_temperature;
    runtime_config.multimodal = config->multimodal;
    runtime_config.active_memory_enabled = config->active_memory_enabled;
    runtime_config.active_memory_write_summary = config->active_memory_write_summary;
    runtime_config.active_memory_max_value_chars = config->active_memory_max_value_chars;
    runtime_config.active_memory_category = config->active_memory_category;
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
    deps.tool_pool = builder->tool_pool;

    cc_agent_runtime_options_t options = {0};
    options.config = runtime_config;
    options.thinking_mode = config->thinking_mode;
    rc = cc_agent_runtime_create(&deps, &options, &builder->runtime);
    if (rc.code != CC_OK) goto fail;

    rc = build_run_queue(builder, config);
    if (rc.code != CC_OK) goto fail;
#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
    cc_agent_manager_options_t manager_options = {0};
    manager_options.default_runtime = builder->runtime;
    manager_options.queue = builder->run_queue;
    manager_options.owns_queue = 0;
    manager_options.default_action = queue_action_from_config(config->queue.mode);
    manager_options.default_agent_id =
        (config->agents.defaults.id && strcmp(config->agents.defaults.id, "defaults") != 0) ?
            config->agents.defaults.id : "default";
    rc = cc_agent_manager_create(&manager_options, &builder->agent_manager);
    if (rc.code != CC_OK) goto fail;
#endif

    *out_builder = builder;
    return cc_result_ok();

fail:
    cc_runtime_builder_destroy(builder);
    return rc;
}

/*
 * 返回 builder 当前 runtime 的借用指针。
 *
 * 调用方不能销毁该指针；runtime 生命周期由 builder 管理，reload 只替换 runtime 内部
 * registry/prompt 等快照，不替换 runtime 对象本身。
 */
cc_agent_runtime_t *cc_runtime_builder_runtime(cc_runtime_builder_t *builder)
{
    return builder ? builder->runtime : NULL;
}

/*
 * 返回 agent manager 的借用指针。
 *
 * 只有 multi-agent/run-queue profile 才会创建 manager；裁剪 profile 返回 NULL，调用方
 * 需要按能力判断降级到单 runtime 执行。
 */
cc_agent_manager_t *cc_runtime_builder_agent_manager(cc_runtime_builder_t *builder)
{
    return builder ? builder->agent_manager : NULL;
}

/*
 * 返回最近一次启动或 reload 的诊断信息借用指针。
 *
 * diagnostics 由 builder 持有，不需要释放；plugin/MCP loader 可把非致命问题写入这里，
 * 让应用展示“部分能力不可用”而不是直接启动失败。
 */
const cc_runtime_diagnostics_t *cc_runtime_builder_diagnostics(cc_runtime_builder_t *builder)
{
    return builder ? &builder->diagnostics : NULL;
}

/* 简化 reload 入口：不需要详细 report 时调用，内部仍走同一套事务式 reload 实现。 */
cc_result_t cc_runtime_builder_reload(
    cc_runtime_builder_t *builder,
    const cc_config_t *config
)
{
    return cc_runtime_builder_reload_with_report(builder, config, NULL);
}

/*
 * 事务式热重载工具、技能和执行池。
 *
 * 新资源全部创建成功后才切换到 runtime；任何阶段失败都会销毁新资源并保留旧 generation。
 * 切换时把旧资源 retire，保证正在执行的 run 不会读到已释放的 registry/prompt。这种
 * “先构建、后提交、失败回滚”的思路在嵌入式配置热更新中很常见。
 */
cc_result_t cc_runtime_builder_reload_with_report(
    cc_runtime_builder_t *builder,
    const cc_config_t *config,
    cc_runtime_reload_report_t *out_report
)
{
    if (!builder || !config) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid runtime reload request");
    }
    reload_report_init(out_report, builder->reload_generation);

    cc_tool_registry_t *new_registry = NULL;
    void *new_plugin_state = NULL;
    void *new_mcp_state = NULL;
    cc_tool_executor_pool_t *new_tool_pool = NULL;
    char *new_system_prompt = NULL;
    char *new_runtime_prompt = NULL;
    char *new_runtime_active_category = NULL;
    cc_skill_catalog_t *new_skill_catalog = NULL;
    cc_runtime_diagnostics_t new_diagnostics;
    cc_runtime_diagnostics_reset(&new_diagnostics);

    cc_result_t rc = build_tool_registry_for_config(
        builder, config, &new_registry, &new_plugin_state, &new_mcp_state,
        &new_diagnostics);
    if (rc.code != CC_OK) {
        reload_report_fail(out_report, "tools", &rc);
        return rc;
    }

    rc = create_tool_pool_from_config(config, &new_tool_pool);
    if (rc.code != CC_OK) {
        cc_tool_registry_destroy(new_registry);
        if (builder->features && builder->features->destroy_plugins) {
            builder->features->destroy_plugins(new_plugin_state);
        }
        if (builder->features && builder->features->destroy_mcp) {
            builder->features->destroy_mcp(new_mcp_state);
        }
        reload_report_fail(out_report, "tool_pool", &rc);
        return rc;
    }
    rc = build_system_prompt_snapshot(
        builder, config, &new_system_prompt, &new_skill_catalog);
    if (rc.code != CC_OK) {
        cc_tool_registry_destroy(new_registry);
        if (builder->features && builder->features->destroy_plugins) {
            builder->features->destroy_plugins(new_plugin_state);
        }
        if (builder->features && builder->features->destroy_mcp) {
            builder->features->destroy_mcp(new_mcp_state);
        }
#if CC_ENABLE_TOOL_POOL
        cc_tool_executor_pool_destroy(new_tool_pool);
#endif
        reload_report_fail(out_report, "skills", &rc);
        return rc;
    }
    new_runtime_prompt = strdup(new_system_prompt ? new_system_prompt : "");
    new_runtime_active_category = strdup(
        config->active_memory_category ? config->active_memory_category : "active_summary");
    if (!new_runtime_prompt || !new_runtime_active_category) {
        free(new_system_prompt);
        free(new_runtime_prompt);
        free(new_runtime_active_category);
#if CC_ENABLE_SKILLS
        cc_skill_catalog_destroy(new_skill_catalog);
#endif
        cc_tool_registry_destroy(new_registry);
        if (builder->features && builder->features->destroy_plugins) {
            builder->features->destroy_plugins(new_plugin_state);
        }
        if (builder->features && builder->features->destroy_mcp) {
            builder->features->destroy_mcp(new_mcp_state);
        }
#if CC_ENABLE_TOOL_POOL
        cc_tool_executor_pool_destroy(new_tool_pool);
#endif
        rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy runtime reload config");
        reload_report_fail(out_report, "runtime_config", &rc);
        return rc;
    }

    cc_tool_registry_t *old_registry = builder->tool_registry;
    void *old_plugin_state = builder->plugin_state;
    void *old_mcp_state = builder->mcp_state;
    cc_tool_executor_pool_t *old_tool_pool = builder->tool_pool;
    char *old_runtime_prompt = builder->runtime ? builder->runtime->config.system_prompt : NULL;
    char *old_runtime_active_category = builder->runtime ?
        builder->runtime->config.active_memory_category : NULL;

    rc = retire_generation(
        builder, old_registry, old_plugin_state, old_mcp_state, old_tool_pool,
        old_runtime_prompt, old_runtime_active_category);
    if (rc.code != CC_OK) {
        free(new_system_prompt);
        free(new_runtime_prompt);
        free(new_runtime_active_category);
#if CC_ENABLE_SKILLS
        cc_skill_catalog_destroy(new_skill_catalog);
#endif
        cc_tool_registry_destroy(new_registry);
        if (builder->features && builder->features->destroy_plugins) {
            builder->features->destroy_plugins(new_plugin_state);
        }
        if (builder->features && builder->features->destroy_mcp) {
            builder->features->destroy_mcp(new_mcp_state);
        }
#if CC_ENABLE_TOOL_POOL
        cc_tool_executor_pool_destroy(new_tool_pool);
#endif
        reload_report_fail(out_report, "retire_generation", &rc);
        return rc;
    }

    builder->tool_registry = new_registry;
    builder->plugin_state = new_plugin_state;
    builder->mcp_state = new_mcp_state;
    builder->tool_pool = new_tool_pool;
    builder->diagnostics = new_diagnostics;
    free(builder->system_prompt);
    builder->system_prompt = new_system_prompt;
#if CC_ENABLE_SKILLS
    cc_skill_catalog_destroy(builder->skill_catalog);
    builder->skill_catalog = new_skill_catalog;
#endif
    if (builder->runtime) {
        cc_mutex_lock(builder->runtime->mutex);
        builder->runtime->tool_registry = new_registry;
        builder->runtime->tool_pool = new_tool_pool;
        builder->runtime->services.tool_pool = new_tool_pool;
        builder->runtime->config.system_prompt = new_runtime_prompt;
        builder->runtime->config.active_memory_enabled = config->active_memory_enabled;
        builder->runtime->config.active_memory_write_summary = config->active_memory_write_summary;
        builder->runtime->config.active_memory_max_value_chars = config->active_memory_max_value_chars;
        builder->runtime->config.active_memory_category = new_runtime_active_category;
        cc_mutex_unlock(builder->runtime->mutex);
    } else {
        free(new_runtime_prompt);
        free(new_runtime_active_category);
    }

    if (builder->logger) {
        cc_logger_log(builder->logger, CC_LOG_INFO,
            "Runtime reload completed");
    }
    if (out_report) {
        out_report->tools_reloaded = 1;
        out_report->plugins_reloaded = new_plugin_state != NULL;
        out_report->mcp_reloaded = new_mcp_state != NULL;
        out_report->skills_reloaded = new_skill_catalog != NULL;
        out_report->tool_pool_reloaded = new_tool_pool != NULL;
        out_report->old_generation = builder->reload_generation;
        out_report->new_generation = builder->reload_generation + 1;
        out_report->diagnostics = new_diagnostics;
    }
    builder->reload_generation++;
    return cc_result_ok();
}

/*
 * 请求关闭 runtime。
 *
 * 当前实现只记录日志；异步 run queue 的实际 worker 销毁发生在 destroy 中。保留这个
 * API 是为了未来接入更细的 stop/drain 流程时不改变上层调用点。
 */
void cc_runtime_builder_request_shutdown(cc_runtime_builder_t *builder)
{
    if (!builder) return;
    if (builder->logger) {
        cc_logger_log(builder->logger, CC_LOG_INFO, "Runtime shutdown requested");
    }
}

/*
 * 返回 logger 的借用指针。
 *
 * 调用方可以临时写日志，但不能销毁；logger 的脱敏策略和线程安全由 logger 模块内部
 * 处理，builder destroy 时统一释放。
 */
cc_logger_t *cc_runtime_builder_logger(cc_runtime_builder_t *builder)
{
    return builder ? builder->logger : NULL;
}

/*
 * 销毁 builder 及其拥有的所有组件。
 *
 * 释放顺序按依赖关系反向执行：先停止队列/manager，再销毁 runtime，然后销毁 tool
 * pool、skills、registry、plugin/MCP、store/provider/policy/sandbox、event bus/logger。
 * 这种顺序能避免后销毁对象在析构期间访问已经失效的底层端口。
 */
void cc_runtime_builder_destroy(cc_runtime_builder_t *builder)
{
    if (!builder) return;
    if (builder->logger) cc_logger_log(builder->logger, CC_LOG_INFO, "Shutting down...");
    cc_runtime_builder_request_shutdown(builder);
#if CC_ENABLE_MULTI_AGENT && CC_ENABLE_RUN_QUEUE
    cc_run_queue_destroy(builder->run_queue);
    builder->run_queue = NULL;
    cc_agent_manager_destroy(builder->agent_manager);
    builder->agent_manager = NULL;
#endif
    cc_agent_runtime_destroy(builder->runtime);
#if CC_ENABLE_TOOL_POOL
    cc_tool_executor_pool_destroy(builder->tool_pool);
#endif
#if CC_ENABLE_SKILLS
    cc_skill_catalog_destroy(builder->skill_catalog);
#endif
    cc_tool_registry_destroy(builder->tool_registry);
    destroy_retired_generations(builder);
    if (builder->features && builder->features->destroy_plugins) {
        builder->features->destroy_plugins(builder->plugin_state);
    }
    if (builder->features && builder->features->destroy_mcp) {
        builder->features->destroy_mcp(builder->mcp_state);
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
