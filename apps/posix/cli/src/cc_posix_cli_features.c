/**
 * 学习导读：apps/posix/cli/src/cc_posix_cli_features.c
 *
 * 所属层次：POSIX CLI 应用层。
 * 阅读重点：这里组装桌面 CLI、工具、插件和 sandbox，阅读时重点看 main 到 runtime builder 的组合流程。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/app/cc_app_features.h"
#include "cc/ports/cc_memory_tool_factory.h"
#include "cc/ports/cc_storage_factory.h"

#if CC_TOOL_PLUGIN
#include "cc/plugin/cc_plugin_manager.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);
#if CC_TOOL_FILE_READ
extern cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
#endif
#if CC_TOOL_FILE_WRITE
extern cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
#endif
#if CC_TOOL_HTTP_REQUEST
extern cc_result_t cc_http_request_tool_create(cc_tool_t *out_tool);
#endif
#if CC_TOOL_SHELL_RUN
extern cc_result_t cc_shell_run_tool_create(cc_sandbox_t sandbox, cc_tool_t *out_tool);
#endif
extern cc_result_t cc_local_sandbox_create(int timeout_ms, cc_sandbox_t *out_sandbox);
extern cc_result_t cc_docker_sandbox_create(int timeout_ms, cc_sandbox_t *out_sandbox);
#if CC_LLM_OPENAI
extern cc_result_t cc_openai_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);
#endif
#if CC_LLM_OLLAMA
extern cc_result_t cc_ollama_provider_create(
    const char *base_url,
    const char *model,
    cc_llm_provider_t *out_provider
);
#endif
#if CC_LLM_ANTHROPIC
extern cc_result_t cc_anthropic_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);
#endif

#if CC_LLM_OPENAI
/* 学习注释：create_openai 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_openai(const cc_config_t *config, cc_llm_provider_t *out_provider)
{
    return cc_openai_provider_create(config->base_url, config->api_key, config->model, out_provider);
}
#endif

#if CC_LLM_OLLAMA
/* 学习注释：create_ollama 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_ollama(const cc_config_t *config, cc_llm_provider_t *out_provider)
{
    return cc_ollama_provider_create(config->base_url, config->model, out_provider);
}
#endif

#if CC_LLM_ANTHROPIC
/* 学习注释：create_anthropic 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_anthropic(const cc_config_t *config, cc_llm_provider_t *out_provider)
{
    return cc_anthropic_provider_create(config->base_url, config->api_key, config->model, out_provider);
}
#endif

/* 学习注释：create_policy 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_policy(const cc_config_t *config, cc_policy_engine_t *out_policy)
{
    return cc_policy_engine_create_default(config->shell_requires_approval, out_policy);
}

static cc_result_t create_memory_store(const cc_config_t *config, cc_memory_store_t *out_store)
{
#if CC_HAS_MEMORY
    return cc_memory_store_factory_create(
        out_store,
        config->memory_backend ? config->memory_backend : "json_file",
        config->memory_path);
#else
    (void)config;
    (void)out_store;
    return cc_result_ok();
#endif
}

/* 学习注释：create_sandbox 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_sandbox(const cc_config_t *config, cc_sandbox_t *out_sandbox)
{
    memset(out_sandbox, 0, sizeof(*out_sandbox));
    const char *type = (config && config->sandbox_type) ? config->sandbox_type : "local";
    if (strcmp(type, "none") == 0) return cc_result_ok();
    if (strcmp(type, "local") == 0) {
#if CC_SANDBOX_LOCAL
        int timeout_ms = config ? config->sandbox_timeout_ms : 30000;
        return cc_local_sandbox_create(timeout_ms, out_sandbox);
#else
        return cc_result_error(CC_ERR_PLATFORM, "Local sandbox disabled in this build");
#endif
    }
    if (strcmp(type, "docker") == 0) {
#if CC_SANDBOX_DOCKER
        int timeout_ms = config ? config->sandbox_timeout_ms : 30000;
        return cc_docker_sandbox_create(timeout_ms, out_sandbox);
#else
        return cc_result_error(CC_ERR_PLATFORM, "Docker sandbox disabled in this build");
#endif
    }
    return cc_result_errf(CC_ERR_INVALID_ARGUMENT, "Unknown sandbox type: %s", type);
}

#if CC_TOOL_FILE_READ
/* 学习注释：create_file_read_tool 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_file_read_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    return cc_file_read_tool_create(ctx->filesystem, out_tool);
}
#endif

#if CC_TOOL_FILE_WRITE
/* 学习注释：create_file_write_tool 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_file_write_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    return cc_file_write_tool_create(ctx->filesystem, out_tool);
}
#endif

#if CC_TOOL_HTTP_REQUEST
/* 学习注释：create_http_request_tool 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_http_request_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    (void)ctx;
    return cc_http_request_tool_create(out_tool);
}
#endif

#if CC_TOOL_SHELL_RUN
/* 学习注释：create_shell_tool 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_shell_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    if (!ctx->create_sandbox) {
        return cc_result_error(CC_ERR_PLATFORM, "No sandbox factory registered");
    }
    cc_sandbox_t sandbox = {0};
    cc_result_t rc = ctx->create_sandbox(ctx->config, &sandbox);
    if (rc.code != CC_OK) return rc;
    if (!sandbox.vtable) {
        return cc_result_error(CC_ERR_PLATFORM, "shell_run requires a sandbox");
    }
    rc = cc_shell_run_tool_create(sandbox, out_tool);
    if (rc.code != CC_OK && sandbox.vtable && sandbox.vtable->destroy) {
        sandbox.vtable->destroy(sandbox.self);
    }
    return rc;
}
#endif

/* 学习注释：create_memory_tool 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_memory_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
#if CC_HAS_MEMORY
    if (!ctx->memory_store || !ctx->memory_store->self) return cc_result_ok();
    return cc_memory_tool_create(ctx->memory_store, out_tool);
#else
    (void)ctx;
    (void)out_tool;
    return cc_result_ok();
#endif
}

#if CC_TOOL_PLUGIN
/* 学习注释：read_file_content 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static char *read_file_content(const char *path)
{
    if (!path || !path[0]) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* 学习注释：load_plugins 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t load_plugins(
    const cc_config_t *config,
    cc_tool_registry_t *registry,
    void **out_state
)
{
    *out_state = NULL;
    cc_plugin_manager_t *manager = NULL;
    cc_result_t rc = cc_plugin_manager_create(&manager);
    if (rc.code != CC_OK) return rc;
    char *plugins_json = read_file_content(config ? config->plugin_config_path : NULL);
    if (plugins_json) {
        rc = cc_plugin_manager_load_plugins(manager, plugins_json, registry);
        free(plugins_json);
        if (rc.code != CC_OK) {
            cc_plugin_manager_destroy(manager);
            return rc;
        }
    }
    *out_state = manager;
    return cc_result_ok();
}

/* 学习注释：destroy_plugins 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static void destroy_plugins(void *state)
{
    cc_plugin_manager_destroy((cc_plugin_manager_t *)state);
}
#endif

static const cc_llm_provider_descriptor_t llm_providers[] = {
#if CC_LLM_OPENAI
    {"openai", CC_LLM_OPENAI, create_openai},
#else
    {"openai", 0, NULL},
#endif
#if CC_LLM_OLLAMA
    {"ollama", CC_LLM_OLLAMA, create_ollama},
#else
    {"ollama", 0, NULL},
#endif
#if CC_LLM_ANTHROPIC
    {"anthropic", CC_LLM_ANTHROPIC, create_anthropic},
#else
    {"anthropic", 0, NULL},
#endif
};

static const cc_tool_descriptor_t tools[] = {
#if CC_TOOL_FILE_READ
    {"file_read", "read", CC_TOOL_FILE_READ, create_file_read_tool},
#else
    {"file_read", "read", 0, NULL},
#endif
#if CC_TOOL_FILE_WRITE
    {"file_write", "write", CC_TOOL_FILE_WRITE, create_file_write_tool},
#else
    {"file_write", "write", 0, NULL},
#endif
#if CC_TOOL_HTTP_REQUEST
    {"http.request", "http", CC_TOOL_HTTP_REQUEST, create_http_request_tool},
#else
    {"http.request", "http", 0, NULL},
#endif
#if CC_TOOL_SHELL_RUN
    {"shell_run", "shell", CC_TOOL_SHELL_RUN, create_shell_tool},
#else
    {"shell_run", "shell", 0, NULL},
#endif
    {"memory", NULL, CC_HAS_MEMORY, create_memory_tool},
};

static const cc_runtime_feature_set_t feature_set = {
    .llm_providers = llm_providers,
    .llm_provider_count = sizeof(llm_providers) / sizeof(llm_providers[0]),
    .tools = tools,
    .tool_count = sizeof(tools) / sizeof(tools[0]),
    .create_session_store = cc_storage_factory_create_store,
    .create_memory_store = create_memory_store,
    .create_policy_engine = create_policy,
    .create_sandbox = create_sandbox,
#if CC_TOOL_PLUGIN
    .load_plugins = load_plugins,
    .destroy_plugins = destroy_plugins,
#endif
};

/* 学习注释：cc_app_default_features 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
const cc_runtime_feature_set_t *cc_app_default_features(void)
{
    return &feature_set;
}
