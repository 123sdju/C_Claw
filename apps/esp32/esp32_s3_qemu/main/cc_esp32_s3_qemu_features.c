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

/**
 * cc_policy_engine_create_default — 声明默认策略引擎工厂，ESP32 profile 用它创建工具审批策略。
 *
 * @param shell_requires_approval 非 0 表示 shell 类工具需要人工审批。
 * @param out_engine 输出策略引擎端口；成功后由 runtime_builder 销毁。
 * @return CC_OK 表示创建成功；失败返回策略适配层错误。
 */
extern cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);
#if CC_TOOL_FILE_READ
/**
 * cc_file_read_tool_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param fs 按值传入，用于控制本次操作。
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
#endif
#if CC_TOOL_FILE_WRITE
/**
 * cc_file_write_tool_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param fs 按值传入，用于控制本次操作。
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
#endif
#if CC_TOOL_HTTP_REQUEST
/**
 * cc_http_request_tool_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_http_request_tool_create(cc_tool_t *out_tool);
#endif
/**
 * cc_esp32_gpio_tool_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_esp32_gpio_tool_create(cc_tool_t *out_tool);
#if CC_LLM_OPENAI
/**
 * cc_openai_provider_create — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param model 借用的只读字符串；函数不会释放该指针。
 * @param out_provider 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_openai_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);
#endif

#if CC_LLM_OPENAI
/**
 * create_openai — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param config 只读配置对象；函数读取字段但不保存 config 指针。
 * @param out_provider 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_openai(const cc_config_t *config, cc_llm_provider_t *out_provider)
{
    return cc_openai_provider_create(config->base_url, config->api_key, config->model, out_provider);
}
#endif

/**
 * create_policy — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param config 只读配置对象；函数读取字段但不保存 config 指针。
 * @param out_policy 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_policy(const cc_config_t *config, cc_policy_engine_t *out_policy)
{
    return cc_policy_engine_create_default(config->shell_requires_approval, out_policy);
}

/**
 * create_memory_store — 根据 config.memory_backend/path 创建长期记忆存储，未启用时返回空端口。
 *
 * @param config 借用的只读配置；读取 memory_backend 与 memory_path。
 * @param out_store 输出 memory store 端口；成功启用时由 runtime_builder 销毁。
 * @return CC_OK 表示创建成功或当前 profile 明确禁用记忆；失败返回后端错误。
 */
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
/**
 * create_file_read_tool — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_file_read_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    return cc_file_read_tool_create(ctx->filesystem, out_tool);
}
#endif

#if CC_TOOL_FILE_WRITE
/**
 * create_file_write_tool — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_file_write_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    return cc_file_write_tool_create(ctx->filesystem, out_tool);
}
#endif

#if CC_TOOL_HTTP_REQUEST
/**
 * create_http_request_tool — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_http_request_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    (void)ctx;
    return cc_http_request_tool_create(out_tool);
}
#endif

/**
 * create_memory_tool — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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

/**
 * create_gpio_tool — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @param ctx 调用上下文；只在本次函数执行期间借用。
 * @param out_tool 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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

/**
 * cc_app_default_features — 返回当前应用 profile 的静态 feature set，供 runtime_builder 发现可编译能力。
 *
 * 位置：ESP32/QEMU 层。注释重点说明当前函数的输入输出、资源边界和错误传播。
 *
 * @return 静态只读 feature set 借用指针；调用方不得释放或修改。
 */
const cc_runtime_feature_set_t *cc_app_default_features(void)
{
    return &feature_set;
}
