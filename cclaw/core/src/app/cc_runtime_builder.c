/**
 * 学习导读：cclaw/core/src/app/cc_runtime_builder.c
 *
 * 所属层次：核心层。
 * 阅读重点：这里是 runtime 的组合根，重点看依赖注入、工具注册、snapshot 发布、
 *           diagnostics 收集，以及失败路径如何释放已创建资源。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

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
    /** builder 拥有的工具并发池；runtime 只借用该指针执行 acquire/release。 */
    cc_tool_executor_pool_t *tool_pool;
    /** 可选 run queue；启用多 Agent 时由 manager 借用，builder 负责销毁。 */
    cc_run_queue_t *run_queue;
    /** 可选多 Agent manager；gateway 可从这里进入，避免绕过 session 串行策略。 */
    cc_agent_manager_t *agent_manager;
    /** 可选 skill catalog；由 builder 拥有，system prompt 已持有注入后的快照文本。 */
    cc_skill_catalog_t *skill_catalog;
    /** builder 拥有的 Agent runtime；销毁时先释放 runtime，再释放其借用依赖。 */
    cc_agent_runtime_t *runtime;
    /** 为 runtime 构造出的系统提示词副本；runtime 创建时会再深拷贝。 */
    char *system_prompt;
    /** 插件加载器返回的不透明状态；由 features->destroy_plugins 释放。 */
    void *plugin_state;
    /** MCP 加载器返回的不透明状态；由 features->destroy_mcp 释放。 */
    void *mcp_state;
    /** 最近一次启动/reload 的非致命 tool/plugin/MCP 诊断。 */
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

/**
 * build_tool_pool — 从 config.json 构造工具并发池。
 *
 * 所有 lane 策略都在这里一次性展开成 core pool 可消费的扁平数组。pool_create
 * 会深拷贝 lane 名称，因此本函数的临时字符串在返回前即可释放。
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

static cc_result_t build_tool_pool(cc_runtime_builder_t *builder, const cc_config_t *config)
{
    return create_tool_pool_from_config(config, &builder->tool_pool);
}

/**
 * build_run_queue — 从 config.json 构造多 Agent run queue。
 *
 * run queue 是 core 可移植并发闸门。它不创建线程，只在调用线程进入 run
 * 前阻塞等待额度，因此 POSIX/Windows/ESP 都能共享同一套语义。
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

static cc_run_queue_action_t queue_action_from_config(const char *mode)
{
    if (!mode || strcmp(mode, "steer") == 0) return CC_RUN_QUEUE_ACTION_STEER;
    if (strcmp(mode, "followup") == 0) return CC_RUN_QUEUE_ACTION_FOLLOWUP;
    if (strcmp(mode, "collect") == 0) return CC_RUN_QUEUE_ACTION_COLLECT;
    if (strcmp(mode, "interrupt") == 0) return CC_RUN_QUEUE_ACTION_INTERRUPT;
    return CC_RUN_QUEUE_ACTION_STEER;
}

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

/**
 * destroy_retired_generations — 释放 reload 后暂存的旧工具 generation。
 *
 * reload 采用“先构建新 generation，全部成功后再 swap”的回滚模型。swap 后，
 * builder 不会立刻销毁旧 registry/plugin/MCP/pool，因为仍可能存在已经开始执行
 * 的 run 正在读取旧工具表或占用旧 lane。当前实现把旧 generation 保留到 builder
 * destroy；这让生命周期更保守，也避免热重载失败路径影响运行中的调用。
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

cc_agent_manager_t *cc_runtime_builder_agent_manager(cc_runtime_builder_t *builder)
{
    return builder ? builder->agent_manager : NULL;
}

const cc_runtime_diagnostics_t *cc_runtime_builder_diagnostics(cc_runtime_builder_t *builder)
{
    return builder ? &builder->diagnostics : NULL;
}

cc_result_t cc_runtime_builder_reload(
    cc_runtime_builder_t *builder,
    const cc_config_t *config
)
{
    return cc_runtime_builder_reload_with_report(builder, config, NULL);
}

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

void cc_runtime_builder_request_shutdown(cc_runtime_builder_t *builder)
{
    if (!builder) return;
    if (builder->logger) {
        cc_logger_log(builder->logger, CC_LOG_INFO, "Runtime shutdown requested");
    }
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
