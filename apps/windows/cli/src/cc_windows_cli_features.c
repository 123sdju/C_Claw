/**
 * 学习导读：apps/windows/cli/src/cc_windows_cli_features.c
 *
 * 所属层次：Windows CLI 应用层。
 * 阅读重点：这里镜像桌面 CLI 能力但使用 Windows 平台实现，阅读时重点比较与 POSIX 版本的差异。
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

/**
 * cc_policy_engine_create_default — 外部策略引擎工厂；根据 shell 审批开关创建默认 policy 端口。
 *
 * @param shell_requires_approval 按值传入的参数，用于控制本次操作。
 * @param out_engine 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_policy_engine_create_default(
    int shell_requires_approval,
    cc_policy_engine_t *out_engine
);
#if CC_TOOL_FILE_READ
/**
 * cc_file_read_tool_create — 外部文件读取工具工厂；把文件系统端口注入 file_read 工具。
 *
 * @param fs 按值传入的参数，用于控制本次操作。
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_file_read_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
#endif
#if CC_TOOL_FILE_WRITE
/**
 * cc_file_write_tool_create — 外部文件写入工具工厂；把文件系统端口注入 file_write 工具。
 *
 * @param fs 按值传入的参数，用于控制本次操作。
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_file_write_tool_create(cc_filesystem_t fs, cc_tool_t *out_tool);
#endif
#if CC_TOOL_HTTP_REQUEST
/**
 * cc_http_request_tool_create — 外部 HTTP 请求工具工厂；创建可由 LLM 调用的 http.request 工具。
 *
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_http_request_tool_create(cc_tool_t *out_tool);
#endif
#if CC_TOOL_SHELL_RUN
/**
 * cc_shell_run_tool_create — 外部 shell 工具工厂；把 sandbox 端口注入 shell_run 工具。
 *
 * @param sandbox 借用的对象指针；函数不取得结构体本身所有权。
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_shell_run_tool_create(cc_sandbox_t sandbox, cc_tool_t *out_tool);
#endif
/**
 * cc_local_sandbox_create — 外部本地 sandbox 工厂；用 timeout_ms 创建受限本机执行环境。
 *
 * @param timeout_ms 按值传入的参数，用于控制本次操作。
 * @param out_sandbox 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_local_sandbox_create(int timeout_ms, cc_sandbox_t *out_sandbox);
/**
 * cc_docker_sandbox_create — 外部 Docker sandbox 工厂；用 timeout_ms 创建容器隔离执行环境。
 *
 * @param timeout_ms 按值传入的参数，用于控制本次操作。
 * @param out_sandbox 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_docker_sandbox_create(int timeout_ms, cc_sandbox_t *out_sandbox);
#if CC_LLM_OPENAI
/**
 * cc_openai_provider_create — 外部 OpenAI provider 工厂；复制 URL、API key 和 model 后创建 LLM 端口。
 *
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param model 借用的只读字符串；函数不会释放该指针。
 * @param out_provider 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_openai_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);
#endif
#if CC_LLM_OLLAMA
/**
 * cc_ollama_provider_create — 外部 Ollama provider 工厂；复制 URL 和 model 后创建本地 LLM 端口。
 *
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param model 借用的只读字符串；函数不会释放该指针。
 * @param out_provider 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_ollama_provider_create(
    const char *base_url,
    const char *model,
    cc_llm_provider_t *out_provider
);
#endif
#if CC_LLM_ANTHROPIC
/**
 * cc_anthropic_provider_create — 外部 Anthropic provider 工厂；复制 URL、API key 和 model 后创建 LLM 端口。
 *
 * @param base_url 借用的只读字符串；函数不会释放该指针。
 * @param api_key 借用的只读字符串；函数不会释放该指针。
 * @param model 借用的只读字符串；函数不会释放该指针。
 * @param out_provider 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
extern cc_result_t cc_anthropic_provider_create(
    const char *base_url,
    const char *api_key,
    const char *model,
    cc_llm_provider_t *out_provider
);
#endif

#if CC_LLM_OPENAI
/**
 * create_openai — 把 cc_config_t 中的 OpenAI 配置转交给 provider 工厂并写入 out_provider。
 *
 * @param config 借用的只读配置；函数只读取需要的字段。
 * @param out_provider 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_openai(const cc_config_t *config, cc_llm_provider_t *out_provider)
{
    return cc_openai_provider_create(config->base_url, config->api_key, config->model, out_provider);
}
#endif

#if CC_LLM_OLLAMA
/**
 * create_ollama — 把 cc_config_t 中的 Ollama 配置转交给 provider 工厂并写入 out_provider。
 *
 * @param config 借用的只读配置；函数只读取需要的字段。
 * @param out_provider 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_ollama(const cc_config_t *config, cc_llm_provider_t *out_provider)
{
    return cc_ollama_provider_create(config->base_url, config->model, out_provider);
}
#endif

#if CC_LLM_ANTHROPIC
/**
 * create_anthropic — 把 cc_config_t 中的 Anthropic 配置转交给 provider 工厂并写入 out_provider。
 *
 * @param config 借用的只读配置；函数只读取需要的字段。
 * @param out_provider 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_anthropic(const cc_config_t *config, cc_llm_provider_t *out_provider)
{
    return cc_anthropic_provider_create(config->base_url, config->api_key, config->model, out_provider);
}
#endif

/**
 * create_policy — 根据 config.shell_requires_approval 创建默认工具策略引擎。
 *
 * @param config 借用的只读配置；函数只读取需要的字段。
 * @param out_policy 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
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

/**
 * create_sandbox — 根据 config.sandbox_type 在 local/docker sandbox 工厂之间选择。
 *
 * @param config 借用的只读配置；函数只读取需要的字段。
 * @param out_sandbox 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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
/**
 * create_file_read_tool — 把 runtime_builder 提供的文件系统端口注入 file_read 工具。
 *
 * @param ctx 借用的上下文对象；函数只在调用期间使用。
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_file_read_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    return cc_file_read_tool_create(ctx->filesystem, out_tool);
}
#endif

#if CC_TOOL_FILE_WRITE
/**
 * create_file_write_tool — 把 runtime_builder 提供的文件系统端口注入 file_write 工具。
 *
 * @param ctx 借用的上下文对象；函数只在调用期间使用。
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_file_write_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    return cc_file_write_tool_create(ctx->filesystem, out_tool);
}
#endif

#if CC_TOOL_HTTP_REQUEST
/**
 * create_http_request_tool — 创建 http.request 工具并交给 runtime_builder 注册。
 *
 * @param ctx 借用的上下文对象；函数只在调用期间使用。
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
static cc_result_t create_http_request_tool(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    (void)ctx;
    return cc_http_request_tool_create(out_tool);
}
#endif

#if CC_TOOL_SHELL_RUN
/**
 * create_shell_tool — 按需创建 sandbox 并注入 shell_run 工具，注册失败时由调用方清理。
 *
 * @param ctx 借用的上下文对象；函数只在调用期间使用。
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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

/**
 * create_memory_tool — 在启用 memory store 时创建 memory 工具，未启用时返回空工具。
 *
 * @param ctx 借用的上下文对象；函数只在调用期间使用。
 * @param out_tool 输出参数；成功时由函数写入，失败时调用方不要使用未初始化内容。
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

#if CC_TOOL_PLUGIN
/**
 * read_file_content — 执行文件系统操作，并把平台错误转换为统一结果。
 *
 * @param path 借用的只读字符串；函数不会释放该指针。
 * @return 新分配字符串；返回 NULL 表示分配或输入校验失败，调用方负责 free。
 */
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

/**
 * load_plugins — 创建、启动或加载组件资源，并把错误统一传播给调用方。
 *
 * @param config 只读配置对象；函数读取字段但不保存 config 指针。
 * @param registry 借用的对象；函数不释放该对象本身。
 * @param out_state 输出参数；成功时写入有效结果，失败时保持为 NULL 或未定义状态。
 * @return CC_OK 表示成功；失败返回具体错误码，错误消息按 cc_result_t 约定释放。
 */
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

/**
 * destroy_plugins — 释放插件加载阶段创建的 plugin manager 状态。
 *
 * @param state 借用的指针参数；若函数需要长期保存内容，会在内部复制。
 * 无返回值；函数通过对象状态、输出参数或释放动作体现副作用。
 */
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

/**
 * cc_app_default_features — 返回当前应用 profile 的静态 feature set，供 runtime_builder 发现可编译能力。
 *
 * @return 静态只读 feature set 借用指针；调用方不得释放或修改。
 */
const cc_runtime_feature_set_t *cc_app_default_features(void)
{
    return &feature_set;
}
