/**
 * 学习导读：apps/esp32/esp32_s3_qemu/main/cc_esp32_s3_qemu_features.c
 *
 * 所属层次：ESP32 应用层。
 * 阅读重点：这里展示设备 profile 的能力裁剪和 QEMU 示例，阅读时重点看哪些桌面能力被禁用或替换。
 * 注释说明：本文件的中文注释用于帮助理解当前实现；如果注释与代码冲突，
 *           以代码行为和测试为准，并应同步修正注释。
 */

#include "cc/app/cc_app_features.h"
#include "cc/ports/cc_memory_tool_factory.h"
#include "cc/ports/cc_storage_factory.h"

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
extern cc_result_t cc_esp32_gpio_tool_create(cc_tool_t *out_tool);
#if CC_LLM_OPENAI
extern cc_result_t cc_openai_provider_create(
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

/* 学习注释：create_gpio_tool 是本文件内部辅助函数。
 * 阅读时先看它服务哪个 public API，再看它如何处理边界条件和资源释放。 */
static cc_result_t create_gpio_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    (void)ctx;
    return cc_esp32_gpio_tool_create(out_tool);
}

static const cc_llm_provider_descriptor_t llm_providers[] = {
#if CC_LLM_OPENAI
    {"openai", CC_LLM_OPENAI, create_openai},
#else
    {"openai", 0, NULL},
#endif
    {"ollama", 0, NULL},
    {"anthropic", 0, NULL},
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
    {"memory", NULL, CC_HAS_MEMORY, create_memory_tool},
    {"gpio", "io", 1, create_gpio_tool},
};

static const cc_runtime_feature_set_t feature_set = {
    .llm_providers = llm_providers,
    .llm_provider_count = sizeof(llm_providers) / sizeof(llm_providers[0]),
    .tools = tools,
    .tool_count = sizeof(tools) / sizeof(tools[0]),
    .create_session_store = cc_storage_factory_create_store,
    .create_memory_store = create_memory_store,
    .create_policy_engine = create_policy,
    .create_sandbox = NULL,
    .load_plugins = NULL,
    .destroy_plugins = NULL,
};

/* 学习注释：cc_app_default_features 是对外可见或跨模块调用的入口。
 * 阅读时重点确认参数校验、所有权转移、错误码和清理路径是否成对出现。 */
const cc_runtime_feature_set_t *cc_app_default_features(void)
{
    return &feature_set;
}
